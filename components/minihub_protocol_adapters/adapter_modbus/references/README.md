# adapter_modbus

Host-static **Modbus master** using [espressif/esp-modbus](https://components.espressif.com/components/espressif/esp-modbus).

| Knob | Default |
|------|---------|
| `CONFIG_MINIHUB_MODBUS` | `y` (build) |
| `adapters.modbus.enabled` | `false` (runtime) |

| `gateways[].transport` | Path |
|------------------------|------|
| `tcp` | Modbus TCP (MBAP) |
| `rtu` | UART / RS-485 RTU (`mbc_master_create_serial`) |
| `rtuovertcp` | Ethernet Modbus-TCP gateway → RS-485 (MBAP + UID) |

Config overview: [docs/en/plugins/overview.rst](../../../../docs/en/plugins/overview.rst).  
Runtime shape: `config/adapters.json` → `adapters.modbus`.

PDU addresses are **0-based**. Function codes 01–04 / 05 / 06 / 15 / 16.
