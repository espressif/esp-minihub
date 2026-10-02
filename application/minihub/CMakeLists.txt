cmake_minimum_required(VERSION 3.16)

# Repo root (shared components/, configs/, cmake/, tools/)
get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Shared Core + adapters. Matter SDK dirs: ESP_MATTER_PATH (opt-out MINIHUB_BUILD_MATTER=0).
# L3 Store plugins: MINIHUB_PLUGIN_DIRS (not a Host builtin tree). Skip .so graph:
# set(MINIHUB_IMPORT_STORE_MODULES_DISABLED ON) before include.
include("${MINIHUB_REPO_ROOT}/cmake/minihub_bundle/minihub_import.cmake")

# RainMaker SDK: components/minihub_rainmaker/idf_component.yml
# (espressif/esp_rainmaker when CONFIG_MINIHUB_RAINMAKER=y). Do not add the
# Host git submodule to EXTRA_COMPONENT_DIRS — that duplicates the registry
# package and breaks products that import Core as a submodule.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
minihub_register_kconfig()
project(minihub)

# RainMaker's managed esp_rainmaker calls esp_sha(). The project runs mbedTLS in
# soft-crypto mode (CONFIG_MBEDTLS_HARDWARE_SHA unset: HW SHA/AES DMA hangs
# while the RGB LCD GDMA is active), and current IDF only builds its own
# esp_sha() under hardware SHA, so the symbol comes from
# components/minihub_rainmaker/esp_sha_compat.c (a weak, always-compiled
# software implementation). Force the linker to pull that archive member:
# esp_rainmaker is linked before minihub_rainmaker, and the member would
# otherwise be a candidate for dropping -> "undefined reference to esp_sha".
# This mirrors IDF's own -u include_esp_phy_override / -u dllist handling.
# The compat symbol is weak, so a real hardware esp_sha() (if the IDF provides
# one) still overrides it and forcing the symbol cannot create a duplicate.
if(CONFIG_MINIHUB_RAINMAKER)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-u,esp_sha")
endif()
