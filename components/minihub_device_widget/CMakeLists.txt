# model/*.csv is the source of truth. Flatten into an embedded CSV C file in
# this component's binary dir at build time — never commit a components/.../generated/
# tree. Runtime still prefers /data/config/devices.csv when present on littlefs;
# the embedded string is the firmware fallback and seeds that path at boot.
idf_component_register(
    SRCS
        "src/minihub_device_widget.c"
        "src/minihub_device_widget_model.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        espressif__cjson
)

# ESP-IDF scans component CMakeLists in script mode for REQUIRES first;
# generation / find_package must not run in that pass.
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MINIHUB_DW_GEN_C "${CMAKE_CURRENT_BINARY_DIR}/minihub_device_widget_gen.c")
set(MINIHUB_DW_GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_device_widget.py")
set(MINIHUB_DW_MODEL_CSV
    "${CMAKE_CURRENT_SOURCE_DIR}/model/devices.csv"
    "${CMAKE_CURRENT_SOURCE_DIR}/model/services.csv"
    "${CMAKE_CURRENT_SOURCE_DIR}/model/parameters.csv"
    "${CMAKE_CURRENT_SOURCE_DIR}/model/ui_elements.csv"
)

add_custom_command(
    OUTPUT "${MINIHUB_DW_GEN_C}"
           "${CMAKE_CURRENT_BINARY_DIR}/devices.csv"
           "${CMAKE_CURRENT_BINARY_DIR}/ui_controls.json"
    COMMAND ${Python3_EXECUTABLE} "${MINIHUB_DW_GEN_SCRIPT}"
            --out-dir "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS ${MINIHUB_DW_MODEL_CSV} "${MINIHUB_DW_GEN_SCRIPT}"
    COMMENT "Generating minihub_device_widget embedded registry from model/*.csv"
    VERBATIM
)

add_custom_target(minihub_device_widget_gen DEPENDS "${MINIHUB_DW_GEN_C}")
target_sources(${COMPONENT_LIB} PRIVATE "${MINIHUB_DW_GEN_C}")
add_dependencies(${COMPONENT_LIB} minihub_device_widget_gen)
