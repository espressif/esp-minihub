# Umbrella: yml lists the default Core/adapter graph (no lcd/usb/rtsp/mqtt/modbus
# Thread adapter). Those stay on EXTRA_COMPONENT_DIRS and stub when Kconfig is off.
# Store L3 not listed. Kconfig.projbuild is the top-level menu "MiniHub"
# (must be in the build graph — Host require / Mosaic path dep).

idf_component_register(
    REQUIRES
        json
        minihub_core
        minihub_config
        minihub_adapter
        minihub_rules
        minihub_scenes
        minihub_web
        minihub_pack
        minihub_module
        minihub_store
        minihub_hub
        minihub_auth
        minihub_ota
        minihub_update
        minihub_provision
        minihub_net
        minihub_matter
        minihub_rainmaker
        minihub_js
        minihub_zigbee
        minihub_ble_mesh
        minihub_ble_gatt
        minihub_mcp_server
        minihub_product_params
        adapter_zigbee
        adapter_ble_mesh
        adapter_matter
)
