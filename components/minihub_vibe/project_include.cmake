# Glue: build the Vibe Mode factory image as a nested IDF project (same role as
# components/bootloader/project_include.cmake for bootloader.bin).
#
# The Host links minihub_vibe as a normal component (stage / pending APIs). The
# retained app,factory binary is produced here into ${BUILD_DIR}/vibe_mode/.

if(VIBE_MODE_BUILD)
    return()
endif()

idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(python PYTHON)
idf_build_get_property(project_dir PROJECT_DIR)

set(VIBE_MODE_BUILD_DIR "${build_dir}/vibe_mode")
set(VIBE_MODE_BIN "${VIBE_MODE_BUILD_DIR}/vibe_mode.bin")
set(VIBE_MODE_ELF "${VIBE_MODE_BUILD_DIR}/vibe_mode.elf")

set(_vibe_builder "${CMAKE_CURRENT_LIST_DIR}/../../tools/build_vibe_mode.py")
set(_vibe_partitions "${project_dir}/partitions.csv")

if(NOT EXISTS "${_vibe_partitions}")
    message(WARNING "minihub_vibe: no ${_vibe_partitions}; skipping vibe_mode target")
    return()
endif()

# Parse once so products without a vibe_mode factory row do not pay for a build.
file(STRINGS "${_vibe_partitions}" _vibe_part_lines)
set(_vibe_has_factory OFF)
foreach(_line IN LISTS _vibe_part_lines)
    if(_line MATCHES "^[ \t]*#" OR _line STREQUAL "")
        continue()
    endif()
    if(_line MATCHES "^[ \t]*vibe_mode[ \t]*,.*factory")
        set(_vibe_has_factory ON)
        break()
    endif()
endforeach()

if(NOT _vibe_has_factory)
    return()
endif()

add_custom_target(vibe_mode ALL
    COMMAND ${python} "${_vibe_builder}"
            --build-dir "${VIBE_MODE_BUILD_DIR}"
            --partitions "${_vibe_partitions}"
            --src-dir "${CMAKE_CURRENT_LIST_DIR}/subproject"
    BYPRODUCTS "${VIBE_MODE_BIN}" "${VIBE_MODE_ELF}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    USES_TERMINAL
    VERBATIM
    COMMENT "Building Vibe Mode factory image (components/minihub_vibe/subproject)"
)

# Host app_check_size is ota_0-only; this checks vibe_mode.bin against factory.
# The `app` target does not exist yet during project_include — attach later.
partition_table_add_check_size_target(vibe_mode_check_size
    DEPENDS vibe_mode
    BINARY_PATH "${VIBE_MODE_BIN}"
    PARTITION_TYPE app
    PARTITION_SUBTYPE factory)

function(minihub_vibe_attach_size_check)
    if(TARGET app AND TARGET vibe_mode_check_size)
        add_dependencies(app vibe_mode_check_size)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${project_dir}" CALL minihub_vibe_attach_size_check)

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY
    ADDITIONAL_CLEAN_FILES
    "${VIBE_MODE_BIN}" "${VIBE_MODE_ELF}")
