# Build adapter components as .so for MiniHub Store (.mhub packaging).

function(minihub_add_module_so module_id component_name)
    # Optional ARGV2: plugin source dir (module.json). Used to pick
    # Stores/ESP32S31 vs Stores/Common from that module's targets.
    if(NOT CONFIG_MINIHUB_DYNAMIC_MODULES)
        return()
    endif()
    # Same plugin id can appear via examples/plugins and MINIHUB_PLUGIN_DIRS; only one
    # module_so_<id> custom target is allowed.
    if(TARGET module_so_${module_id})
        return()
    endif()

    # Tight PT_LOAD vaddr alignment: default 4 KiB page size leaves a 4099-byte gap
    # between RX and RW segments; esp_elf_loader warns above 0x400 and S31 can fault
    # at minihub_module_entry. max-page-size=4 keeps separate RX/RW (unlike -Wl,-n RWX).
    set(so_link_flags -shared -fPIC -static-libgcc -nostdlib -nostartfiles
        -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden
        -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4
        -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded)

    set(out_dir "${CMAKE_BINARY_DIR}/modules")
    set(so_output "${out_dir}/libadapter_${module_id}.so")
    set(mhmd_output "${CMAKE_BINARY_DIR}/${module_id}.so")
    set(static_lib "${CMAKE_BINARY_DIR}/esp-idf/${component_name}/lib${component_name}.a")

    string(REPLACE "-elf-gcc" "-elf-strip" CMAKE_STRIP_SO ${CMAKE_C_COMPILER})
    set(so_strip_flags --strip-unneeded --remove-section=.comment --remove-section=.got.loc
        --remove-section=.dynamic)
    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        list(APPEND so_strip_flags --remove-section=.xt.lit --remove-section=.xt.prop
            --remove-section=.xtensa.info)
    elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
        list(APPEND so_strip_flags --remove-section=.riscv.attributes)
    endif()

    set(so_dependencies "")
    if(${CMAKE_GENERATOR} STREQUAL "Unix Makefiles")
        add_custom_command(OUTPUT so_${module_id}_dep
            COMMAND +${CMAKE_MAKE_PROGRAM} "__idf_${component_name}"
            COMMENT "Build component for module .so: ${component_name}"
        )
        list(APPEND so_dependencies "so_${module_id}_dep")
    else()
        list(APPEND so_dependencies "idf::${component_name}")
    endif()

    set(so_check_script "${CMAKE_SOURCE_DIR}/cmake/check_shared_object.cmake")
    if(DEFINED MINIHUB_REPO_ROOT AND EXISTS "${MINIHUB_REPO_ROOT}/cmake/check_shared_object.cmake")
        set(so_check_script "${MINIHUB_REPO_ROOT}/cmake/check_shared_object.cmake")
    endif()

    if(NOT MINIHUB_REPO_ROOT)
        get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
    endif()
    if(DEFINED ENV{MINIHUB_PLUGINS_DEPLOY_DIR} AND NOT "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}" STREQUAL "")
        set(_mh_deploy "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}")
    else()
        set(_mh_deploy "$ENV{HOME}/workspace/minihub_store")
    endif()
    set(_mh_pack_py "${MINIHUB_REPO_ROOT}/tools/pack_store_module.py")
    set(_mh_python "${Python3_EXECUTABLE}")
    if(NOT _mh_python)
        set(_mh_python "python3")
    endif()
    set(_mh_mod_dir "")
    if(ARGC GREATER 2)
        set(_mh_mod_dir "${ARGV2}")
    endif()
    set(_mh_mhub "${_mh_deploy}/Stores/Common/${module_id}/${module_id}.mhub")
    if(NOT "${_mh_mod_dir}" STREQUAL "")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env
                    MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}
                    ${_mh_python} -c
                    "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); import store_paths; print(store_paths.module_mhub_file(sys.argv[2], deploy=Path(sys.argv[3]), module_dir=Path(sys.argv[4])))"
                    "${MINIHUB_REPO_ROOT}/tools"
                    "${module_id}"
                    "${_mh_deploy}"
                    "${_mh_mod_dir}"
            OUTPUT_VARIABLE _mh_mhub
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _mh_mhub_rc
        )
        if(NOT _mh_mhub_rc EQUAL 0 OR "${_mh_mhub}" STREQUAL "")
            message(WARNING "modstore: could not resolve ${module_id} deploy path (rc=${_mh_mhub_rc})")
            set(_mh_mhub "${_mh_deploy}/Stores/Common/${module_id}/${module_id}.mhub")
        endif()
    endif()
    # Configure-time staging may copy a stale deploy .mhub before libadapter_*.so
    # exists. After linking, refresh MHMD + deploy .mhub + modstore files/<id>.mhub.
    # Do NOT rewrite/copy shared catalog.json here: ninja builds module_so_* in
    # parallel and concurrent cmake -E copy of the same catalog fails with
    # "Error copying file ... catalog.json". Catalog sync is module_store_catalog.
    set(_mh_modstore_mhub "${CMAKE_BINARY_DIR}/modstore_image/files/${module_id}.mhub")

    add_custom_command(OUTPUT ${so_output} ${mhmd_output} ${_mh_modstore_mhub}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_C_COMPILER} ${so_link_flags} -o ${so_output}
                -Wl,--whole-archive ${static_lib} -Wl,--no-whole-archive
                -Wl,--allow-shlib-undefined
        COMMAND ${CMAKE_COMMAND} -DOUT=${so_output}
                -P ${so_check_script}
        COMMAND ${CMAKE_STRIP_SO} ${so_strip_flags} ${so_output}
        COMMAND ${_mh_python} ${_mh_pack_py} mhmd ${so_output} ${mhmd_output}
        COMMAND ${CMAKE_COMMAND} -E env
                MINIHUB_PLUGINS_DEPLOY_DIR=${_mh_deploy}
                MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}
                MINIHUB_APP=$ENV{MINIHUB_APP}
                ${_mh_python} ${_mh_pack_py} ${module_id} --deploy ${_mh_deploy}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_BINARY_DIR}/modstore_image/files
        COMMAND ${CMAKE_COMMAND} -E copy
                ${_mh_mhub}
                ${_mh_modstore_mhub}
        DEPENDS ${so_dependencies}
        COMMENT "Build module .so + pack .mhub: ${module_id}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
    )
    add_custom_target(module_so_${module_id} ALL DEPENDS ${so_output} ${mhmd_output} ${_mh_modstore_mhub})
endfunction()

function(minihub_register_module_sos)
    # Build a .so per NATIVE (L3) plugin. Sources come from
    # examples/plugins/<id> and MINIHUB_PLUGIN_DIRS.
    # minihub_plugin_dirs already excludes L1/L2 (no CMakeLists); also skip
    # runtime js/pack here so a stray entry never creates libadapter_*.so /
    # idf::<id> ninja edges. L2 .mhub staging is configure-time only.
    minihub_plugin_dirs(_MOD_DIRS)
    set(_MH_MODULE_SO_TARGETS "")
    foreach(_MOD_DIR ${_MOD_DIRS})
        get_filename_component(_MOD ${_MOD_DIR} NAME)
        if(_MOD MATCHES "^_")
            continue()
        endif()
        if(NOT EXISTS "${_MOD_DIR}/module.json")
            continue()
        endif()
        if(NOT EXISTS "${_MOD_DIR}/CMakeLists.txt")
            continue()
        endif()
        # Skip L1/L2 even if someone added an empty CMakeLists by mistake.
        file(READ "${_MOD_DIR}/module.json" _MH_MOD_JSON)
        if(_MH_MOD_JSON MATCHES "\"runtime\"[ \t\r\n]*:[ \t\r\n]*\"js\"" OR
           _MH_MOD_JSON MATCHES "\"runtime\"[ \t\r\n]*:[ \t\r\n]*\"pack\"")
            continue()
        endif()
        if(TARGET module_so_${_MOD})
            list(APPEND _MH_MODULE_SO_TARGETS module_so_${_MOD})
            continue()
        endif()
        minihub_add_module_so(${_MOD} ${_MOD} ${_MOD_DIR})
        message(STATUS "store module .so target: ${_MOD}")
        list(APPEND _MH_MODULE_SO_TARGETS module_so_${_MOD})
    endforeach()

    # One catalog rewrite after all packs finish — sizes match files/*.mhub and
    # ninja no longer races on the shared catalog.json.
    if(NOT MINIHUB_REPO_ROOT)
        get_filename_component(MINIHUB_REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
    endif()
    if(DEFINED ENV{MINIHUB_PLUGINS_DEPLOY_DIR} AND NOT "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}" STREQUAL "")
        set(_mh_deploy "$ENV{MINIHUB_PLUGINS_DEPLOY_DIR}")
    else()
        set(_mh_deploy "$ENV{HOME}/workspace/minihub_store")
    endif()
    set(_mh_python "${Python3_EXECUTABLE}")
    if(NOT _mh_python)
        set(_mh_python "python3")
    endif()
    set(_mh_catalog_py "${MINIHUB_REPO_ROOT}/tools/sync_store_catalog.py")
    set(_mh_modstore_catalog "${CMAKE_BINARY_DIR}/modstore_image/catalog.json")

    if(NOT TARGET module_store_catalog)
        add_custom_command(OUTPUT ${_mh_modstore_catalog}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_BINARY_DIR}/modstore_image
            COMMAND ${CMAKE_COMMAND} -E env
                    MINIHUB_PLUGINS_DEPLOY_DIR=${_mh_deploy}
                    MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}
                    MINIHUB_APP=$ENV{MINIHUB_APP}
                    ${_mh_python} ${_mh_catalog_py} --deploy ${_mh_deploy}
                    --aggregate ${_mh_modstore_catalog}
            DEPENDS ${_MH_MODULE_SO_TARGETS}
            COMMENT "Sync Store catalog into modstore_image"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            VERBATIM
        )
        add_custom_target(module_store_catalog ALL DEPENDS ${_mh_modstore_catalog})
    endif()

    if(TARGET littlefs_modstore_bin)
        foreach(_t ${_MH_MODULE_SO_TARGETS})
            add_dependencies(littlefs_modstore_bin ${_t})
        endforeach()
        if(TARGET module_store_catalog)
            add_dependencies(littlefs_modstore_bin module_store_catalog)
        endif()
    endif()
endfunction()
