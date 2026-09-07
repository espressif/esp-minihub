idf_component_register(
    SRCS "module_entry.c" "hello_adapter.c"
    INCLUDE_DIRS "."
    REQUIRES minihub_adapter minihub_core minihub_module
)

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fPIC)
endif()
