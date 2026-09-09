/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_WEB_UI
/**
 * Tiny ROM fallback when /store/preinstall/web/index.html is missing.
 * Full console lives on modstore (OEM-skinnable) — see components/minihub_web/web/.
 * Language follows Accept-Language (zh* → Chinese, otherwise English).
 */
#define MINIHUB_INDEX_FALLBACK_STYLE \
    "body{font-family:system-ui,sans-serif;background:#0b0f1a;color:#eef2ff;" \
    "display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0;padding:24px}" \
    ".box{max-width:420px;border:1px solid #243049;border-radius:14px;padding:24px;background:#151c2f}" \
    "h1{font-size:1.1rem;margin:0 0 12px}p{color:#8b9bb4;font-size:.9rem;line-height:1.5;margin:0 0 10px}" \
    "code{color:#818cf8;font-size:.8rem}"

const char MINIHUB_INDEX_HTML_FALLBACK_ZH[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>MiniHub</title>"
    "<style>" MINIHUB_INDEX_FALLBACK_STYLE "</style></head><body><div class=\"box\">"
    "<h1>Web UI 未安装</h1>"
    "<p>完整控制台应位于 <code>/store/preinstall/web/index.html</code>。</p>"
    "<p>请重新烧录 modstore，或恢复出厂预装的 Web 资源后再访问。</p>"
    "<p>REST API（<code>/api/</code>）在固件中仍然可用。</p>"
    "</div></body></html>";

const char MINIHUB_INDEX_HTML_FALLBACK_EN[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>MiniHub</title>"
    "<style>" MINIHUB_INDEX_FALLBACK_STYLE "</style></head><body><div class=\"box\">"
    "<h1>Web UI is not installed</h1>"
    "<p>The console should be at <code>/store/preinstall/web/index.html</code>.</p>"
    "<p>Flash modstore again, or restore the factory web assets, then reload.</p>"
    "<p>The REST API (<code>/api/</code>) is still available in firmware.</p>"
    "</div></body></html>";
#endif
