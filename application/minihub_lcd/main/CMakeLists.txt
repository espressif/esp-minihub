find_package(Python3 COMPONENTS Interpreter QUIET)
get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_minihub_app "$ENV{MINIHUB_APP}")
if(NOT _minihub_app AND EXISTS ${MINIHUB_REPO_ROOT}/.minihub_app)
    file(READ ${MINIHUB_REPO_ROOT}/.minihub_app _minihub_app)
    string(STRIP "${_minihub_app}" _minihub_app)
endif()
if(NOT _minihub_app)
    set(_minihub_app "minihub")
endif()
if(Python3_FOUND AND EXISTS ${MINIHUB_REPO_ROOT}/configs/minihub_manifest.yml)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env MINIHUB_APP=${_minihub_app}
                ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/sync_host_manifest.py
                --app ${_minihub_app}
        WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
        RESULT_VARIABLE _sync_result
    )
    if(NOT _sync_result EQUAL 0)
        message(FATAL_ERROR "sync_host_manifest.py failed (${_sync_result})")
    endif()
endif()

set(_gen_cmake ${MINIHUB_REPO_ROOT}/cmake/generated/minihub_components.cmake)
if(EXISTS ${_gen_cmake})
    include(${_gen_cmake})
else()
    message(WARNING "Generated components cmake missing; run tools/sync_host_manifest.py")
    set(MINIHUB_MAIN_REQUIRES
        minihub_core minihub_config minihub_adapter minihub_rules minihub_scenes
        minihub_web minihub_pack minihub_module minihub_hub minihub_auth minihub_ota
        minihub_provision minihub_net minihub_lcd minihub_rainmaker minihub_mqtt_broker
        adapter_zigbee adapter_matter adapter_thread nvs_flash esp_wifi esp_netif esp_event lwip esp_system
    )
endif()

# Always link USB MSC + RTSP recorder when present (also listed in manifest).
list(APPEND MINIHUB_MAIN_REQUIRES minihub_usb_msc minihub_rtsp_cam minihub_js)

if(CONFIG_MINIHUB_MCP_SERVER)
    list(APPEND MINIHUB_MAIN_REQUIRES minihub_mcp_server)
endif()

# Store L3 plugins: pull into the IDF build graph so libadapter_*.so can be linked
# from their archives. They are NOT registered as Host adapters when
# CONFIG_MINIHUB_DYNAMIC_MODULES=y (see REGISTER_ADAPTER guards in Store plugins).
# --gc-sections drops unused objects from the app ELF when nothing references them.
if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    # Pull plugin components into the IDF build graph (built-in + MINIHUB_PLUGINS).
    minihub_plugin_dirs(_MINIHUB_STORE_MOD_DIRS)
    foreach(_SM_DIR ${_MINIHUB_STORE_MOD_DIRS})
        get_filename_component(_SM ${_SM_DIR} NAME)
        if(EXISTS "${_SM_DIR}/module.json")
            list(APPEND MINIHUB_MAIN_REQUIRES ${_SM})
        endif()
    endforeach()
endif()

idf_component_register(
    SRCS "main.c" "adapters_setup.generated.c"
    INCLUDE_DIRS "."
    REQUIRES ${MINIHUB_MAIN_REQUIRES}
)

include(${MINIHUB_REPO_ROOT}/cmake/minihub_product_assets.cmake)

set(MINIHUB_FS_DIR ${CMAKE_BINARY_DIR}/littlefs_image)
file(REMOVE_RECURSE ${MINIHUB_FS_DIR})
file(MAKE_DIRECTORY ${MINIHUB_FS_DIR}/config)
# Prefer application/<app>/configs/*; fall back to repo configs/*
foreach(_cfg adapters.json rules.json scenes.json)
    minihub_resolve_config_asset(${_cfg} _cfg_src)
    if(_cfg_src)
        file(COPY ${_cfg_src} DESTINATION ${MINIHUB_FS_DIR}/config)
        message(STATUS "fs config: ${_cfg} ← ${_cfg_src}")
    else()
        message(WARNING "fs config: ${_cfg} missing (app + repo configs/)")
    endif()
endforeach()
minihub_resolve_config_asset(packs _packs_src)
if(_packs_src)
    file(COPY ${_packs_src} DESTINATION ${MINIHUB_FS_DIR}/config)
endif()
minihub_resolve_config_asset(product_params _pp_src)
if(_pp_src)
    # Repo config/product_params/example_*.json are templates, not factory seeds.
    file(GLOB _pp_json "${_pp_src}/*.json")
    set(_pp_keep "")
    foreach(_f ${_pp_json})
        get_filename_component(_n ${_f} NAME)
        if(NOT _n MATCHES "^example_")
            list(APPEND _pp_keep ${_f})
        endif()
    endforeach()
    if(_pp_keep)
        file(MAKE_DIRECTORY ${MINIHUB_FS_DIR}/config/product_params)
        file(COPY ${_pp_keep} DESTINATION ${MINIHUB_FS_DIR}/config/product_params)
    endif()
endif()
# Device control icons → /data/images (label storage). The Web console resolves
# the icon for a control from its attributes at render time, so the images must
# be in storage rather than baked into the app binary. .stamp doubles as the
# littlefs target dependency (it is re-created whenever the image dir is wiped).
set(_minihub_img_dep)
set(_MINIHUB_DW_IMAGES_SRC ${MINIHUB_REPO_ROOT}/components/minihub_device_widget/images)
if(EXISTS ${_MINIHUB_DW_IMAGES_SRC})
    file(GLOB_RECURSE _minihub_img_src CONFIGURE_DEPENDS ${_MINIHUB_DW_IMAGES_SRC}/*)
    add_custom_command(
        OUTPUT ${MINIHUB_FS_DIR}/images/.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${MINIHUB_FS_DIR}/images
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${_MINIHUB_DW_IMAGES_SRC} ${MINIHUB_FS_DIR}/images
        COMMAND ${CMAKE_COMMAND} -E touch ${MINIHUB_FS_DIR}/images/.stamp
        DEPENDS ${_minihub_img_src}
        COMMENT "Sync device widget images into littlefs image"
        VERBATIM
    )
    set(_minihub_img_dep ${MINIHUB_FS_DIR}/images/.stamp)
else()
    message(WARNING "device widget images missing: ${_MINIHUB_DW_IMAGES_SRC}")
endif()

set(_minihub_ui_dep)
if(CONFIG_MINIHUB_LCD_UI AND EXISTS ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui/root.json)
    file(GLOB_RECURSE _minihub_ui_src CONFIGURE_DEPENDS
         ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui/*)
    add_custom_command(
        OUTPUT ${MINIHUB_FS_DIR}/ui/.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${MINIHUB_FS_DIR}/ui
        # UI JSON keeps editor-friendly formatting in the source tree. Minify it
        # into the image (as the Web console already does): the expanded layout
        # alone costs ~147 KB and pushed littlefs into LFS_ERR_NOSPC at build
        # time, which also surfaced as runtime "No more free space" on the board.
        # Stamp CONFIG_MINIHUB_LCD_UI_VERSION into version.txt (same Kconfig
        # tools/deploy.sh publishes as LCD_UI/v<ver>/), so a freshly flashed Hub
        # and the store tree report the same LCD UI version.
        COMMAND ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/minify_lcd_ui_assets.py
                --src ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui
                --dst ${MINIHUB_FS_DIR}/ui
                --ui-version "${CONFIG_MINIHUB_LCD_UI_VERSION}"
                -q
        COMMAND ${CMAKE_COMMAND} -E touch ${MINIHUB_FS_DIR}/ui/.stamp
        DEPENDS ${_minihub_ui_src} ${MINIHUB_REPO_ROOT}/tools/minify_lcd_ui_assets.py
        COMMENT "Minify minihub_lcd UI JSON into littlefs image"
        VERBATIM
    )
    set(_minihub_ui_dep ${MINIHUB_FS_DIR}/ui/.stamp)
endif()

# Host Web console → /data/web/ (source dir from CONFIG_MINIHUB_CUSTOM_WEB_SRC).
# Minify at build time so edits under components/minihub_web/web rebuild storage.bin.
set(_minihub_web_dep)
if(CONFIG_MINIHUB_WEB_UI)
    set(_MINIHUB_WEB_REL "${CONFIG_MINIHUB_CUSTOM_WEB_SRC}")
    if(NOT _MINIHUB_WEB_REL)
        set(_MINIHUB_WEB_REL "components/minihub_web/web")
    endif()
    if(IS_ABSOLUTE "${_MINIHUB_WEB_REL}")
        set(_MINIHUB_WEB_SRC "${_MINIHUB_WEB_REL}")
    else()
        set(_MINIHUB_WEB_SRC "${MINIHUB_REPO_ROOT}/${_MINIHUB_WEB_REL}")
    endif()
    get_filename_component(_MINIHUB_WEB_SRC "${_MINIHUB_WEB_SRC}" ABSOLUTE)
    if(NOT EXISTS "${_MINIHUB_WEB_SRC}/index.html")
        message(FATAL_ERROR
            "CONFIG_MINIHUB_CUSTOM_WEB_SRC='${_MINIHUB_WEB_REL}': missing index.html at ${_MINIHUB_WEB_SRC}. "
            "Set a repo-relative or absolute path that contains the Host Web console.")
    endif()
    file(GLOB_RECURSE _minihub_web_src CONFIGURE_DEPENDS ${_MINIHUB_WEB_SRC}/*)
    # Single source of truth for the bundle version: CONFIG_MINIHUB_WEB_VERSION,
    # the same Kconfig tools/deploy.sh uses for Webs/v<version>/. It is stamped
    # into version.txt and window.mhWebVersion here, so a freshly flashed Hub and
    # the published bundle report the same value (no phantom "new web" hint).
    add_custom_command(
        OUTPUT ${MINIHUB_FS_DIR}/web/.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${MINIHUB_FS_DIR}/web
        COMMAND ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/minify_web_assets.py
                --src ${_MINIHUB_WEB_SRC}
                --dst ${MINIHUB_FS_DIR}/web
                --web-version "${CONFIG_MINIHUB_WEB_VERSION}"
                --gzip
                -q
        COMMAND ${CMAKE_COMMAND} -E touch ${MINIHUB_FS_DIR}/web/.stamp
        DEPENDS ${_minihub_web_src} ${MINIHUB_REPO_ROOT}/tools/minify_web_assets.py
        COMMENT "Minify Host Web console into littlefs storage image"
        VERBATIM
    )
    set(_minihub_web_dep ${MINIHUB_FS_DIR}/web/.stamp)
    message(STATUS "storage: Host Web UI from ${_MINIHUB_WEB_SRC} → web/ (minified at build)")
endif()

littlefs_create_partition_image(storage ${MINIHUB_FS_DIR} FLASH_IN_PROJECT
    DEPENDS ${_minihub_ui_dep} ${_minihub_img_dep} ${_minihub_web_dep})

set(MINIHUB_MODSTORE_DIR ${CMAKE_BINARY_DIR}/modstore_image)
file(REMOVE_RECURSE ${MINIHUB_MODSTORE_DIR})
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/modules)
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/packs)
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/files)

# Built .mhub (zip) / .so / catalog live under deploy dir (not in the source tree).
if(DEFINED ENV{MINIHUB_PLUGINS_DEPLOY_DIR} AND NOT "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}" STREQUAL "")
    set(_MINIHUB_PLUGINS_DEPLOY_DIR "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}")
else()
    set(_MINIHUB_PLUGINS_DEPLOY_DIR "$ENV{HOME}/workspace/minihub_store")
endif()
# Catalog is copied AFTER packing below so size/binary_size match staged .mhub.
# Stage /store/files from Stores/Common for L1 and L2.
# A native module is not a store package.
minihub_store_preinstall_list(MINIHUB_STORE_PREINSTALL)
message(STATUS "store_preinstall: ${MINIHUB_STORE_PREINSTALL}")
# All Store modules (L1/L2/L3) for .mhub staging — includes runtime js/pack under
# MINIHUB_PLUGIN_DIRS. Native .so ninja rules use minihub_plugin_dirs instead.
set(_STORE_MOD_JSONS "")
minihub_store_module_dirs(_MH_STORE_MOD_DIRS)
foreach(_pd ${_MH_STORE_MOD_DIRS})
    if(EXISTS "${_pd}/module.json")
        list(APPEND _STORE_MOD_JSONS "${_pd}/module.json")
    endif()
endforeach()
foreach(_MOD_JSON ${_STORE_MOD_JSONS})
    get_filename_component(_MOD_DIR ${_MOD_JSON} DIRECTORY)
    get_filename_component(_MOD ${_MOD_DIR} NAME)
    set(_IS_NATIVE FALSE)
    if(EXISTS "${_MOD_DIR}/CMakeLists.txt")
        set(_IS_NATIVE TRUE)
    endif()
    if(_IS_NATIVE)
        # L3 is compiled by `build_firmware.py module` into the build tree.
        # Configure does not look up or copy a store package for it.
        set(_DEPLOY_MOD "")
        set(_PKG "")
        set(_SO "")
    else()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c
                "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); import store_paths; print(store_paths.module_mhub_file(sys.argv[2], deploy=Path(sys.argv[3]), module_dir=Path(sys.argv[4])).parent)"
                "${MINIHUB_REPO_ROOT}/tools"
                "${_MOD}"
                "${_MINIHUB_PLUGINS_DEPLOY_DIR}"
                "${_MOD_DIR}"
        OUTPUT_VARIABLE _DEPLOY_MOD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _DEPLOY_MOD_RC
    )
    if(NOT _DEPLOY_MOD_RC EQUAL 0 OR "${_DEPLOY_MOD}" STREQUAL "")
        message(WARNING "modstore: could not resolve ${_MOD} deploy dir (rc=${_DEPLOY_MOD_RC})")
        set(_DEPLOY_MOD ${_MINIHUB_PLUGINS_DEPLOY_DIR}/Stores/Common/${_MOD})
    endif()
    set(_SO ${_DEPLOY_MOD}/${_MOD}.so)
    set(_SO_BUILD ${CMAKE_SOURCE_DIR}/build/${_MOD}.so)
    set(_LIBAD ${CMAKE_BINARY_DIR}/modules/libadapter_${_MOD}.so)
    if(NOT EXISTS ${_LIBAD})
        set(_LIBAD ${CMAKE_SOURCE_DIR}/build/modules/libadapter_${_MOD}.so)
    endif()
    set(_PKG ${_DEPLOY_MOD}/${_MOD}.mhub)
    endif()
    # Native/hybrid plugins are compiled only by `build_firmware.py module`.
    # Host configure does not rebuild them and does not publish a store package.
    if(NOT _IS_NATIVE)
        # Always refresh MHMD from libadapter_*.so when present. A stale
        # build/<id>.so previously blocked refresh because the wrap step only
        # ran when build/<id>.so was missing.
        if(EXISTS ${_LIBAD})
            execute_process(
                COMMAND ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/pack_store_module.py mhmd
                        ${_LIBAD} ${_SO_BUILD}
                WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
                RESULT_VARIABLE _PACK_MHMD_RC
            )
            if(NOT _PACK_MHMD_RC EQUAL 0)
                message(WARNING "pack_store_module.py mhmd failed for ${_MOD} (rc=${_PACK_MHMD_RC})")
            endif()
        endif()
        # Prefer freshly wrapped build/<id>.so over deploy or extracted zip copies.
        if(EXISTS ${_SO_BUILD})
            set(_SO ${_SO_BUILD})
        elseif(NOT EXISTS ${_SO} AND EXISTS ${_PKG})
            file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/modstore_so_extract)
            execute_process(
                COMMAND ${Python3_EXECUTABLE} -c
                    "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); n=sys.argv[2]+'.so'; open(sys.argv[3],'wb').write(z.read(n)) if n in z.namelist() else sys.exit(1)"
                ${_PKG} ${_MOD} ${CMAKE_BINARY_DIR}/modstore_so_extract/${_MOD}.so
                RESULT_VARIABLE _EXTRACT_SO_RC
            )
            if(_EXTRACT_SO_RC EQUAL 0)
                set(_SO ${CMAKE_BINARY_DIR}/modstore_so_extract/${_MOD}.so)
            endif()
        endif()
        # Repack .mhub whenever we have a fresh MHMD so factory/store files are not stale.
        if(EXISTS ${_SO_BUILD})
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E env
                        MINIHUB_PLUGINS_DEPLOY_DIR=${_MINIHUB_PLUGINS_DEPLOY_DIR}
                        ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/pack_store_module.py
                        ${_MOD} --deploy ${_MINIHUB_PLUGINS_DEPLOY_DIR}
                WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
                RESULT_VARIABLE _PACK_HUB_RC
            )
            if(NOT _PACK_HUB_RC EQUAL 0)
                message(WARNING "pack_store_module.py failed for ${_MOD} (rc=${_PACK_HUB_RC})")
            endif()
            set(_PKG ${_DEPLOY_MOD}/${_MOD}.mhub)
        elseif(NOT EXISTS ${_PKG})
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E env
                        MINIHUB_PLUGINS_DEPLOY_DIR=${_MINIHUB_PLUGINS_DEPLOY_DIR}
                        ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/pack_store_module.py
                        ${_MOD} --deploy ${_MINIHUB_PLUGINS_DEPLOY_DIR}
                WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
                RESULT_VARIABLE _PACK_HUB_RC
            )
            if(NOT _PACK_HUB_RC EQUAL 0)
                message(WARNING "pack_store_module.py failed for ${_MOD} (rc=${_PACK_HUB_RC})")
            endif()
        endif()
    endif()
    if(_IS_NATIVE)
        # L3 has no store package to stage.
    elseif(EXISTS ${_PKG})
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_PKG}
                        ${MINIHUB_MODSTORE_DIR}/files/${_MOD}.mhub)
    else()
        message(WARNING "modstore: ${_MOD}.mhub missing under ${_DEPLOY_MOD}")
    endif()
    # Factory-preinstall into /store/modules/<id>/ only when listed in
    # application/<app>/manifest.yml → store_preinstall (module.json "preinstall" ignored).
    # Catalog + /files/*.mhub stay available for on-device Store install.
    set(_PREINSTALL FALSE)
    list(FIND MINIHUB_STORE_PREINSTALL ${_MOD} _PRE_IDX)
    if(NOT _PRE_IDX EQUAL -1)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c
                "import json,sys; m=json.load(open(sys.argv[1])); r=m.get('runtime'); sys.exit(0 if r=='hybrid' else 1)"
                ${_MOD_JSON}
            RESULT_VARIABLE _IS_HYBRID_RC
        )
        if(_IS_HYBRID_RC EQUAL 0)
            # Hybrid: require both MHMD .so and main.js
            if(EXISTS ${_SO} AND EXISTS ${_MOD_DIR}/main.js)
                set(_PREINSTALL TRUE)
            else()
                message(WARNING "modstore: hybrid ${_MOD} preinstall skipped (need .so + main.js)")
            endif()
        elseif(EXISTS ${_SO})
            set(_PREINSTALL TRUE)
        elseif(EXISTS ${_MOD_DIR}/main.js OR EXISTS ${_MOD_DIR}/web)
            set(_PREINSTALL TRUE)
        else()
            message(WARNING "modstore: ${_MOD} in store_preinstall but no .so/main.js/web — skipped")
        endif()
    endif()
    if(_PREINSTALL)
        file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/modules/${_MOD})
        if(EXISTS ${_SO})
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_SO}
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/${_MOD}.so)
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${_MOD_JSON}
                    ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/module.json)
        if(EXISTS ${_MOD_DIR}/main.js)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_MOD_DIR}/main.js
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/main.js)
        endif()
        if(EXISTS ${_MOD_DIR}/meta.json)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_MOD_DIR}/meta.json
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/meta.json)
        elseif(EXISTS ${_MOD_DIR}/ui.json)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_MOD_DIR}/ui.json
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/ui.json)
        endif()
        if(EXISTS ${_MOD_DIR}/devices.json)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_MOD_DIR}/devices.json
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/devices.json)
        endif()
        if(EXISTS ${_MOD_DIR}/web)
            file(COPY ${_MOD_DIR}/web DESTINATION ${MINIHUB_MODSTORE_DIR}/modules/${_MOD})
        endif()
        message(STATUS "modstore: bundled ${_MOD} (factory preinstall under modules/${_MOD}/)")
    elseif(EXISTS ${_PKG})
        message(STATUS "modstore: ${_MOD} available as files/${_MOD}.mhub (not preinstalled)")
    endif()
endforeach()

# The deploy tree carries no derived index: /api/catalog aggregates the
# per-plugin catalog.json objects at request time. The device's bundled local
# store still needs a single array, so ask for one at an explicit path.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            MINIHUB_PLUGINS_DEPLOY_DIR=${_MINIHUB_PLUGINS_DEPLOY_DIR}
            MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}
            MINIHUB_APP=$ENV{MINIHUB_APP}
            ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/sync_store_catalog.py
            --catalog-only --deploy ${_MINIHUB_PLUGINS_DEPLOY_DIR}
            --aggregate ${MINIHUB_MODSTORE_DIR}/catalog.json
    WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
    RESULT_VARIABLE _CATALOG_RC
)
if(NOT _CATALOG_RC EQUAL 0)
    message(WARNING "sync_store_catalog --catalog-only failed (rc=${_CATALOG_RC})")
endif()
if(NOT EXISTS ${MINIHUB_MODSTORE_DIR}/catalog.json)
    message(WARNING "modstore: catalog.json not generated under ${MINIHUB_MODSTORE_DIR}")
endif()

minihub_resolve_config_asset(product_params _pp_modstore)
if(_pp_modstore)
    file(GLOB _pp_mjson "${_pp_modstore}/*.json")
    set(_pp_mkeep "")
    foreach(_f ${_pp_mjson})
        get_filename_component(_n ${_f} NAME)
        if(NOT _n MATCHES "^example_")
            list(APPEND _pp_mkeep ${_f})
        endif()
    endforeach()
    if(_pp_mkeep)
        file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/preinstall/product_params)
        file(COPY ${_pp_mkeep} DESTINATION ${MINIHUB_MODSTORE_DIR}/preinstall/product_params)
        message(STATUS "modstore: staged product_params quirks ← ${_pp_modstore}")
    endif()
endif()

littlefs_create_partition_image(modstore ${MINIHUB_MODSTORE_DIR} FLASH_IN_PROJECT)
