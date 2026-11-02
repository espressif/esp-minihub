/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @brief LittleFS mount point (partition label: storage).
 */
#define MINIHUB_VFS_MOUNT       "/data"

#define MINIHUB_CONFIG_DIR        MINIHUB_VFS_MOUNT "/config"
#define MINIHUB_ADAPTERS_JSON     MINIHUB_CONFIG_DIR "/adapters.json"
#define MINIHUB_RULES_JSON        MINIHUB_CONFIG_DIR "/rules.json"
#define MINIHUB_SCENES_JSON       MINIHUB_CONFIG_DIR "/scenes.json"
#define MINIHUB_PACKS_DIR         MINIHUB_CONFIG_DIR "/packs"
#define MINIHUB_MODULE_TMP_DIR    MINIHUB_VFS_MOUNT "/tmp"

/**
 * @brief Device / control icons (SVG) live in storage, not in the firmware: the build
 * stages components/minihub_device_widget/images into the `storage` partition
 * (littlefs) at /data/images, so icons can be added/replaced at runtime next to
 * /data/config/devices.csv. That directory is split by what an icon depicts:
 *   /data/images/devices/<device>.svg                 device-class icon (avatar)
 *   /data/images/parameters/<device>_<parameter>.svg  control icon (mockup)
 * The icon root, the two sub-directory names and the Web console /img/ URI
 * prefix are defined in minihub_device_widget.h (MINIHUB_DW_IMAGES_DIR /
 * MINIHUB_DW_IMAGES_{DEVICES,PARAMETERS}_SUBDIR / MINIHUB_IMAGES_URI_PREFIX) -
 * that component is the single source of truth and the lower layer, so this
 * header only documents the on-device layout.
 */

/**
 * @brief LittleFS partition for store downloads (.mhub / pack JSON).
 */
#define MINIHUB_STORE_MOUNT       "/store"
#define MINIHUB_STORE_MODULES_DIR MINIHUB_STORE_MOUNT "/modules"
#define MINIHUB_STORE_PACKS_DIR   MINIHUB_STORE_MOUNT "/packs"
/**
 * @brief Module packages live under `/store/modules/<id>/`:
 *   module.json, <id>.mhub, optional ui.json
 * Legacy flat `/store/modules/<id>.json` + `<id>.mhub` still accepted by the loader.
 * Source of truth in tree: examples/plugins/<id>/ or MINIHUB_PLUGIN_DIRS.
 */
#define MINIHUB_MODULES_DIR       MINIHUB_STORE_MODULES_DIR
/**
 * @brief Default search path for relative "binary" names (prefer per-module dir).
 */
#define MINIHUB_MODULES_BIN_DIR   MINIHUB_STORE_MODULES_DIR
/**
 * @brief Per-module static UI assets (served when CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB).
 */
#define MINIHUB_MODULE_WEB_SUBDIR "web"

/**
 * @brief Host Web console (OEM-skinnable) on storage — staged into the `storage`
 * LittleFS image at build as /data/web/. PUT /api/dev/web writes here.
 */
#define MINIHUB_WEB_DIR              MINIHUB_VFS_MOUNT "/web"
#define MINIHUB_WEB_INDEX            MINIHUB_WEB_DIR "/index.html"
#define MINIHUB_WEB_LOGIN            MINIHUB_WEB_DIR "/login.html"

/**
 * @brief Store factory preinstall (product_params quirks, zigbee map, …) on /store.
 */
#define MINIHUB_PREINSTALL_DIR       MINIHUB_STORE_MOUNT "/preinstall"
/**
 * @brief Legacy Web path (pre-storage migration); HTTP read fallback only.
 */
#define MINIHUB_PREINSTALL_WEB_DIR   MINIHUB_PREINSTALL_DIR "/web"
#define MINIHUB_PREINSTALL_WEB_INDEX MINIHUB_PREINSTALL_WEB_DIR "/index.html"
#define MINIHUB_PREINSTALL_WEB_LOGIN MINIHUB_PREINSTALL_WEB_DIR "/login.html"

/**
 * @brief Shared buffer sizes (include trailing NUL).
 * Prefer these over ad-hoc array lengths in store / module / JS path code.
 */
#define MINIHUB_MODULE_ID_MAX          48 /*!< `/store/modules/<id>/` directory name. */
/**
 * @brief `/store/modules/<id>` — sizeof(dir) includes NUL; covers '/' before id.
 */
#define MINIHUB_MODULE_PKG_PATH_MAX    (sizeof(MINIHUB_MODULES_DIR) + MINIHUB_MODULE_ID_MAX)
/**
 * @brief `/store/modules/<id>.staging`.
 */
#define MINIHUB_MODULE_STAGING_PATH_MAX (MINIHUB_MODULE_PKG_PATH_MAX + 8)
/**
 * @brief Staging/module.json or staging/<id>.mhub / main.js.
 */
#define MINIHUB_MODULE_FILE_PATH_MAX   (MINIHUB_MODULE_STAGING_PATH_MAX + MINIHUB_MODULE_ID_MAX + 8)
#define MINIHUB_PATH_MAX               256 /*!< General VFS path under /store or /data. */
#define MINIHUB_STORE_BASE_URL_MAX     96  /*!< `http://host:port`. */
#define MINIHUB_STORE_URL_MAX          256 /*!< Catalog url path, e.g. /files/x.hub. */
#define MINIHUB_STORE_ABS_URL_MAX      (MINIHUB_STORE_BASE_URL_MAX + MINIHUB_STORE_URL_MAX)
