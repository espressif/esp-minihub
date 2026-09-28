# Product config + Store preinstall helpers (application/<app>/manifest.yml).
# Requires Python3_EXECUTABLE and MINIHUB_REPO_ROOT.

function(minihub_product_app_name out_var)
    set(_app "$ENV{MINIHUB_APP}")
    if(NOT _app AND DEFINED MINIHUB_REPO_ROOT AND EXISTS "${MINIHUB_REPO_ROOT}/.minihub_app")
        file(READ "${MINIHUB_REPO_ROOT}/.minihub_app" _app)
        string(STRIP "${_app}" _app)
    endif()
    if(NOT _app)
        set(_app "minihub")
    endif()
    set(${out_var} "${_app}" PARENT_SCOPE)
endfunction()

# Resolve application/<app>/configs/<name> else configs/<name> → absolute path in out_var.
# Empty string if missing.
function(minihub_resolve_config_asset name out_var)
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "minihub_resolve_config_asset: Python3_EXECUTABLE required")
    endif()
    minihub_product_app_name(_app)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/product_assets.py
                --repo ${MINIHUB_REPO_ROOT} --app ${_app} resolve ${name}
        WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
        OUTPUT_VARIABLE _path
        RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _err
    )
    if(NOT _rc EQUAL 0)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()

# CMake list of module ids from application/<app>/manifest.yml store_preinstall.
function(minihub_store_preinstall_list out_var)
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "minihub_store_preinstall_list: Python3_EXECUTABLE required")
    endif()
    minihub_product_app_name(_app)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MINIHUB_REPO_ROOT}/tools/product_assets.py
                --repo ${MINIHUB_REPO_ROOT} --app ${_app} store-preinstall --format cmake
        WORKING_DIRECTORY ${MINIHUB_REPO_ROOT}
        OUTPUT_VARIABLE _ids
        RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _err
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "store-preinstall failed for app=${_app}: ${_err}")
    endif()
    set(${out_var} "${_ids}" PARENT_SCOPE)
endfunction()
