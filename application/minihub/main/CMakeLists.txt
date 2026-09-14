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
if(Python3_FOUND AND EXISTS ${MINIHUB_REPO_ROOT}/config/minihub_manifest.yml)
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
        minihub_provision minihub_net minihub_lcd minihub_rainmaker adapter_mqtt adapter_zigbee
        adapter_matter adapter_thread nvs_flash esp_wifi esp_netif esp_event lwip esp_system
    )
endif()

# Always link USB MSC + RTSP recorder when present (also listed in manifest).
list(APPEND MINIHUB_MAIN_REQUIRES minihub_usb_msc minihub_rtsp_cam minihub_js)

if(CONFIG_MINIHUB_MCP_SERVER)
    list(APPEND MINIHUB_MAIN_REQUIRES minihub_mcp_server)
endif()

# Store L3 plugins: pull into the IDF build graph so libadapter_*.so can be linked
# from their archives. They are NOT registered as Host adapters when
# CONFIG_MINIHUB_DYNAMIC_MODULES=y (see REGISTER_ADAPTER guards in store_modules).
# --gc-sections drops unused objects from the app ELF when nothing references them.
if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    file(GLOB _MINIHUB_STORE_MOD_CMAKE "${MINIHUB_REPO_ROOT}/components/store_modules/*/CMakeLists.txt")
    foreach(_SM_CMAKE ${_MINIHUB_STORE_MOD_CMAKE})
        get_filename_component(_SM_DIR ${_SM_CMAKE} DIRECTORY)
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
# Prefer application/<app>/config/*; fall back to repo config/*
foreach(_cfg adapters.json rules.json scenes.json)
    minihub_resolve_config_asset(${_cfg} _cfg_src)
    if(_cfg_src)
        file(COPY ${_cfg_src} DESTINATION ${MINIHUB_FS_DIR}/config)
        message(STATUS "fs config: ${_cfg} ← ${_cfg_src}")
    else()
        message(WARNING "fs config: ${_cfg} missing (app + repo config/)")
    endif()
endforeach()
minihub_resolve_config_asset(packs _packs_src)
if(_packs_src)
    file(COPY ${_packs_src} DESTINATION ${MINIHUB_FS_DIR}/config)
endif()
minihub_resolve_config_asset(product_params _pp_src)
if(_pp_src)
    file(MAKE_DIRECTORY ${MINIHUB_FS_DIR}/config/product_params)
    file(COPY ${_pp_src}/ DESTINATION ${MINIHUB_FS_DIR}/config/product_params)
endif()
set(_minihub_ui_dep)
if(CONFIG_MINIHUB_LCD_UI AND EXISTS ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui/root.json)
    file(GLOB_RECURSE _minihub_ui_src CONFIGURE_DEPENDS
         ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui/*)
    add_custom_command(
        OUTPUT ${MINIHUB_FS_DIR}/ui/.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${MINIHUB_FS_DIR}/ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${MINIHUB_REPO_ROOT}/components/minihub_lcd/ui ${MINIHUB_FS_DIR}/ui
        COMMAND ${CMAKE_COMMAND} -E touch ${MINIHUB_FS_DIR}/ui/.stamp
        DEPENDS ${_minihub_ui_src}
        COMMENT "Sync minihub_lcd UI into littlefs image"
        VERBATIM
    )
    set(_minihub_ui_dep ${MINIHUB_FS_DIR}/ui/.stamp)
endif()
littlefs_create_partition_image(storage ${MINIHUB_FS_DIR} FLASH_IN_PROJECT
    DEPENDS ${_minihub_ui_dep})

set(MINIHUB_MODSTORE_DIR ${CMAKE_BINARY_DIR}/modstore_image)
file(REMOVE_RECURSE ${MINIHUB_MODSTORE_DIR})
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/modules)
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/packs)
file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/files)

set(_MINIHUB_STORE_ROOT ${MINIHUB_REPO_ROOT}/components/store_modules)
# Built .mhub (zip) / .so / catalog live under deploy dir (not in the source tree).
if(DEFINED ENV{MINIHUB_STORE_DEPLOY} AND NOT "$ENV{MINIHUB_STORE_DEPLOY}" STREQUAL "")
    set(_MINIHUB_STORE_DEPLOY "$ENV{MINIHUB_STORE_DEPLOY}")
else()
    set(_MINIHUB_STORE_DEPLOY "$ENV{HOME}/workspace/minihub_store")
endif()
if(EXISTS ${_MINIHUB_STORE_DEPLOY}/catalog.json)
    file(COPY ${_MINIHUB_STORE_DEPLOY}/catalog.json DESTINATION ${MINIHUB_MODSTORE_DIR})
endif()
# Stage /store/files from deploy/<id>/<id>.mhub (zip package)
minihub_store_preinstall_list(MINIHUB_STORE_PREINSTALL)
message(STATUS "store_preinstall: ${MINIHUB_STORE_PREINSTALL}")
file(GLOB _STORE_MOD_JSONS "${_MINIHUB_STORE_ROOT}/*/module.json")
foreach(_MOD_JSON ${_STORE_MOD_JSONS})
    get_filename_component(_MOD_DIR ${_MOD_JSON} DIRECTORY)
    get_filename_component(_MOD ${_MOD_DIR} NAME)
    set(_DEPLOY_MOD ${_MINIHUB_STORE_DEPLOY}/${_MOD})
    set(_SO ${_DEPLOY_MOD}/${_MOD}.so)
    set(_SO_BUILD ${CMAKE_SOURCE_DIR}/build/${_MOD}.so)
    set(_LIBAD ${CMAKE_BINARY_DIR}/modules/libadapter_${_MOD}.so)
    if(NOT EXISTS ${_LIBAD})
        set(_LIBAD ${CMAKE_SOURCE_DIR}/build/modules/libadapter_${_MOD}.so)
    endif()
    set(_PKG ${_DEPLOY_MOD}/${_MOD}.mhub)
    # MHMD intermediate: wrap libadapter_*.so when build/<id>.so is missing.
    if(NOT EXISTS ${_SO_BUILD} AND EXISTS ${_LIBAD})
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
    if(NOT EXISTS ${_SO} AND EXISTS ${_SO_BUILD})
        set(_SO ${_SO_BUILD})
    endif()
    if(NOT EXISTS ${_PKG})
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env
                    MINIHUB_STORE_DEPLOY=${_MINIHUB_STORE_DEPLOY}
                    ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/pack_store_module.py
                    ${_MOD} --deploy ${_MINIHUB_STORE_DEPLOY}
            WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
            RESULT_VARIABLE _PACK_HUB_RC
        )
        if(NOT _PACK_HUB_RC EQUAL 0)
            message(WARNING "pack_store_module.py failed for ${_MOD} (rc=${_PACK_HUB_RC})")
        endif()
    endif()
    # Prefer MHMD from build/ when deploy has only the zip (pack_store_module strips bare .so).
    if(NOT EXISTS ${_SO} AND EXISTS ${_SO_BUILD})
        set(_SO ${_SO_BUILD})
    endif()
    if(NOT EXISTS ${_SO} AND EXISTS ${_PKG})
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
    if(EXISTS ${_PKG})
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
        if(EXISTS ${_MOD_DIR}/ui.json)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${_MOD_DIR}/ui.json
                            ${MINIHUB_MODSTORE_DIR}/modules/${_MOD}/ui.json)
        endif()
        if(EXISTS ${_MOD_DIR}/web)
            file(COPY ${_MOD_DIR}/web DESTINATION ${MINIHUB_MODSTORE_DIR}/modules/${_MOD})
        endif()
        message(STATUS "modstore: bundled ${_MOD} (factory preinstall under modules/${_MOD}/)")
    elseif(EXISTS ${_PKG})
        message(STATUS "modstore: ${_MOD} available as files/${_MOD}.mhub (not preinstalled)")
    endif()
endforeach()

# Host Web console → /store/preinstall/web/ (source dir from CONFIG_MINIHUB_WEB_STATIC_SRC)
if(CONFIG_MINIHUB_WEB_UI)
    set(_MINIHUB_WEB_REL "${CONFIG_MINIHUB_WEB_STATIC_SRC}")
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
            "CONFIG_MINIHUB_WEB_STATIC_SRC='${_MINIHUB_WEB_REL}': missing index.html at ${_MINIHUB_WEB_SRC}. "
            "Set a repo-relative or absolute path that contains the Host Web console.")
    endif()
    file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/preinstall/web)
    file(COPY ${_MINIHUB_WEB_SRC}/ DESTINATION ${MINIHUB_MODSTORE_DIR}/preinstall/web)
    message(STATUS "modstore: staged Host Web UI from ${_MINIHUB_WEB_SRC} → preinstall/web/")
endif()
minihub_resolve_config_asset(product_params _pp_modstore)
if(_pp_modstore)
    file(MAKE_DIRECTORY ${MINIHUB_MODSTORE_DIR}/preinstall/product_params)
    file(COPY ${_pp_modstore}/ DESTINATION ${MINIHUB_MODSTORE_DIR}/preinstall/product_params)
    message(STATUS "modstore: staged product_params quirks ← ${_pp_modstore}")
endif()

littlefs_create_partition_image(modstore ${MINIHUB_MODSTORE_DIR} FLASH_IN_PROJECT)

include(${MINIHUB_REPO_ROOT}/cmake/matter_no_rtti.cmake)
