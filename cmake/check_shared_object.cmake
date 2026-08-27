if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "Error: ${OUT} was not created")
endif()

file(SIZE "${OUT}" SIZE_VAR)
if(SIZE_VAR EQUAL 0)
    message(FATAL_ERROR "Error: ${OUT} is empty")
endif()

set(_layout_script "${CMAKE_SOURCE_DIR}/tools/check_module_so_layout.py")
if(DEFINED MINIHUB_REPO_ROOT AND EXISTS "${MINIHUB_REPO_ROOT}/tools/check_module_so_layout.py")
    set(_layout_script "${MINIHUB_REPO_ROOT}/tools/check_module_so_layout.py")
endif()
if(EXISTS "${_layout_script}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} "${_layout_script}" --warn-only "${OUT}"
            RESULT_VARIABLE _layout_rc
        )
        if(NOT _layout_rc EQUAL 0)
            message(WARNING "Module .so PT_LOAD layout check failed for ${OUT}")
        endif()
    endif()
endif()
