# RainMaker bridge — do NOT gate REQUIRES/SRCS on CONFIG_* here.
# Component CMakeLists is evaluated during early requirement expansion when
# Kconfig values are unavailable; using CONFIG_MINIHUB_RAINMAKER left us with
# bridge sources (second pass) but without esp_rainmaker in REQUIRES (first pass).
#
# Gate on filesystem + MINIHUB_BUILD_RAINMAKER instead (same idea as minihub_matter).
set(_rmaker_srcs)
set(_rmaker_requires minihub_core espressif__cjson)

set(_rm_cmake
    "${CMAKE_CURRENT_LIST_DIR}/../../application/minihub/components/esp-rainmaker/components/esp_rainmaker/CMakeLists.txt")
set(_rm_sibling
    "${CMAKE_CURRENT_LIST_DIR}/../../../esp-rainmaker/components/esp_rainmaker/CMakeLists.txt")

set(_has_rm 0)
if(EXISTS "${_rm_cmake}")
    set(_has_rm 1)
elseif(EXISTS "${_rm_sibling}")
    set(_has_rm 1)
endif()

set(_rmaker_real 0)
if(_has_rm AND NOT "$ENV{MINIHUB_BUILD_RAINMAKER}" STREQUAL "0")
    # ENV=1 from build_firmware, or unset during board-manager cmake re-entry.
    # Claw builds leave the submodule absent → stub. Explicit ENV=0 → stub.
    set(_rmaker_real 1)
endif()

if(_rmaker_real)
    list(APPEND _rmaker_srcs rainmaker_node.c rainmaker_bridge.c)
    list(APPEND _rmaker_requires esp_rainmaker esp_event nvs_flash esp_hw_support minihub_ota)
    message(STATUS "minihub_rainmaker: linking esp_rainmaker (EXTRA tree present)")
else()
    list(APPEND _rmaker_srcs rainmaker_stub.c)
    message(STATUS "minihub_rainmaker: stub only")
endif()

idf_component_register(
    SRCS ${_rmaker_srcs}
    INCLUDE_DIRS include
    REQUIRES ${_rmaker_requires}
)
