# Build adapter components as .so for MiniHub Store (.mhub packaging).

function(minihub_add_module_so module_id component_name)
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
    set(_mh_catalog_py "${MINIHUB_REPO_ROOT}/tools/sync_store_catalog.py")
    set(_mh_python "${Python3_EXECUTABLE}")
    if(NOT _mh_python)
        set(_mh_python "python3")
    endif()
    # Configure-time staging may copy a stale deploy .mhub before libadapter_*.so
    # exists. After linking, always refresh MHMD + deploy .mhub + catalog size +
    # modstore_image so littlefs_modstore_bin (DEPENDS module_so_*) does not flash
    # an old ELF or a catalog size that disagrees with files/<id>.mhub.
    set(_mh_modstore_mhub "${CMAKE_BINARY_DIR}/modstore_image/files/${module_id}.mhub")
    set(_mh_modstore_catalog "${CMAKE_BINARY_DIR}/modstore_image/catalog.json")

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
        COMMAND ${CMAKE_COMMAND} -E env
                MINIHUB_PLUGINS_DEPLOY_DIR=${_mh_deploy}
                MINIHUB_PLUGIN_DIRS=$ENV{MINIHUB_PLUGIN_DIRS}
                MINIHUB_APP=$ENV{MINIHUB_APP}
                ${_mh_python} ${_mh_catalog_py} --deploy ${_mh_deploy}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_BINARY_DIR}/modstore_image/files
        COMMAND ${CMAKE_COMMAND} -E copy
                ${_mh_deploy}/${module_id}/${module_id}.mhub
                ${_mh_modstore_mhub}
        COMMAND ${CMAKE_COMMAND} -E copy
                ${_mh_deploy}/catalog.json
                ${_mh_modstore_catalog}
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
            continue()
        endif()
        minihub_add_module_so(${_MOD} ${_MOD})
        message(STATUS "store module .so target: ${_MOD}")
        # littlefs image is created in main/CMakeLists at configure time; sos
        # are registered later via DEFER — attach build-order deps here.
        if(TARGET littlefs_modstore_bin AND TARGET module_so_${_MOD})
            add_dependencies(littlefs_modstore_bin module_so_${_MOD})
        endif()
    endforeach()
endfunction()
