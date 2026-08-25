cmake_minimum_required(VERSION 3.16)

# Repo root (shared components/, config/, cmake/, tools/)
get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Matter/esp-matter (optional). Enable: MINIHUB_BUILD_MATTER=1 ESP_MATTER_PATH=...
# ESP32-S31 commissioner: see tools/build_matter.sh and config/sdkconfig/feature/matter*.defaults
if(DEFINED ENV{ESP_MATTER_PATH} AND "$ENV{MINIHUB_BUILD_MATTER}" STREQUAL "1")
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
endif()

# Shared Core + nested adapters / store_modules (IDF does not recurse under components/)
list(APPEND EXTRA_COMPONENT_DIRS
    "${MINIHUB_REPO_ROOT}/components"
    "${MINIHUB_REPO_ROOT}/components/adapters"
    "${MINIHUB_REPO_ROOT}/components/store_modules")

# Claw-only facades (cap_mcp_server / MVP aggregator) — not part of public Host.
set(EXCLUDE_COMPONENTS
    minihub_mcp_server
    minihub_mvp
)

# RainMaker: git submodule at application/minihub/components/esp-rainmaker.
# Sibling ../esp-rainmaker still works as a fallback for local overrides.
# Record MINIHUB_HAS_ESP_RAINMAKER in the CMake cache so board-manager / later
# cmake re-entry still sees the tree even if MINIHUB_BUILD_RAINMAKER is unset
# in that subprocess. Real vs stub is gated in components/minihub_rainmaker.
#   git submodule update --init application/minihub/components/esp-rainmaker
set(_MH_RM_SUBMODULE "${CMAKE_CURRENT_LIST_DIR}/components/esp-rainmaker/components")
set(_MH_RM_SIBLING "${MINIHUB_REPO_ROOT}/../esp-rainmaker/components")
set(_MH_RM_HAS 0)
if(EXISTS "${_MH_RM_SUBMODULE}/esp_rainmaker")
    list(APPEND EXTRA_COMPONENT_DIRS "${_MH_RM_SUBMODULE}")
    set(_MH_RM_HAS 1)
elseif(EXISTS "${_MH_RM_SIBLING}/esp_rainmaker")
    list(APPEND EXTRA_COMPONENT_DIRS "${_MH_RM_SIBLING}")
    set(_MH_RM_HAS 1)
elseif(DEFINED ENV{MINIHUB_BUILD_RAINMAKER} AND "$ENV{MINIHUB_BUILD_RAINMAKER}" STREQUAL "1")
    message(WARNING
        "MINIHUB_BUILD_RAINMAKER=1 but esp-rainmaker missing; stub only\n"
        "  git submodule update --init application/minihub/components/esp-rainmaker")
endif()
set(MINIHUB_HAS_ESP_RAINMAKER "${_MH_RM_HAS}" CACHE INTERNAL "esp-rainmaker on EXTRA_COMPONENT_DIRS")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# RISC-V gcc + -Werror: CHIP/esp_matter_controller need toolchain relaxations in merged build.
if(DEFINED ENV{MINIHUB_BUILD_MATTER} AND "$ENV{MINIHUB_BUILD_MATTER}" STREQUAL "1")
    idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format;-Wno-error=unused-result" APPEND)
endif()

project(minihub)

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    include(${MINIHUB_REPO_ROOT}/cmake/minihub_module_so.cmake)
    minihub_register_module_sos()
endif()
