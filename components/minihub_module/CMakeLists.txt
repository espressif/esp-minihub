
set(MOD_SRCS module_loader.c module_abi_check.c)

# Repository root (cheap path derivation; used by the generation step below and
# again further down for the plugin SDK check).
get_filename_component(_mh_repo_for_sdk "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Bake the host ABI symbol contract into firmware for offline module compatibility
# checks (defense-in-depth; the store server already filters by abi_total_hash).
# Guarded so the custom command is only emitted during the real configure pass,
# not the ESP-IDF early-expansion (get-requirements) pass where add_custom_command
# is not scriptable (CMAKE_BUILD_EARLY_EXPANSION == 1 there).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
set(_abi_gen "${CMAKE_CURRENT_BINARY_DIR}/minihub_abi_symbols.gen.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_command(
    OUTPUT "${_abi_gen}"
    COMMAND python3 "${_mh_repo_for_sdk}/tools/gen_abi_contract.py"
        --header "${_mh_repo_for_sdk}/components/minihub_module/include/minihub_module_abi.h"
        --allowlist "${_mh_repo_for_sdk}/docs/module_sdk/host_export_allowlist.json"
        --out-header "${_abi_gen}"
    DEPENDS
        "${_mh_repo_for_sdk}/tools/gen_abi_contract.py"
        "${_mh_repo_for_sdk}/tools/abi_contract_lib.py"
        "${_mh_repo_for_sdk}/components/minihub_module/include/minihub_module_abi.h"
        "${_mh_repo_for_sdk}/docs/module_sdk/host_export_allowlist.json"
    COMMENT "Bake host ABI symbol contract (minihub_abi_symbols.gen.h)"
    VERBATIM
)
set_source_files_properties("${_abi_gen}" PROPERTIES GENERATED TRUE)
list(APPEND MOD_SRCS "${_abi_gen}")
endif()

set(MOD_REQ
    minihub_core minihub_adapter minihub_config minihub_hub
    minihub_ble_mesh minihub_ble_gatt minihub_zigbee minihub_net
    espressif__cjson log
    esp_http_client mbedtls nvs_flash esp-tls freertos
)
# elf_loader wraps esp_flash_* ; only pull it in when .so loading is enabled.
if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    list(APPEND MOD_SRCS module_dl.c host_elf_symbols.c)
    # Prefer build/gen copy from tools/build_store_modules.py; fall back to committed baseline.
    set(_host_elf_gen "${CMAKE_BINARY_DIR}/../../gen/host_elf_symbols_generated.c")
    if(EXISTS "${_host_elf_gen}")
        list(APPEND MOD_SRCS "${_host_elf_gen}")
    else()
        list(APPEND MOD_SRCS host_elf_symbols_generated.c)
    endif()
endif()

set(_plugin_sdk_gen_dir "${CMAKE_BINARY_DIR}/minihub_plugin_sdk_check")
set(_plugin_sdk_sizes_h "${_plugin_sdk_gen_dir}/plugin_sdk_sizes.h")
set(_plugin_sdk_abi_c "${_plugin_sdk_gen_dir}/plugin_sdk_abi_check.c")
set(_plugin_sdk_h "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk.h")

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    # Fail Host build if minihub_plugin_sdk.h drifts from allowlist / Host layouts.
    file(MAKE_DIRECTORY "${_plugin_sdk_gen_dir}")
    add_custom_command(
        OUTPUT "${_plugin_sdk_sizes_h}" "${_plugin_sdk_abi_c}"
        COMMAND python3 "${_mh_repo_for_sdk}/tools/check_plugin_sdk.py" emit-check
            --cc "${CMAKE_C_COMPILER}"
            --sdk "${_plugin_sdk_h}"
            --sdk-inc "${_mh_repo_for_sdk}/docs/module_sdk/include"
            --allowlist "${_mh_repo_for_sdk}/docs/module_sdk/host_export_allowlist.json"
            --sizes-out "${_plugin_sdk_sizes_h}"
            --check-out "${_plugin_sdk_abi_c}"
            --cflags "-DMH_PLUGIN_SDK_HOST_BUILD -I${CMAKE_BINARY_DIR}/config -include sdkconfig.h"
        DEPENDS
            "${CMAKE_BINARY_DIR}/config/sdkconfig.h"
            "${_plugin_sdk_h}"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/esp_idf.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/lwip.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/cjson.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/mbedtls.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/psa.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/freertos.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/include/minihub_plugin_sdk/minihub.h"
            "${_mh_repo_for_sdk}/docs/module_sdk/host_export_allowlist.json"
            "${_mh_repo_for_sdk}/tools/check_plugin_sdk.py"
        COMMENT "Check Store L3 plugin SDK vs Host IDF/MiniHub ABI"
        VERBATIM
    )
    set_source_files_properties("${_plugin_sdk_abi_c}" PROPERTIES GENERATED TRUE)
    list(APPEND MOD_SRCS "${_plugin_sdk_abi_c}")
endif()

idf_component_register(
    SRCS ${MOD_SRCS}
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES ${MOD_REQ}
)

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${_plugin_sdk_gen_dir}")
    target_compile_options(${COMPONENT_LIB} PRIVATE -fPIC)
    # elf_loader only exports INCLUDE_DIRS when CONFIG_ELF_LOADER is visible at
    # its first CMake pass; claw incremental builds can miss that. Force headers.
    idf_component_optional_requires(PRIVATE espressif__elf_loader)
    idf_component_get_property(_elf_dir espressif__elf_loader COMPONENT_DIR)
    if(_elf_dir)
        target_include_directories(${COMPONENT_LIB} PRIVATE "${_elf_dir}/include")
    endif()
    # Pull elf_loader dlfcn.o (strong dlopen) so QuickJS weak stubs do not win.
    if(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT)
        target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,dllist")
    endif()
endif()
