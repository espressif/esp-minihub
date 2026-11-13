/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_lcd_i18n.h"

#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_strings.gen.h"

namespace {

constexpr const char *TAG = "lcd_i18n";
constexpr const char *NVS_NAMESPACE = "minihub_lcd";
constexpr const char *NVS_KEY_LANG = "lang";

std::string s_language = LCD_LANG_ZH;
uint32_t s_generation = 0;

bool supported(const char *lang)
{
    return lang != nullptr &&
           (std::strcmp(lang, LCD_LANG_ZH) == 0 || std::strcmp(lang, LCD_LANG_EN) == 0);
}

const char *translation(const char *key)
{
    const bool english = s_language == LCD_LANG_EN;
    for (const auto &entry : lcd_i18n::kEntries) {
        if (std::strcmp(entry.key, key) == 0) {
            return english ? entry.en : entry.zh;
        }
    }
    /* A missing key is a programming error: show the key so it is obvious on
     * screen, and let the contract test catch it before release. */
    ESP_LOGE(TAG, "missing i18n key: %s", key ? key : "(null)");
    return key;
}

}  // namespace

extern "C" void lcd_i18n_init(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    char stored[8] = {};
    size_t len = sizeof(stored);
    const esp_err_t err = nvs_get_str(handle, NVS_KEY_LANG, stored, &len);
    nvs_close(handle);
    if (err != ESP_OK || !supported(stored)) {
        return;
    }
    s_language = stored;
    ESP_LOGI(TAG, "UI language: %s", s_language.c_str());
}

extern "C" const char *lcd_language(void)
{
    return s_language.c_str();
}

extern "C" const char *lcd_tr(const char *key)
{
    if (key == nullptr || key[0] == '\0') {
        return "";
    }
    return translation(key);
}

extern "C" uint32_t lcd_i18n_generation(void)
{
    return s_generation;
}

extern "C" esp_err_t lcd_i18n_set(const char *lang)
{
    if (!supported(lang)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_language == lang) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        const esp_err_t err = nvs_set_str(handle, NVS_KEY_LANG, lang);
        if (err == ESP_OK) {
            (void)nvs_commit(handle);
        } else {
            ESP_LOGW(TAG, "persist language failed: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
    }
    s_language = lang;
    ++s_generation;
    ESP_LOGI(TAG, "UI language -> %s", s_language.c_str());
    return ESP_OK;
}

extern "C" esp_err_t minihub_lcd_set_language(const char *lang)
{
    const esp_err_t err = lcd_i18n_set(lang);
    if (err != ESP_OK) {
        return err;
    }
    /* Brookesia resolves ${constant.strings.*} at parse time, so a live
     * language switch would need Runtime::update() and a full re-apply of every
     * runtime label/binding (update() does not preserve them). A reboot takes
     * the same path as Web's reload: lcd_i18n_init() reads NVS, make_environment()
     * publishes the new language, and the whole document is parsed with the
     * matching i18n_*.json variant. Language changes are rare and reversible. */
    ESP_LOGI(TAG, "rebooting to apply UI language");
    esp_restart();
    return ESP_OK;
}
