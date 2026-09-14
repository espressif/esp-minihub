
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

idf_component_register(
    SRCS ${MOD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${MOD_REQ}
)

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
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
