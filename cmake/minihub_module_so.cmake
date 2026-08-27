# Build adapter components as .so for MiniHub Store (.mhub packaging).

function(minihub_add_module_so module_id component_name)
    if(NOT CONFIG_MINIHUB_DYNAMIC_MODULES)
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

    add_custom_command(OUTPUT ${so_output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_C_COMPILER} ${so_link_flags} -o ${so_output}
                -Wl,--whole-archive ${static_lib} -Wl,--no-whole-archive
                -Wl,--allow-shlib-undefined
        COMMAND ${CMAKE_COMMAND} -DOUT=${so_output}
                -P ${so_check_script}
        COMMAND ${CMAKE_STRIP_SO} ${so_strip_flags} ${so_output}
        DEPENDS ${so_dependencies}
        COMMENT "Build module .so: libadapter_${module_id}.so"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_custom_target(module_so_${module_id} ALL DEPENDS ${so_output})
endfunction()

function(minihub_register_module_sos)
    # Prefer repo-root store_modules (monorepo: application/<app> is CMAKE_SOURCE_DIR).
    set(_store_root "${CMAKE_SOURCE_DIR}/components/store_modules")
    if(DEFINED MINIHUB_REPO_ROOT AND EXISTS "${MINIHUB_REPO_ROOT}/components/store_modules")
        set(_store_root "${MINIHUB_REPO_ROOT}/components/store_modules")
    endif()
    file(GLOB _STORE_MOD_CMAKE "${_store_root}/*/CMakeLists.txt")
    foreach(_CMAKE ${_STORE_MOD_CMAKE})
        get_filename_component(_MOD_DIR ${_CMAKE} DIRECTORY)
        get_filename_component(_MOD ${_MOD_DIR} NAME)
        if(EXISTS "${_MOD_DIR}/module.json")
            minihub_add_module_so(${_MOD} ${_MOD})
            message(STATUS "store module .so target: ${_MOD}")
        endif()
    endforeach()
endfunction()
