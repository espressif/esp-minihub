# Always use the product 16MB partition table (partitions.csv).
# Board manager flash-size lines may still set SPIRAM etc.; partition CSV is fixed.
set(EDGE_AGENT_PROJECT_LOG_PREFIX "[claw_minihub]")
set(EDGE_AGENT_BOARD_MANAGER_DEFAULTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/board_manager.defaults")
set(EDGE_AGENT_MINIHUB_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.minihub")
set(EDGE_AGENT_PARTITION_DEFAULTS "${CMAKE_BINARY_DIR}/edge_agent_partition_auto.defaults")

file(WRITE "${EDGE_AGENT_PARTITION_DEFAULTS}"
    "# Auto-generated: claw_minihub is 16MB-only. Do not edit.\n"
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y\n"
    "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\"\n"
    "CONFIG_PARTITION_TABLE_FILENAME=\"partitions.csv\"\n")

# Stale sdkconfig from older trees may still name partitions_16MB.csv — force rewrite.
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(READ "${CMAKE_SOURCE_DIR}/sdkconfig" EDGE_AGENT_SDKCONFIG_CONTENT)
    string(REGEX REPLACE
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"]*\""
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\""
        EDGE_AGENT_SDKCONFIG_CONTENT "${EDGE_AGENT_SDKCONFIG_CONTENT}")
    string(REGEX REPLACE
        "CONFIG_PARTITION_TABLE_FILENAME=\"[^\"]*\""
        "CONFIG_PARTITION_TABLE_FILENAME=\"partitions.csv\""
        EDGE_AGENT_SDKCONFIG_CONTENT "${EDGE_AGENT_SDKCONFIG_CONTENT}")
    file(WRITE "${CMAKE_SOURCE_DIR}/sdkconfig" "${EDGE_AGENT_SDKCONFIG_CONTENT}")
endif()

if(SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${EDGE_AGENT_MINIHUB_DEFAULTS};${EDGE_AGENT_PARTITION_DEFAULTS}")
elseif(NOT "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
    set(SDKCONFIG_DEFAULTS "$ENV{SDKCONFIG_DEFAULTS};${EDGE_AGENT_MINIHUB_DEFAULTS};${EDGE_AGENT_PARTITION_DEFAULTS}")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${EDGE_AGENT_MINIHUB_DEFAULTS};${EDGE_AGENT_PARTITION_DEFAULTS}")
endif()

if(EXISTS "${EDGE_AGENT_BOARD_MANAGER_DEFAULTS}")
    set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${EDGE_AGENT_BOARD_MANAGER_DEFAULTS}")
endif()

message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} Partition table: partitions.csv (16MB)")
