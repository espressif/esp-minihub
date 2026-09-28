# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). matter_adapter is referenced only under the same guard.
set(_matter_srcs "")
if(CONFIG_MINIHUB_MATTER_ADAPTER)
    set(_matter_srcs "matter_adapter.c" "matter_rpc.c")
endif()

idf_component_register(
    SRCS ${_matter_srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_matter espressif__cjson
)
