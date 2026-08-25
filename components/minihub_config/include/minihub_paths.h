/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/** LittleFS mount point (partition label: storage) */
#define MINIHUB_VFS_MOUNT       "/data"

#define MINIHUB_CONFIG_DIR        MINIHUB_VFS_MOUNT "/config"
#define MINIHUB_ADAPTERS_JSON     MINIHUB_CONFIG_DIR "/adapters.json"
#define MINIHUB_RULES_JSON        MINIHUB_CONFIG_DIR "/rules.json"
#define MINIHUB_SCENES_JSON       MINIHUB_CONFIG_DIR "/scenes.json"
#define MINIHUB_PACKS_DIR         MINIHUB_CONFIG_DIR "/packs"
#define MINIHUB_MODULE_TMP_DIR    MINIHUB_VFS_MOUNT "/tmp"

/** LittleFS partition for store downloads (.mhub / pack JSON) */
#define MINIHUB_STORE_MOUNT       "/store"
#define MINIHUB_STORE_MODULES_DIR MINIHUB_STORE_MOUNT "/modules"
#define MINIHUB_STORE_PACKS_DIR   MINIHUB_STORE_MOUNT "/packs"
/**
 * Module packages live under `/store/modules/<id>/`:
 *   module.json, <id>.mhub, optional ui.json
 * Legacy flat `/store/modules/<id>.json` + `<id>.mhub` still accepted by the loader.
 * Source of truth in tree: components/store_modules/<id>/
 */
#define MINIHUB_MODULES_DIR       MINIHUB_STORE_MODULES_DIR
/** Default search path for relative "binary" names (prefer per-module dir) */
#define MINIHUB_MODULES_BIN_DIR   MINIHUB_STORE_MODULES_DIR
/** Per-module static UI assets (served when CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB) */
#define MINIHUB_MODULE_WEB_SUBDIR "web"

/** Host Web console (OEM-skinnable); staged into modstore at build */
#define MINIHUB_PREINSTALL_DIR       MINIHUB_STORE_MOUNT "/preinstall"
#define MINIHUB_PREINSTALL_WEB_DIR   MINIHUB_PREINSTALL_DIR "/web"
#define MINIHUB_PREINSTALL_WEB_INDEX MINIHUB_PREINSTALL_WEB_DIR "/index.html"

/**
 * Shared buffer sizes (include trailing NUL).
 * Prefer these over ad-hoc array lengths in store / module / JS path code.
 */
#define MINIHUB_MODULE_ID_MAX          48 /**< /store/modules/<id>/ directory name */
/** `/store/modules/<id>` — sizeof(dir) includes NUL; covers '/' before id */
#define MINIHUB_MODULE_PKG_PATH_MAX    (sizeof(MINIHUB_MODULES_DIR) + MINIHUB_MODULE_ID_MAX)
/** `/store/modules/<id>.staging` */
#define MINIHUB_MODULE_STAGING_PATH_MAX (MINIHUB_MODULE_PKG_PATH_MAX + 8)
/** staging/module.json or staging/<id>.mhub / main.js */
#define MINIHUB_MODULE_FILE_PATH_MAX   (MINIHUB_MODULE_STAGING_PATH_MAX + MINIHUB_MODULE_ID_MAX + 8)
#define MINIHUB_PATH_MAX               256 /**< general VFS path under /store or /data */
#define MINIHUB_STORE_BASE_URL_MAX     96  /**< http://host:port */
#define MINIHUB_STORE_URL_MAX          256 /**< catalog url path, e.g. /files/x.hub */
#define MINIHUB_STORE_ABS_URL_MAX      (MINIHUB_STORE_BASE_URL_MAX + MINIHUB_STORE_URL_MAX)
