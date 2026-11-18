cmake_minimum_required(VERSION 3.16)

# Vibe Mode factory image — second IDF project nested under minihub_vibe.
# One Host project emits one app image; this subproject emits the retained
# app,factory image that copies staged firmware into ota_0.
#
# Do NOT point EXTRA_COMPONENT_DIRS at the repo's components/ tree. The
# component manager would parse every Host component manifest and drag in
# elf_loader / mosquitto / esp_rainmaker, whose `if` clauses reference
# MINIHUB_* Kconfig symbols that do not exist in this minimal project.
# Shared media/state/apply sources are compiled straight into main/ instead.
#
# Built from the Host via components/minihub_vibe/project_include.cmake
# (same idea as the IDF bootloader subproject).

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vibe_mode)
