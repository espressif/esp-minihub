# Standalone (no ESP-IDF) build for MiniHub Store L3 native plugins.
#
# Include from the plugin directory's CMakeLists.txt. Defaults:
#   MINIHUB_MODULE_ID    — name of that directory (override if it differs from module.json id)
#   MINIHUB_MODULE_SRCS  — every *.c and *.cpp in that directory (not subfolders)
#
# Optional:
#   MINIHUB_REPO_ROOT     — esp-minihub checkout (demos); used to find tools +
#                           default SDK include under docs/module_sdk/include
#   MINIHUB_SDK_INCLUDE   — Plugin SDK include dir (overrides repo default)
#   MINIHUB_MHMD_OUT      — full path for MHMD .so output
#   MINIHUB_PACK_PY       — path to pack_store_module.py
#   MINIHUB_GEN_DEVICES_PY — path to generate_devices_property_from_json.py
#   MINIHUB_MODULE_DIR    — plugin directory if it is not CMAKE_CURRENT_SOURCE_DIR
#
# Artifacts (this file is standalone-only):
#   ${CMAKE_BINARY_DIR}/lib/<id>.so  — linked ELF (intermediate)
#   ${CMAKE_BINARY_DIR}/<id>.so      — MHMD-wrapped .so (pack input)
# Prefer cmake -B <plugin>/build so outputs stay under the plugin tree.
# Host in-tree builds still write application/<app>/build/<id>.so via
# cmake/minihub_module_so.cmake (not this fragment).
#
# Third-party: copy this file into the plugin directory, set MINIHUB_SDK_INCLUDE
# to your unzipped module_sdk/include, and point MINIHUB_PACK_PY at
# pack_store_module.py if you want the mhmd/pack targets.

set(_MH_MOD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(DEFINED MINIHUB_MODULE_DIR AND NOT MINIHUB_MODULE_DIR STREQUAL "")
    set(_MH_MOD_DIR "${MINIHUB_MODULE_DIR}")
endif()

if(NOT DEFINED MINIHUB_MODULE_ID OR MINIHUB_MODULE_ID STREQUAL "")
    get_filename_component(MINIHUB_MODULE_ID "${_MH_MOD_DIR}" NAME)
endif()

if(NOT DEFINED MINIHUB_MODULE_SRCS OR MINIHUB_MODULE_SRCS STREQUAL "")
    file(GLOB MINIHUB_MODULE_SRCS CONFIGURE_DEPENDS
        "${_MH_MOD_DIR}/*.c"
        "${_MH_MOD_DIR}/*.cpp")
endif()
if(NOT MINIHUB_MODULE_SRCS)
    message(FATAL_ERROR
        "minihub_plugin_standalone: no .c/.cpp under ${_MH_MOD_DIR}")
endif()

set(_MH_NEED_CXX FALSE)
foreach(_mh_src ${MINIHUB_MODULE_SRCS})
    if(_mh_src MATCHES "\\.cpp$")
        set(_MH_NEED_CXX TRUE)
    endif()
endforeach()
if(_MH_NEED_CXX)
    enable_language(CXX)
endif()

if(NOT DEFINED MINIHUB_REPO_ROOT OR MINIHUB_REPO_ROOT STREQUAL "")
    # When copied next to a lone plugin, leave empty; require MINIHUB_SDK_INCLUDE.
    set(MINIHUB_REPO_ROOT "")
endif()

if(NOT DEFINED MINIHUB_SDK_INCLUDE OR MINIHUB_SDK_INCLUDE STREQUAL "")
    if(MINIHUB_REPO_ROOT)
        set(MINIHUB_SDK_INCLUDE "${MINIHUB_REPO_ROOT}/docs/module_sdk/include")
    else()
        message(FATAL_ERROR
            "minihub_plugin_standalone: set MINIHUB_SDK_INCLUDE (Plugin SDK include dir)")
    endif()
endif()

if(NOT EXISTS "${MINIHUB_SDK_INCLUDE}/minihub_plugin_sdk.h")
    message(FATAL_ERROR
        "minihub_plugin_standalone: missing minihub_plugin_sdk.h under ${MINIHUB_SDK_INCLUDE}")
endif()

# Generic / bare-metal toolchains report no shared libs; we still link with -shared.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_MODULE_PREFIX "")
set(CMAKE_SHARED_MODULE_SUFFIX ".so")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_EXECUTABLE)
    set(_MH_PYTHON "${Python3_EXECUTABLE}")
else()
    set(_MH_PYTHON "python3")
endif()

if(NOT DEFINED MINIHUB_PACK_PY OR MINIHUB_PACK_PY STREQUAL "")
    if(MINIHUB_REPO_ROOT)
        set(MINIHUB_PACK_PY "${MINIHUB_REPO_ROOT}/tools/pack_store_module.py")
    endif()
endif()
if(NOT DEFINED MINIHUB_GEN_DEVICES_PY OR MINIHUB_GEN_DEVICES_PY STREQUAL "")
    if(MINIHUB_REPO_ROOT)
        set(MINIHUB_GEN_DEVICES_PY
            "${MINIHUB_REPO_ROOT}/tools/generate_devices_property_from_json.py")
    endif()
endif()

if(DEFINED ENV{MINIHUB_APP} AND NOT "$ENV{MINIHUB_APP}" STREQUAL "")
    set(_MH_APP "$ENV{MINIHUB_APP}")
else()
    set(_MH_APP "minihub")
endif()

# Linked ELF and MHMD cannot share one path (mhmd reads then writes).
# Final deliverable is always <id>.so under the cmake build dir (plugin/build/).
if(NOT DEFINED MINIHUB_MHMD_OUT OR MINIHUB_MHMD_OUT STREQUAL "")
    set(MINIHUB_MHMD_OUT "${CMAKE_BINARY_DIR}/${MINIHUB_MODULE_ID}.so")
endif()
set(_MH_RAW_DIR "${CMAKE_BINARY_DIR}/lib")

# devices.json → device_desc.gen.h (when present)
set(_MH_DEVICES_JSON "${_MH_MOD_DIR}/devices.json")
set(_MH_DEVICES_GEN "${_MH_MOD_DIR}/device_desc.gen.h")
set(_MH_GEN_DEPS "")
if(EXISTS "${_MH_DEVICES_JSON}")
    if(NOT MINIHUB_GEN_DEVICES_PY OR NOT EXISTS "${MINIHUB_GEN_DEVICES_PY}")
        message(FATAL_ERROR
            "minihub_plugin_standalone: devices.json present but "
            "MINIHUB_GEN_DEVICES_PY missing (${MINIHUB_GEN_DEVICES_PY})")
    endif()
    add_custom_command(
        OUTPUT "${_MH_DEVICES_GEN}"
        COMMAND ${_MH_PYTHON} "${MINIHUB_GEN_DEVICES_PY}"
                "${_MH_DEVICES_JSON}" "${_MH_DEVICES_GEN}"
        DEPENDS "${_MH_DEVICES_JSON}" "${MINIHUB_GEN_DEVICES_PY}"
        COMMENT "Generate device_desc.gen.h for ${MINIHUB_MODULE_ID}"
        VERBATIM
    )
    list(APPEND _MH_GEN_DEPS "${_MH_DEVICES_GEN}")
endif()

add_library(${MINIHUB_MODULE_ID} SHARED ${MINIHUB_MODULE_SRCS} ${_MH_GEN_DEPS})
set_target_properties(${MINIHUB_MODULE_ID} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${MINIHUB_MODULE_ID}"
    LIBRARY_OUTPUT_DIRECTORY "${_MH_RAW_DIR}"
)

target_compile_definitions(${MINIHUB_MODULE_ID} PRIVATE
    MINIHUB_PLUGIN_STANDALONE
    CONFIG_MINIHUB_DYNAMIC_MODULES=1
)

target_include_directories(${MINIHUB_MODULE_ID} PRIVATE
    "${MINIHUB_SDK_INCLUDE}"
    "${_MH_MOD_DIR}"
)

target_compile_options(${MINIHUB_MODULE_ID} PRIVATE
    -std=gnu17
    -fPIC
    -fdata-sections
    -ffunction-sections
    -fvisibility=hidden
)

# Keep / export the loader entry (hidden + --gc-sections would drop it otherwise).
set_source_files_properties(
    "${_MH_MOD_DIR}/module_entry.c"
    PROPERTIES COMPILE_FLAGS "-fvisibility=default"
)

# Match docs/en/plugins/develop_l3.rst and cmake/minihub_module_so.cmake
set(_MH_LINK_FLAGS
    "-shared -fPIC -nostdlib -nostartfiles -fdata-sections -ffunction-sections -fvisibility=hidden -Wl,--gc-sections -Wl,--allow-shlib-undefined -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 -Wl,-u,minihub_module_entry"
)
set_target_properties(${MINIHUB_MODULE_ID} PROPERTIES
    LINK_FLAGS "${_MH_LINK_FLAGS}"
)

get_filename_component(_MH_MHMD_DIR "${MINIHUB_MHMD_OUT}" DIRECTORY)

if(MINIHUB_PACK_PY AND EXISTS "${MINIHUB_PACK_PY}")
    add_custom_command(
        OUTPUT "${MINIHUB_MHMD_OUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_MH_MHMD_DIR}"
        COMMAND ${_MH_PYTHON} "${MINIHUB_PACK_PY}" mhmd
                "$<TARGET_FILE:${MINIHUB_MODULE_ID}>" "${MINIHUB_MHMD_OUT}"
        DEPENDS ${MINIHUB_MODULE_ID}
        COMMENT "MHMD wrap ${MINIHUB_MODULE_ID}.so"
        VERBATIM
    )
    add_custom_target(mhmd ALL DEPENDS "${MINIHUB_MHMD_OUT}")

    add_custom_target(pack
        COMMAND ${CMAKE_COMMAND} -E env
                "MINIHUB_APP=${_MH_APP}"
                "MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}"
                ${_MH_PYTHON} "${MINIHUB_PACK_PY}" pack "${MINIHUB_MODULE_ID}"
        DEPENDS mhmd
        COMMENT "pack ${MINIHUB_MODULE_ID}.mhub"
        VERBATIM
    )
else()
    # No pack script: still build the shared object as the default artifact.
    add_custom_command(
        OUTPUT "${MINIHUB_MHMD_OUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_MH_MHMD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:${MINIHUB_MODULE_ID}>" "${MINIHUB_MHMD_OUT}"
        DEPENDS ${MINIHUB_MODULE_ID}
        COMMENT "Copy ${MINIHUB_MODULE_ID}.so (no pack_store_module.py; not MHMD)"
        VERBATIM
    )
    add_custom_target(mhmd ALL DEPENDS "${MINIHUB_MHMD_OUT}")
    message(WARNING
        "minihub_plugin_standalone: MINIHUB_PACK_PY unset — "
        "output is a raw .so copy, not MHMD-wrapped")
endif()

message(STATUS "minihub standalone plugin: ${MINIHUB_MODULE_ID}")
message(STATUS "  SDK include: ${MINIHUB_SDK_INCLUDE}")
message(STATUS "  linked ELF:  ${_MH_RAW_DIR}/${MINIHUB_MODULE_ID}.so")
message(STATUS "  MHMD out:    ${MINIHUB_MHMD_OUT}")
