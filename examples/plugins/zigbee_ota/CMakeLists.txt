if(ESP_PLATFORM)
    idf_component_register(
        SRCS "module_entry.c" "zigbee_ota_adapter.c"
        INCLUDE_DIRS "."
        REQUIRES minihub_adapter minihub_core minihub_module minihub_net esp_http_client
    )
    if(CONFIG_MINIHUB_DYNAMIC_MODULES)
        target_compile_options(${COMPONENT_LIB} PRIVATE -fPIC)
    endif()
else()
    cmake_minimum_required(VERSION 3.16)
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    project(zigbee_ota C)
    get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
    set(MINIHUB_MODULE_SRCS module_entry.c zigbee_ota_adapter.c)
    include(${MINIHUB_REPO_ROOT}/cmake/minihub_plugin_standalone.cmake)
endif()
