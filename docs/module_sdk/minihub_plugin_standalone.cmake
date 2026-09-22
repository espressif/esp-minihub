# Standalone (no ESP-IDF, no MiniHub tree) build for Store L3 native plugins.
#
# Ship this file inside module_sdk.zip next to include/ and pack_*.py.
# Third-party / plugins CI: unzip module_sdk.zip and point MINIHUB_MODULE_SDK_PATH
# at that directory — do not clone esp-minihub.
#
# Include from the plugin CMakeLists.txt after setting:
#   MINIHUB_MODULE_SRCS  — source list (required; same files as idf_component_register SRCS)
#   MINIHUB_MODULE_ID    — optional; defaults to the plugin directory name
#
# Optional:
#   MINIHUB_SDK_INCLUDE   — default: ${CMAKE_CURRENT_LIST_DIR}/include
#   MINIHUB_MHMD_OUT      — full path for MHMD .so output
#   MINIHUB_PACK_PY       — default: ${CMAKE_CURRENT_LIST_DIR}/pack_store_module.py
#
# Artifacts:
#   ${CMAKE_BINARY_DIR}/lib/<id>.so  — linked ELF (intermediate)
#   ${CMAKE_BINARY_DIR}/<id>.so      — MHMD-wrapped .so (pack input)

if(NOT DEFINED MINIHUB_MODULE_ID OR MINIHUB_MODULE_ID STREQUAL "")
    get_filename_component(MINIHUB_MODULE_ID "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
endif()

if(NOT DEFINED MINIHUB_MODULE_SRCS OR MINIHUB_MODULE_SRCS STREQUAL "")
    message(FATAL_ERROR
        "minihub_plugin_standalone: set MINIHUB_MODULE_SRCS in the plugin CMakeLists.txt")
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

if(NOT DEFINED MINIHUB_SDK_INCLUDE OR MINIHUB_SDK_INCLUDE STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/include/minihub_plugin_sdk.h")
        set(MINIHUB_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/include")
    else()
        message(FATAL_ERROR
            "minihub_plugin_standalone: missing include/ next to this cmake "
            "(unzip module_sdk.zip and set MINIHUB_MODULE_SDK_PATH)")
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

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_EXECUTABLE)
    set(_MH_PYTHON "${Python3_EXECUTABLE}")
else()
    set(_MH_PYTHON "python3")
endif()

if(NOT DEFINED MINIHUB_PACK_PY OR MINIHUB_PACK_PY STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/pack_store_module.py")
        set(MINIHUB_PACK_PY "${CMAKE_CURRENT_LIST_DIR}/pack_store_module.py")
    endif()
endif()

# Linked ELF and MHMD cannot share one path (mhmd reads then writes).
if(NOT DEFINED MINIHUB_MHMD_OUT OR MINIHUB_MHMD_OUT STREQUAL "")
    set(MINIHUB_MHMD_OUT "${CMAKE_BINARY_DIR}/${MINIHUB_MODULE_ID}.so")
endif()
set(_MH_RAW_DIR "${CMAKE_BINARY_DIR}/lib")

add_library(${MINIHUB_MODULE_ID} SHARED ${MINIHUB_MODULE_SRCS})
set_target_properties(${MINIHUB_MODULE_ID} PROPERTIES
    OUTPUT_NAME "${MINIHUB_MODULE_ID}"
    LIBRARY_OUTPUT_DIRECTORY "${_MH_RAW_DIR}"
)

target_compile_definitions(${MINIHUB_MODULE_ID} PRIVATE
    MINIHUB_PLUGIN_STANDALONE
    CONFIG_MINIHUB_DYNAMIC_MODULES=1
)

target_include_directories(${MINIHUB_MODULE_ID} PRIVATE
    "${MINIHUB_SDK_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/module_entry.c"
    PROPERTIES COMPILE_FLAGS "-fvisibility=default"
)

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
                "MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}"
                ${_MH_PYTHON} "${MINIHUB_PACK_PY}" pack "${MINIHUB_MODULE_ID}"
        DEPENDS mhmd
        COMMENT "pack ${MINIHUB_MODULE_ID}.mhub"
        VERBATIM
    )
else()
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
        "minihub_plugin_standalone: pack_store_module.py missing next to this cmake — "
        "output is a raw .so copy, not MHMD-wrapped")
endif()

message(STATUS "minihub standalone plugin: ${MINIHUB_MODULE_ID}")
message(STATUS "  SDK include: ${MINIHUB_SDK_INCLUDE}")
message(STATUS "  linked ELF:  ${_MH_RAW_DIR}/${MINIHUB_MODULE_ID}.so")
message(STATUS "  MHMD out:    ${MINIHUB_MHMD_OUT}")
