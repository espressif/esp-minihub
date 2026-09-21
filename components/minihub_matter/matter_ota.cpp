/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_matter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstdlib>
#include <utility>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "minihub_core.h"
#include "sdkconfig.h"

#if CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED && CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER

#include <controller/CHIPDeviceControllerFactory.h>
#include <credentials/FabricTable.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_ota_bdx_sender.h>
#include <esp_matter_ota_provider.h>
#include "controller_data_model_provider.h"

#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <app/clusters/ota-provider/ota-provider-delegate.h>
#include <crypto/RandUtils.h>
#include <lib/core/OTAImageHeader.h>
#include <messaging/ExchangeMgr.h>
#include <protocols/bdx/BdxUri.h>
#include <protocols/interaction_model/StatusCode.h>
#include <protocols/Protocols.h>

using chip::ByteSpan;
using chip::CharSpan;
using chip::FabricIndex;
using chip::NodeId;
using chip::ScopedNodeId;
using chip::app::CommandHandler;
using chip::app::ConcreteCommandPath;
using chip::app::Clusters::OTAProviderDelegate;
using chip::bdx::TransferControlFlags;
using chip::Protocols::InteractionModel::Status;
using OTAQueryStatus = chip::app::Clusters::OtaSoftwareUpdateProvider::OTAQueryStatus;
using OTAApplyUpdateAction = chip::app::Clusters::OtaSoftwareUpdateProvider::OTAApplyUpdateAction;
using QueryImage = chip::app::Clusters::OtaSoftwareUpdateProvider::Commands::QueryImage;
using QueryImageResponse = chip::app::Clusters::OtaSoftwareUpdateProvider::Commands::QueryImageResponse;
using ApplyUpdateRequest = chip::app::Clusters::OtaSoftwareUpdateProvider::Commands::ApplyUpdateRequest;
using ApplyUpdateResponse = chip::app::Clusters::OtaSoftwareUpdateProvider::Commands::ApplyUpdateResponse;
using NotifyUpdateApplied = chip::app::Clusters::OtaSoftwareUpdateProvider::Commands::NotifyUpdateApplied;

static constexpr char TAG[] = "matter_ota";
static constexpr uint64_t k_controller_node_id = 0x1ULL;
static constexpr uint16_t k_local_httpd_port = 18080;
static constexpr uint32_t k_max_bdx_block = 1024;
static constexpr chip::System::Clock::Timeout k_bdx_timeout = chip::System::Clock::Seconds16(5 * 60);
static constexpr uint32_t k_bdx_poll_ms = 50;
static constexpr uint8_t k_update_token_len = 32;
static constexpr size_t k_uri_max = 256;

struct offer_t {
    bool active;
    uint64_t node_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t software_version;
    char software_version_str[64];
    char image_url[OTA_URL_MAX_LEN];
    char local_file[256];
    char token[17];
    size_t image_size;
    uint8_t update_token[k_update_token_len];
    char bdx_uri[k_uri_max];
};

class MiniHubOtaProvider : public OTAProviderDelegate {
public:
    static MiniHubOtaProvider &instance()
    {
        static MiniHubOtaProvider inst;
        return inst;
    }

    esp_err_t install();
    esp_err_t set_offer(uint64_t node_id, const char *path, uint32_t version_hint,
                        const char *version_str_hint);

    void HandleQueryImage(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                          const QueryImage::DecodableType &commandData) override;
    void HandleApplyUpdateRequest(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                                  const ApplyUpdateRequest::DecodableType &commandData) override;
    void HandleNotifyUpdateApplied(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                                   const NotifyUpdateApplied::DecodableType &commandData) override;

    const char *local_file() const
    {
        return m_offer.local_file[0] ? m_offer.local_file : nullptr;
    }
    const char *token() const
    {
        return m_offer.token;
    }
    uint16_t vendor_id() const
    {
        return m_offer.vendor_id;
    }

private:
    MiniHubOtaProvider() = default;

    void send_query_response(OTAQueryStatus status);
    esp_err_t ensure_local_httpd();
    esp_err_t fill_from_file(const char *path);
    esp_err_t fill_from_url(const char *url, uint32_t version_hint, const char *version_str_hint);

    offer_t m_offer = {};
    esp_matter::ota_provider::OtaBdxSender m_bdx;
    chip::System::Layer *m_system_layer = nullptr;
    chip::FabricTable *m_fabric_table = nullptr;
    CommandHandler::Handle m_async;
    ConcreteCommandPath m_path = ConcreteCommandPath(0, 0, 0);
    chip::Access::SubjectDescriptor m_subject;
    ScopedNodeId m_peer;
    httpd_handle_t m_httpd = nullptr;
};

static bool path_is_http(const char *path)
{
    return path && (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
}

static bool peer_is_loopback(httpd_req_t *req)
{
    int sock = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (sock < 0 || getpeername(sock, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
        return false;
    }
    if (addr.ss_family == AF_INET) {
        auto *in = reinterpret_cast<struct sockaddr_in *>(&addr);
        return ntohl(in->sin_addr.s_addr) == INADDR_LOOPBACK;
    }
#if defined(AF_INET6)
    if (addr.ss_family == AF_INET6) {
        auto *in6 = reinterpret_cast<struct sockaddr_in6 *>(&addr);
#ifdef IN6_IS_ADDR_V4MAPPED
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            uint32_t v4 = 0;
            memcpy(&v4, in6->sin6_addr.s6_addr + 12, sizeof(v4));
            return ntohl(v4) == INADDR_LOOPBACK;
        }
#endif
#ifdef IN6_IS_ADDR_LOOPBACK
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
#endif
    }
#endif
    return false;
}

static esp_err_t handle_local_image(httpd_req_t *req)
{
    MiniHubOtaProvider &p = MiniHubOtaProvider::instance();
    const char *file = p.local_file();
    const char *token = p.token();
    if (!file || !token[0] || !peer_is_loopback(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
    }
    const char *uri = req->uri ? req->uri : "";
    if (*uri == '/') {
        uri++;
    }
    if (strncmp(uri, token, strlen(token)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    FILE *fp = fopen(file, "rb");
    if (!fp) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(fp);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t MiniHubOtaProvider::ensure_local_httpd()
{
    if (m_httpd) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = k_local_httpd_port;
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2;
    cfg.max_uri_handlers = 2;
    cfg.max_open_sockets = 4;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&m_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local ota httpd: %s", esp_err_to_name(err));
        m_httpd = nullptr;
        return err;
    }
    httpd_uri_t uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = handle_local_image,
        .user_ctx = nullptr,
    };
    err = httpd_register_uri_handler(m_httpd, &uri);
    if (err != ESP_OK) {
        httpd_stop(m_httpd);
        m_httpd = nullptr;
        return err;
    }
    ESP_LOGI(TAG, "loopback Matter OTA HTTP on :%u (BDX source)", k_local_httpd_port);
    return ESP_OK;
}

esp_err_t MiniHubOtaProvider::fill_from_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    chip::OTAImageHeaderParser parser;
    parser.Init();
    chip::ByteSpan span(buf, n);
    chip::OTAImageHeader header;
    CHIP_ERROR cerr = parser.AccumulateAndDecode(span, header);
    if (cerr != CHIP_NO_ERROR) {
        parser.Clear();
        ESP_LOGE(TAG, "not a Matter OTA image (%s): %" CHIP_ERROR_FORMAT, path, cerr.Format());
        return ESP_ERR_INVALID_ARG;
    }

    m_offer.vendor_id = header.mVendorId;
    m_offer.product_id = header.mProductId;
    m_offer.software_version = header.mSoftwareVersion;
    size_t vs = header.mSoftwareVersionString.size();
    if (vs >= sizeof(m_offer.software_version_str)) {
        vs = sizeof(m_offer.software_version_str) - 1;
    }
    memcpy(m_offer.software_version_str, header.mSoftwareVersionString.data(), vs);
    m_offer.software_version_str[vs] = '\0';
    parser.Clear();

    m_offer.image_size = static_cast<size_t>(st.st_size);
    strlcpy(m_offer.local_file, path, sizeof(m_offer.local_file));

    uint32_t rnd = esp_random();
    snprintf(m_offer.token, sizeof(m_offer.token), "%08lx.bin", (unsigned long)rnd);
    ESP_RETURN_ON_ERROR(ensure_local_httpd(), TAG, "local httpd");
    snprintf(m_offer.image_url, sizeof(m_offer.image_url), "http://127.0.0.1:%u/%s", k_local_httpd_port,
             m_offer.token);
    return ESP_OK;
}

esp_err_t MiniHubOtaProvider::fill_from_url(const char *url, uint32_t version_hint, const char *version_str_hint)
{
    strlcpy(m_offer.image_url, url, sizeof(m_offer.image_url));
    m_offer.local_file[0] = '\0';
    m_offer.token[0] = '\0';
    m_offer.image_size = 0;
    m_offer.vendor_id = 0;
    m_offer.product_id = 0;
    m_offer.software_version = version_hint;
    if (version_str_hint && version_str_hint[0]) {
        strlcpy(m_offer.software_version_str, version_str_hint, sizeof(m_offer.software_version_str));
    } else if (version_hint) {
        snprintf(m_offer.software_version_str, sizeof(m_offer.software_version_str), "%" PRIu32, version_hint);
    } else {
        m_offer.software_version_str[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t MiniHubOtaProvider::install()
{
    auto *system_state = chip::Controller::DeviceControllerFactory::GetInstance().GetSystemState();
    if (!system_state || !system_state->ExchangeMgr() || !system_state->Fabrics() || !system_state->SystemLayer()) {
        return ESP_ERR_INVALID_STATE;
    }
    m_system_layer = system_state->SystemLayer();
    m_fabric_table = system_state->Fabrics();
    (void)system_state->ExchangeMgr()->UnregisterUnsolicitedMessageHandlerForProtocol(chip::Protocols::BDX::Id);
    CHIP_ERROR cerr =
        system_state->ExchangeMgr()->RegisterUnsolicitedMessageHandlerForProtocol(chip::Protocols::BDX::Id, &m_bdx);
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "register BDX: %" CHIP_ERROR_FORMAT, cerr.Format());
        return ESP_FAIL;
    }
    esp_matter::controller::data_model::provider::get_instance().set_ota_provider_delegate(this);
    ESP_LOGI(TAG, "Matter OTA Provider cluster 0x0029 + BDX installed");
    return ESP_OK;
}

esp_err_t MiniHubOtaProvider::set_offer(uint64_t node_id, const char *path, uint32_t version_hint,
                                        const char *version_str_hint)
{
    memset(&m_offer, 0, sizeof(m_offer));
    m_offer.node_id = node_id;
    esp_err_t err;
    if (path_is_http(path)) {
        err = fill_from_url(path, version_hint, version_str_hint);
    } else {
        err = fill_from_file(path);
        if (err == ESP_OK && version_hint && version_hint > m_offer.software_version) {
            m_offer.software_version = version_hint;
        }
        if (err == ESP_OK && version_str_hint && version_str_hint[0] && !m_offer.software_version_str[0]) {
            strlcpy(m_offer.software_version_str, version_str_hint, sizeof(m_offer.software_version_str));
        }
    }
    if (err != ESP_OK) {
        memset(&m_offer, 0, sizeof(m_offer));
        return err;
    }
    m_offer.active = true;
    ESP_LOGI(TAG, "offer node=%" PRIu64 " ver=%" PRIu32 " (%s) url=%s", node_id, m_offer.software_version,
             m_offer.software_version_str, m_offer.image_url);
    return ESP_OK;
}

void MiniHubOtaProvider::send_query_response(OTAQueryStatus status)
{
    auto handle_ref = std::move(m_async);
    CommandHandler *commandHandle = handle_ref.Get();
    if (!commandHandle) {
        ESP_LOGE(TAG, "no QueryImage command handle");
        return;
    }

    QueryImageResponse::Type response;
    if (status == OTAQueryStatus::kUpdateAvailable && m_offer.active && m_peer.GetNodeId() == m_offer.node_id) {
        const chip::FabricInfo *fabric = m_fabric_table->FindFabricWithIndex(m_subject.fabricIndex);
        if (!fabric) {
            commandHandle->AddStatus(m_path, Status::Failure);
            return;
        }
        NodeId provider_node = fabric->GetPeerId().GetNodeId();
        chip::MutableCharSpan uri(m_offer.bdx_uri);
        const char *slash = strrchr(m_offer.image_url, '/');
        const char *name = slash ? slash + 1 : "image.bin";
        CHIP_ERROR error = chip::bdx::MakeURI(provider_node, CharSpan::fromCharString(name), uri);
        if (error != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "MakeURI failed");
            commandHandle->AddStatus(m_path, Status::Failure);
            return;
        }

        chip::BitFlags<TransferControlFlags> flags;
        flags.Set(TransferControlFlags::kReceiverDrive);
        if (m_bdx.InitializeTransfer(m_subject.fabricIndex, m_subject.subject) != ESP_OK) {
            status = OTAQueryStatus::kBusy;
        } else {
            m_bdx.SetOtaImageUrl(m_offer.image_url);
            CHIP_ERROR bdx_err = m_bdx.PrepareForTransfer(
                m_system_layer, chip::bdx::TransferRole::kSender, flags, k_max_bdx_block, k_bdx_timeout,
                chip::System::Clock::Milliseconds32(k_bdx_poll_ms));
            if (bdx_err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "PrepareForTransfer: %" CHIP_ERROR_FORMAT, bdx_err.Format());
                commandHandle->AddStatus(m_path, Status::Failure);
                return;
            }
            for (uint8_t i = 0; i < k_update_token_len; i++) {
                m_offer.update_token[i] = chip::Crypto::GetRandU8();
            }
            if (!m_offer.software_version_str[0]) {
                snprintf(m_offer.software_version_str, sizeof(m_offer.software_version_str), "%" PRIu32,
                         m_offer.software_version);
            }
            response.imageURI.Emplace(CharSpan::fromCharString(m_offer.bdx_uri));
            response.softwareVersion.Emplace(m_offer.software_version);
            response.softwareVersionString.Emplace(CharSpan::fromCharString(m_offer.software_version_str));
            response.updateToken.Emplace(ByteSpan(m_offer.update_token, k_update_token_len));
            ESP_LOGI(TAG, "QueryImage UpdateAvailable node=%" PRIu64 " ver=%" PRIu32, m_offer.node_id,
                     m_offer.software_version);
        }
    }

    if (status == OTAQueryStatus::kBusy) {
        response.delayedActionTime.Emplace(120);
    }
    response.status = status;
    commandHandle->AddResponse(m_path, response);
}

void MiniHubOtaProvider::HandleQueryImage(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                                          const QueryImage::DecodableType &commandData)
{
    if (!commandObj) {
        return;
    }
    if (m_async.Get() != nullptr) {
        QueryImageResponse::Type response;
        response.status = OTAQueryStatus::kBusy;
        response.delayedActionTime.Emplace(120);
        commandObj->AddResponse(commandPath, response);
        return;
    }

    commandObj->FlushAcksRightAwayOnSlowCommand();
    m_subject = commandObj->GetSubjectDescriptor();
    m_peer = commandObj->GetExchangeContext()->GetSessionHandle()->GetPeer();
    m_async = CommandHandler::Handle(commandObj);
    m_path = commandPath;

    OTAQueryStatus status = OTAQueryStatus::kNotAvailable;
    if (m_offer.active && m_peer.GetNodeId() == m_offer.node_id) {
        uint32_t offered = m_offer.software_version;
        if (offered == 0) {
            offered = commandData.softwareVersion + 1;
            m_offer.software_version = offered;
        }
        const bool vendor_ok = (m_offer.vendor_id == 0) || (m_offer.vendor_id == commandData.vendorID);
        const bool product_ok = (m_offer.product_id == 0) || (m_offer.product_id == commandData.productID);
        if (vendor_ok && product_ok && offered > commandData.softwareVersion) {
            status = OTAQueryStatus::kUpdateAvailable;
        } else {
            ESP_LOGW(TAG, "QueryImage skip vid=%u/%u pid=%u/%u ver=%" PRIu32 "<%" PRIu32,
                     commandData.vendorID, m_offer.vendor_id, commandData.productID, m_offer.product_id,
                     offered, commandData.softwareVersion);
        }
    }
    send_query_response(status);
}

void MiniHubOtaProvider::HandleApplyUpdateRequest(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                                                  const ApplyUpdateRequest::DecodableType &commandData)
{
    if (!commandObj) {
        return;
    }
    ScopedNodeId peer = commandObj->GetExchangeContext()->GetSessionHandle()->GetPeer();
    if (!m_offer.active || peer.GetNodeId() != m_offer.node_id ||
        !commandData.updateToken.data_equal(ByteSpan(m_offer.update_token, k_update_token_len))) {
        commandObj->AddStatus(commandPath, Status::InvalidCommand);
        return;
    }
    ApplyUpdateResponse::Type response;
    response.action = OTAApplyUpdateAction::kProceed;
    response.delayedActionTime = 0;
    commandObj->AddResponse(commandPath, response);
}

void MiniHubOtaProvider::HandleNotifyUpdateApplied(CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
                                                   const NotifyUpdateApplied::DecodableType &commandData)
{
    if (!commandObj) {
        return;
    }
    ScopedNodeId peer = commandObj->GetExchangeContext()->GetSessionHandle()->GetPeer();
    if (!m_offer.active || peer.GetNodeId() != m_offer.node_id ||
        !commandData.updateToken.data_equal(ByteSpan(m_offer.update_token, k_update_token_len))) {
        commandObj->AddStatus(commandPath, Status::InvalidCommand);
        return;
    }
    commandObj->AddStatus(commandPath, Status::Success);
    ESP_LOGI(TAG, "NotifyUpdateApplied node=%" PRIu64 " ver=%" PRIu32, m_offer.node_id,
             commandData.softwareVersion);
    m_offer.active = false;
}

static uint64_t node_id_from_point(const char *point_id)
{
    const minihub_point_t *pt = minihub_point_get(point_id);
    if (pt && pt->props) {
        const cJSON *n = cJSON_GetObjectItem(pt->props, "node_id");
        if (cJSON_IsString(n) && n->valuestring && n->valuestring[0]) {
            return strtoull(n->valuestring, NULL, 10);
        }
        if (cJSON_IsNumber(n)) {
            return (uint64_t)n->valuedouble;
        }
    }
    const char *p = point_id ? strstr(point_id, "matter_n") : NULL;
    if (p) {
        return strtoull(p + 8, NULL, 10);
    }
    return 0;
}

static void hint_from_point(const char *point_id, uint32_t *version, char *ver_str, size_t ver_str_len)
{
    *version = 0;
    if (ver_str && ver_str_len) {
        ver_str[0] = '\0';
    }
    const minihub_point_t *pt = minihub_point_get(point_id);
    if (!pt || !pt->props) {
        return;
    }
    const cJSON *ota = cJSON_GetObjectItem(pt->props, "ota");
    if (!cJSON_IsObject(ota)) {
        return;
    }
    const cJSON *v = cJSON_GetObjectItem(ota, "version");
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        if (ver_str && ver_str_len) {
            strlcpy(ver_str, v->valuestring, ver_str_len);
        }
        *version = (uint32_t)strtoul(v->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(v)) {
        *version = (uint32_t)v->valuedouble;
        if (ver_str && ver_str_len) {
            snprintf(ver_str, ver_str_len, "%" PRIu32, *version);
        }
    }
}

static const char *path_from_point(const char *point_id)
{
    const minihub_point_t *pt = minihub_point_get(point_id);
    if (!pt || !pt->props) {
        return NULL;
    }
    const cJSON *ota = cJSON_GetObjectItem(pt->props, "ota");
    if (!cJSON_IsObject(ota)) {
        return NULL;
    }
    const cJSON *file = cJSON_GetObjectItem(ota, "file");
    if (cJSON_IsString(file) && file->valuestring && file->valuestring[0]) {
        return file->valuestring;
    }
    const cJSON *url = cJSON_GetObjectItem(ota, "url");
    if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
        return url->valuestring;
    }
    return NULL;
}

esp_err_t minihub_matter_ota_provider_install(void)
{
    return MiniHubOtaProvider::instance().install();
}

esp_err_t minihub_matter_ota_offer(const char *point_id, const char *file_path)
{
    if (!point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!minihub_matter_ready()) {
        ESP_LOGW(TAG, "ota_offer: commissioner not ready");
        return ESP_ERR_INVALID_STATE;
    }
    const char *path = file_path;
    if (!path || !path[0]) {
        path = path_from_point(point_id);
    }
    if (!path || !path[0]) {
        ESP_LOGW(TAG, "ota_offer: no file/url for %s", point_id);
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t node_id = node_id_from_point(point_id);
    if (node_id < 2) {
        ESP_LOGW(TAG, "ota_offer: cannot resolve node_id from %s", point_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t ver_hint = 0;
    char ver_str[64] = {0};
    hint_from_point(point_id, &ver_hint, ver_str, sizeof(ver_str));

    MiniHubOtaProvider &prov = MiniHubOtaProvider::instance();
    esp_err_t err = prov.set_offer(node_id, path, ver_hint, ver_str[0] ? ver_str : NULL);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t vendor = MiniHubOtaProvider::instance().vendor_id();
    if (vendor == 0) {
        vendor = 0xFFF1;
    }
    char json[160];
    snprintf(json, sizeof(json), "{\"0:U64\": %" PRIu64 ", \"1:U16\": %u, \"2:U8\": 1, \"4:U16\": 0}",
             k_controller_node_id, vendor);

    constexpr uint32_t k_req_cluster = chip::app::Clusters::OtaSoftwareUpdateRequestor::Id;
    constexpr uint32_t k_announce = chip::app::Clusters::OtaSoftwareUpdateRequestor::Commands::AnnounceOTAProvider::Id;

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    err = esp_matter::controller::send_invoke_cluster_command(node_id, 0, k_req_cluster, k_announce, json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AnnounceOTAProvider failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AnnounceOTAProvider sent to node %" PRIu64 " (Provider 0x0029 / BDX)", node_id);
    return ESP_OK;
}

#else /* OTA provider Kconfig off */

static const char *TAG = "matter_ota";

esp_err_t minihub_matter_ota_provider_install(void)
{
    ESP_LOGW(TAG, "OTA Provider disabled (need DYNAMIC_SERVER + ESP_MATTER_OTA_PROVIDER_ENABLED)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_ota_offer(const char *point_id, const char *file_path)
{
    (void)point_id;
    (void)file_path;
    ESP_LOGW(TAG, "ota_offer: enable CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER and "
                  "CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
