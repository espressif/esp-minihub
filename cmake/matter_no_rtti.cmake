# esp-matter C++ must match libCHIP.a: -fno-rtti and CHIP_HAVE_CONFIG_H.
# Scope to Matter components only — brookesia/LCD needs RTTI (dynamic_cast).
function(minihub_matter_apply_chip_cxx_flags component)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(NOT ${component} IN_LIST build_components)
        return()
    endif()
    idf_component_get_property(_lib ${component} COMPONENT_LIB)
    if(NOT _lib)
        return()
    endif()

    get_target_property(_lib_type ${_lib} TYPE)
    if(_lib_type STREQUAL "INTERFACE_LIBRARY")
        # No translation units (esp_matter_console without CONFIG_ENABLE_CHIP_SHELL,
        # or esp_matter_ota_provider when the provider is off). PRIVATE is illegal
        # on INTERFACE libraries. Propagate only CHIP_HAVE_CONFIG_H: -fno-rtti and
        # -std=gnu++17 must not follow a public REQUIRES edge
        # (console → controller → minihub_matter → hub → LCD, which needs RTTI
        # and C++20). Real libraries still get those flags PRIVATE below.
        target_compile_definitions(${_lib} INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:CHIP_HAVE_CONFIG_H>)
    else()
        target_compile_options(${_lib} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17;-DCHIP_HAVE_CONFIG_H;-fno-rtti>)
    endif()
endfunction()

if(CONFIG_MINIHUB_MATTER_ADAPTER)
    # esp_matter examples set CHIP_HAVE_CONFIG_H on every CXX TU via the
    # top-level CMakeLists. MiniHub only opts in Matter components, so any
    # component that includes CHIP headers must be listed here. Without the
    # define, SystemBuildConfig.h is skipped and SystemConfig.h #errors
    # (no LWIP/sockets), which then cascades into incomplete *Impl types
    # and an undefined Crypto::DefaultSessionKeystore.
    foreach(_comp esp_matter_controller esp_matter esp_matter_ota_provider esp_matter_console minihub_matter)
        minihub_matter_apply_chip_cxx_flags(${_comp})
    endforeach()
endif()
