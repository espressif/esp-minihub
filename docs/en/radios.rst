Radios and access overview
==========================

:link_to_translation:`zh_CN:[中文]`

The bundled adapters map radio and IP traffic onto Core points. Developer logic —
associating a device, pulling a cloud catalog, marking the OTA-ready state — belongs in a
Store plugin on the **same** ``point_id``. One physical device is one point; a
second point looks to Core like a second device.

Thread is not a device adapter. It runs an OpenThread Border Router, so
Matter-over-Thread end-devices appear as Matter points. MQTT and Modbus are
configuration-only; see :doc:`data_model`.

The scarce resource is the 2.4 GHz radio. Wi-Fi and BLE Mesh coexist on it. IEEE
802.15.4 — Zigbee **or** Thread — shares it with neither of them, and Zigbee and
Thread exclude each other, so one product runs either the Wi-Fi / BLE Mesh group
or one 802.15.4 stack. A Zigbee or Thread Hub therefore takes its IP uplink from
Ethernet. ``minihub_adapter_start_all()`` starts Wi-Fi first, then 802.15.4, then
BLE Mesh; adapters that hold no radio (Modbus, for example) start last. The Wi-Fi
group wins: when ``adapters.json`` asks for Wi-Fi or BLE Mesh together with
Zigbee/Thread, the 802.15.4 adapter is the loser — it is not initialised, not
started, publishes no points, is never polled, a ``radio mutex`` warning is
logged, and the file is not rewritten. Zigbee and Thread are also held apart at
build time: ``tools/build_firmware.py`` refuses a configuration that turns
on both (a stack already enabled in sdkconfig is pulled in before that check),
and neither Kconfig option has a ``depends on`` / ``select`` tie to the other.
Wi-Fi STA is a Host adapter (``adapters.wifi``). Ethernet is started by
``minihub_net_init()``, is not an adapter, and never competes for the radio.

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Enabled radio combination
     - Result
   * - Wi-Fi + BLE Mesh
     - OK (the one supported pair)
   * - Wi-Fi + Zigbee
     - **Zigbee dropped**
   * - Wi-Fi + Thread
     - **Thread dropped**
   * - BLE Mesh + Zigbee
     - **Zigbee dropped**
   * - BLE Mesh + Thread
     - **Thread dropped**
   * - Zigbee + Thread
     - **Thread dropped** (also blocked at build time)
   * - Wi-Fi + BLE Mesh + Zigbee
     - **Zigbee dropped**; Wi-Fi + BLE Mesh stay

Identity, associate, OTA
------------------------

Overview cards skip identity fields (``manufacturer`` / ``uuid`` /
``node_id``, …). The detail page shows vendor / model / version. After associate,
if that plugin slot is ``enabled``, the card moves from the radio group to
``props.owner``; commands still use ``point.adapter`` (the radio Host).

.. list-table::
   :header-rows: 1
   :widths: 14 28 30 28

   * - Adapter
     - Hub point
     - Identity (Host)
     - OTA
   * - wifi
     - **no** device points (STA uplink)
     - —
     - —
   * - :doc:`zigbee`
     - ``zigbee.zb_XXXX``, one node one point
     - Basic 0x0004 / 0x0005, …
     - Stub (``NOT_SUPPORTED``)
   * - :doc:`ble_mesh`
     - ``ble_mesh.bm_XXXX``, one node one point
     - Composition CID/PID/VID;
       ``uuid``
     - Stub (Firmware Update /
       BLOB)
   * - :doc:`matter`
     - ``{transport}.matter_n{node}_ep{ep}``
       **per endpoint**
     - EP0 Basic Information;
       ``node_id``
     - CSA Provider ``0x0029`` +
       BDX
   * - :doc:`thread`
     - **no** device points
     - —
     - Use the Matter point

No radio has a Store demo in-tree: associate and OTA logic ship as
their own plugin on the same ``point_id``. Thread needs no such demo — its
end-devices are Matter points, so do not ship a ``thread_ota`` plugin.

Associate (``props.owner``) and the OTA readiness block (``props.ota``) are both
**plugin-side conventions** — no Host code writes either, and the Hub never
fetches a vendor catalog on a plugin's behalf. A plugin decides for itself when
a firmware update is ready (its own ``adapters.json`` keys, NVS state, a scheduled
fetch). Plugins pack via
``MINIHUB_PLUGIN_DIRS``; do **not** add them to public Host
``store_preinstall``.

End-device OTA REST: ``POST /api/devices/ota`` (:doc:`api/host_rest`). Hub
**firmware** update is a different path: :doc:`ota`.

Do not L1-inject ``devices[]`` on ``zigbee`` / ``matter`` / ``ble_mesh``, and
do not invent a Thread device list. See :doc:`plugins/develop_l1`.
