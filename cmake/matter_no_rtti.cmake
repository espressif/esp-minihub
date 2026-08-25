# esp-matter C++ must match libCHIP.a: -fno-rtti and CHIP_HAVE_CONFIG_H.
# Scope to Matter components only — brookesia/LCD needs RTTI (dynamic_cast).
function(minihub_matter_apply_chip_cxx_flags component)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(NOT ${component} IN_LIST build_components)
        return()
    endif()
    idf_component_get_property(_lib ${component} COMPONENT_LIB)
    if(_lib)
        target_compile_options(${_lib} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17;-DCHIP_HAVE_CONFIG_H;-fno-rtti>)
    endif()
endfunction()

if(CONFIG_MINIHUB_MATTER_ADAPTER)
    foreach(_comp esp_matter_controller esp_matter minihub_matter)
        minihub_matter_apply_chip_cxx_flags(${_comp})
    endforeach()
endif()
