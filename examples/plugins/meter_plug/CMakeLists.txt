if(ESP_PLATFORM)
    idf_component_register(
        SRCS "module_entry.c" "meter_plug_adapter.c"
        INCLUDE_DIRS "."
        REQUIRES minihub_adapter minihub_core minihub_module
    )
    if(CONFIG_MINIHUB_DYNAMIC_MODULES)
        target_compile_options(${COMPONENT_LIB} PRIVATE -fPIC)
    endif()
else()
    cmake_minimum_required(VERSION 3.16)
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    project(meter_plug C)
    get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
    include(${MINIHUB_REPO_ROOT}/cmake/minihub_plugin_standalone.cmake)
endif()
