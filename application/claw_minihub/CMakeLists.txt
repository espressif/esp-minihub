cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

# ---------------------------------------------------------------------------
# esp-claw: git submodule at application/claw_minihub/components/esp-claw.
# Per-component git+path extracts a single subtree and breaks claw's internal
# relative path deps — keep the whole tree. Init with:
#   git submodule update --init application/claw_minihub/components/esp-claw
# ---------------------------------------------------------------------------
get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ESP_CLAW_DIR "${CMAKE_CURRENT_LIST_DIR}/components/esp-claw")
if(NOT EXISTS "${ESP_CLAW_DIR}/components")
    message(FATAL_ERROR
        "esp-claw submodule missing at ${ESP_CLAW_DIR}\n"
        "  git submodule update --init application/claw_minihub/components/esp-claw")
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_CLAW_DIR}/components"
    # Shared MiniHub Core (same repo)
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
    # MVP adapters only (avoid compiling every adapters/*)
    "${CMAKE_CURRENT_LIST_DIR}/../../components/adapters/adapter_zigbee"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/adapters/adapter_ble_mesh"
    # Store L3 plugins (e.g. miot) are loaded at runtime — not EXTRA_COMPONENT_DIRS.
)

# Host MVP: Claw owns Wi-Fi; MiniHub web attaches to the Claw portal httpd.
# Do not build LCD/Matter/USB/RTSP stacks.
set(EXCLUDE_COMPONENTS
    minihub_lcd
    minihub_matter
    minihub_rtsp_cam
    minihub_usb_msc
    adapter_matter
    adapter_modbus
    adapter_mqtt
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claw_minihub VERSION 0.1.0)

set(EDGE_AGENT_FATFS_IMAGE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/storage")
set(EDGE_AGENT_FATFS_BUILD_ROOT "${CMAKE_BINARY_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/system")
set(EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT "${CMAKE_BINARY_DIR}/system_fs_image")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
set(EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS "")

set(EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS "")

if(EXISTS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}" EDGE_AGENT_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(EDGE_AGENT_BOARD_PATH_LINE)
        list(GET EDGE_AGENT_BOARD_PATH_LINE 0 EDGE_AGENT_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            EDGE_AGENT_BOARD_PATH
            "${EDGE_AGENT_BOARD_PATH_LINE}"
        )

        set(EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT "${EDGE_AGENT_BOARD_PATH}/fatfs_image")

        if(EXISTS "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}")
            list(APPEND EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

set(EDGE_AGENT_MINIHUB_WEB_INDEX
    "${CMAKE_CURRENT_LIST_DIR}/../../components/minihub_web/web/index.html")

find_package(Python3 COMPONENTS Interpreter QUIET)
set(EDGE_AGENT_SEED_CONFIG_COMMANDS "")
if(Python3_FOUND)
    list(APPEND EDGE_AGENT_SEED_CONFIG_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${EDGE_AGENT_FATFS_BUILD_ROOT}/config"
        COMMAND ${CMAKE_COMMAND} -E env MINIHUB_APP=claw_minihub
                ${Python3_EXECUTABLE} "${MINIHUB_REPO_ROOT}/tools/product_assets.py"
                --repo "${MINIHUB_REPO_ROOT}" --app claw_minihub
                seed-config "${EDGE_AGENT_FATFS_BUILD_ROOT}/config"
    )
endif()

add_custom_target(edge_agent_prepare_fatfs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_BUILD_ROOT}/web"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${EDGE_AGENT_MINIHUB_WEB_INDEX}"
            "${EDGE_AGENT_FATFS_BUILD_ROOT}/web/index.html"
    ${EDGE_AGENT_SEED_CONFIG_COMMANDS}
    ${EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS}
    VERBATIM
)

# Prepare the read-only system partition build tree: mirror the source
# system_fs_image/ into a build-side staging dir that skill/lua syncs
# can also write into before fatfsgen packs it.
add_custom_target(edge_agent_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS}
    VERBATIM
)

fatfs_create_rawflash_image(system "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT)
fatfs_create_spiflash_image(storage "${EDGE_AGENT_FATFS_BUILD_ROOT}" FLASH_IN_PROJECT)

# Skill / Lua resources are now baked into the read-only system partition
# so storage can be reformatted at runtime without losing them.
skill_builder_configure_skill_sync(
    TARGET fatfs_system_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_system_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_system_bin edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_storage_bin edge_agent_prepare_fatfs_image)
