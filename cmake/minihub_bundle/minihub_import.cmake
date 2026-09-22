# Include this file before include($ENV{IDF_PATH}/tools/cmake/project.cmake).
# After that include, call minihub_register_kconfig() before project().
# After project() (DEFER): L3 .so targets and CHIP -fno-rtti.
#
#   set(MINIHUB_IMPORT_STORE_MODULES_DISABLED ON)  # skip L3 .so from MINIHUB_PLUGIN_DIRS
#   include("${MINIHUB_REPO_ROOT}/cmake/minihub_bundle/minihub_import.cmake")
#   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   minihub_register_kconfig()
#   project(my_app)
#
# menu "MiniHub" is cmake/minihub_bundle/Kconfig.projbuild. The product must
# have minihub_bundle in the build graph (Host: require + EXTRA below;
# Mosaic: main yml path: …/cmake/minihub_bundle).

include_guard(GLOBAL)

get_filename_component(MINIHUB_IMPORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(MINIHUB_BUNDLE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
if(NOT MINIHUB_REPO_ROOT)
    set(MINIHUB_REPO_ROOT "${MINIHUB_IMPORT_ROOT}")
endif()

if(NOT EXISTS "${MINIHUB_IMPORT_ROOT}/components/minihub_core")
    message(FATAL_ERROR "minihub_import: Core missing at ${MINIHUB_IMPORT_ROOT}/components")
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "${MINIHUB_IMPORT_ROOT}/components"
    "${MINIHUB_IMPORT_ROOT}/components/minihub_adapters"
    "${MINIHUB_BUNDLE_DIR}"
)
if(NOT MINIHUB_IMPORT_STORE_MODULES_DISABLED)
    # L3 sources come from MINIHUB_PLUGIN_DIRS (plugin dir or parent). Demos:
    # examples/plugins via that env — not a Host builtin component tree.
    set(_mh_store_ids "")
    set(_mh_store_reals "")
    if(DEFINED ENV{MINIHUB_PLUGIN_DIRS} AND NOT "$ENV{MINIHUB_PLUGIN_DIRS}" STREQUAL "")
        string(REPLACE ":" ";" _mh_env_pl "$ENV{MINIHUB_PLUGIN_DIRS}")
        foreach(_pd ${_mh_env_pl})
            if(NOT EXISTS "${_pd}")
                message(WARNING "MINIHUB_PLUGIN_DIRS entry not found: ${_pd}")
                continue()
            endif()
            # Native L3 only: need CMakeLists.txt. L1/L2 (runtime js/pack, main.js
            # only) must not enter EXTRA_COMPONENT_DIRS or ninja will demand idf::<id>.
            if(EXISTS "${_pd}/module.json" AND EXISTS "${_pd}/CMakeLists.txt")
                get_filename_component(_id ${_pd} NAME)
                get_filename_component(_real ${_pd} REALPATH)
                if(_id IN_LIST _mh_store_ids OR _real IN_LIST _mh_store_reals)
                    continue()
                endif()
                list(APPEND _mh_store_ids "${_id}")
                list(APPEND _mh_store_reals "${_real}")
                list(APPEND EXTRA_COMPONENT_DIRS "${_pd}")
            elseif(NOT EXISTS "${_pd}/module.json")
                file(GLOB _pd_sub "${_pd}/*/CMakeLists.txt")
                foreach(_c ${_pd_sub})
                    get_filename_component(_d ${_c} DIRECTORY)
                    get_filename_component(_id ${_d} NAME)
                    get_filename_component(_real ${_d} REALPATH)
                    if(_id IN_LIST _mh_store_ids OR _real IN_LIST _mh_store_reals)
                        continue()
                    endif()
                    list(APPEND _mh_store_ids "${_id}")
                    list(APPEND _mh_store_reals "${_real}")
                    list(APPEND EXTRA_COMPONENT_DIRS "${_d}")
                endforeach()
            endif()
        endforeach()
    endif()
endif()

# cap_mcp_server lives in esp-claw, not this repo. Do not put it in bundle yml:
# the claw tree is a product checkout (Host submodule vs Mosaic third-party).
foreach(_mh_claw IN ITEMS
        "${MINIHUB_CLAW_CAPABILITIES_DIR}"
        "${CMAKE_SOURCE_DIR}/third-party/esp-claw/components/claw_capabilities"
        "${MINIHUB_IMPORT_ROOT}/components/esp-claw/components/claw_capabilities"
        "${MINIHUB_IMPORT_ROOT}/application/minihub/components/esp-claw/components/claw_capabilities")
    if(_mh_claw AND EXISTS "${_mh_claw}/cap_mcp_server")
        list(APPEND EXTRA_COMPONENT_DIRS "${_mh_claw}")
        break()
    endif()
endforeach()

# Same gate as minihub_matter: ESP_MATTER_PATH and MINIHUB_BUILD_MATTER != 0.
# Product opt-out: set(MINIHUB_BUILD_MATTER OFF) before this include, or env=0.
set(MINIHUB_MATTER_SDK_IMPORTED 0)
set(_mh_matter_off "$ENV{MINIHUB_BUILD_MATTER}")
if(DEFINED MINIHUB_BUILD_MATTER AND (
        MINIHUB_BUILD_MATTER STREQUAL "OFF" OR MINIHUB_BUILD_MATTER STREQUAL "0"))
    set(_mh_matter_off "0")
endif()
if(DEFINED ENV{ESP_MATTER_PATH} AND NOT "${_mh_matter_off}" STREQUAL "0")
    set(ESP_MATTER_PATH "$ENV{ESP_MATTER_PATH}")
    set(MATTER_SDK_PATH "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip")
    if(NOT EXISTS "${ESP_MATTER_PATH}/components")
        message(FATAL_ERROR
            "ESP_MATTER_PATH has no components/: ${ESP_MATTER_PATH}")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
    set(_mh_gn_dir "${MATTER_SDK_PATH}/.environment/cipd/packages/pigweed")
    if(EXISTS "${_mh_gn_dir}/gn")
        set(ENV{PATH} "${_mh_gn_dir}:$ENV{PATH}")
    endif()
    # `gn gen` runs under ninja, so it inherits the shell env, not this one:
    # _PW_ACTUAL_ENVIRONMENT_ROOT must come from export.sh or CHIP fails to
    # import build_overrides/pigweed_environment.gni.
    if("$ENV{_PW_ACTUAL_ENVIRONMENT_ROOT}" STREQUAL "")
        message(FATAL_ERROR
            "esp-matter env not sourced. Run in this shell:\n"
            "  source \"${ESP_MATTER_PATH}/export.sh\"\n"
            "Or build without Matter: MINIHUB_BUILD_MATTER=0")
    endif()
    set(MINIHUB_MATTER_SDK_IMPORTED 1)
endif()

# Collect NATIVE (L3) plugin SOURCE dirs (module.json + CMakeLists.txt) into <out>.
# Only MINIHUB_PLUGIN_DIRS (;/:-separated). Export examples/plugins there to
# compile in-tree demos — they are not a Host builtin tree.
# A MINIHUB_PLUGIN_DIRS entry may be a single plugin dir or a parent dir.
# L1/L2 (no CMakeLists.txt) are excluded — use minihub_store_module_dirs for
# .mhub staging. Deduplicate by plugin id and REALPATH.
function(minihub_plugin_dirs out)
    set(_dirs "")
    set(_seen_ids "")
    set(_seen_reals "")
    if(DEFINED ENV{MINIHUB_PLUGIN_DIRS} AND NOT "$ENV{MINIHUB_PLUGIN_DIRS}" STREQUAL "")
        string(REPLACE ":" ";" _env_pl "$ENV{MINIHUB_PLUGIN_DIRS}")
        foreach(_pd ${_env_pl})
            if(NOT EXISTS "${_pd}")
                message(WARNING "MINIHUB_PLUGIN_DIRS entry not found: ${_pd}")
                continue()
            endif()
            if(EXISTS "${_pd}/module.json")
                # Single plugin entry: L3 only (needs CMakeLists.txt for idf::<id>).
                if(NOT EXISTS "${_pd}/CMakeLists.txt")
                    continue()
                endif()
                set(_cand "${_pd}")
                get_filename_component(_id ${_cand} NAME)
                get_filename_component(_real ${_cand} REALPATH)
                if(_id IN_LIST _seen_ids OR _real IN_LIST _seen_reals)
                    continue()
                endif()
                list(APPEND _seen_ids "${_id}")
                list(APPEND _seen_reals "${_real}")
                list(APPEND _dirs "${_cand}")
            else()
                file(GLOB _sub "${_pd}/*/CMakeLists.txt")
                foreach(_c ${_sub})
                    get_filename_component(_d ${_c} DIRECTORY)
                    get_filename_component(_id ${_d} NAME)
                    get_filename_component(_real ${_d} REALPATH)
                    if(_id IN_LIST _seen_ids OR _real IN_LIST _seen_reals)
                        continue()
                    endif()
                    list(APPEND _seen_ids "${_id}")
                    list(APPEND _seen_reals "${_real}")
                    list(APPEND _dirs "${_d}")
                endforeach()
            endif()
        endforeach()
    endif()
    set(${out} ${_dirs} PARENT_SCOPE)
endfunction()

# All Store module dirs with module.json (L1/L2/L3) for .mhub staging / catalog.
# Unlike minihub_plugin_dirs, includes runtime js/pack plugins that have no
# CMakeLists.txt. Sources: MINIHUB_PLUGIN_DIRS only. Dedupes by id and REALPATH.
function(minihub_store_module_dirs out)
    set(_dirs "")
    set(_seen_ids "")
    set(_seen_reals "")
    if(DEFINED ENV{MINIHUB_PLUGIN_DIRS} AND NOT "$ENV{MINIHUB_PLUGIN_DIRS}" STREQUAL "")
        string(REPLACE ":" ";" _env_pl "$ENV{MINIHUB_PLUGIN_DIRS}")
        foreach(_pd ${_env_pl})
            if(NOT EXISTS "${_pd}")
                continue()
            endif()
            if(EXISTS "${_pd}/module.json")
                set(_cand_list "${_pd}")
            else()
                file(GLOB _sub_json "${_pd}/*/module.json")
                set(_cand_list "")
                foreach(_j ${_sub_json})
                    get_filename_component(_d ${_j} DIRECTORY)
                    list(APPEND _cand_list "${_d}")
                endforeach()
            endif()
            foreach(_cand ${_cand_list})
                get_filename_component(_id ${_cand} NAME)
                if(_id MATCHES "^_")
                    continue()
                endif()
                get_filename_component(_real ${_cand} REALPATH)
                if(_id IN_LIST _seen_ids OR _real IN_LIST _seen_reals)
                    continue()
                endif()
                list(APPEND _seen_ids "${_id}")
                list(APPEND _seen_reals "${_real}")
                list(APPEND _dirs "${_cand}")
            endforeach()
        endforeach()
    endif()
    set(${out} ${_dirs} PARENT_SCOPE)
endfunction()

function(minihub_register_kconfig)
    if(NOT COMMAND idf_build_set_property)
        message(FATAL_ERROR
            "minihub_register_kconfig() must run after include(project.cmake) "
            "and before project()")
    endif()
    if(MINIHUB_MATTER_SDK_IMPORTED)
        idf_build_set_property(
            COMPILE_OPTIONS "-Wno-error=format;-Wno-error=unused-result" APPEND)
    endif()
endfunction()

# CONFIG_* and BUILD_COMPONENTS exist only after project().
function(minihub_after_project)
    if(NOT MINIHUB_IMPORT_STORE_MODULES_DISABLED AND CONFIG_MINIHUB_DYNAMIC_MODULES)
        include("${MINIHUB_IMPORT_ROOT}/cmake/minihub_module_so.cmake")
        minihub_register_module_sos()
    endif()
    if(CONFIG_MINIHUB_MATTER_ADAPTER)
        include("${MINIHUB_IMPORT_ROOT}/cmake/matter_no_rtti.cmake")
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL minihub_after_project)
