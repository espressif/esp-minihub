get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
)

# Match minihub: CHIP/esp-matter C++ needs -DCHIP_HAVE_CONFIG_H (-fno-rtti scoped).
include(${MINIHUB_REPO_ROOT}/cmake/matter_no_rtti.cmake)
