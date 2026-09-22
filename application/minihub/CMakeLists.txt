cmake_minimum_required(VERSION 3.16)

# Repo root (shared components/, config/, cmake/, tools/)
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
