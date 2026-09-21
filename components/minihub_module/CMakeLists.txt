
set(MOD_SRCS module_loader.c)
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
get_filename_component(_mh_repo_for_sdk "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
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
        DEPENDS
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
    INCLUDE_DIRS "include"
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
