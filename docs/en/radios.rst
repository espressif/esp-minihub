Radios and access
=================

:link_to_translation:`zh_CN:[中文]`

Host **static adapters** turn radio / IP devices into Core points. Vendor
logic (claim, cloud catalog, OTA flag) is a Store plugin on the **same**
``point_id`` — do not register a second point for one physical device.

**Thread is not a device adapter**: it is an OpenThread Border Router.
Matter-over-Thread end-devices are Matter points. MQTT / Modbus are
config-only; see :doc:`data_model`.

Identity, claim, OTA
--------------------

Overview cards skip identity chips (``manufacturer`` / ``uuid`` /
``node_id``, …). The detail page shows vendor / model / version. After claim,
if that plugin slot is ``enabled``, the card moves from the radio group to
``props.owner``; commands still use ``point.adapter`` (the radio Host).

.. list-table::
   :header-rows: 1
   :widths: 12 22 24 22 20

   * - Adapter
     - Hub point
     - Identity (Host)
     - Air OTA
     - Demo plugin
   * - :doc:`zigbee`
     - ``zigbee.zb_XXXX``, one
       node one point
     - Basic 0x0004 / 0x0005, …
     - Stub (``NOT_SUPPORTED``)
     - ``examples/plugins/zigbee_ota``
   * - :doc:`ble_mesh`
     - ``light/bm_XXXX``, one
       node one point
     - Composition CID/PID/VID;
       ``uuid``
     - Stub (Firmware Update /
       BLOB)
     - ``examples/plugins/ble_mesh_ota``
   * - :doc:`matter`
     - ``light/matter_n{node}_ep{ep}``
       **per endpoint**
     - EP0 Basic Information;
       ``node_id``
     - CSA Provider ``0x0029`` +
       BDX
     - ``examples/plugins/matter_ota``
   * - :doc:`thread`
     - **no** device points
     - —
     - Use the Matter point
     - Do not ship ``thread_ota``

Claim (``props.owner``) does **not** need a cloud catalog. ``props.ota`` is
merged only after GET ``catalog_url`` succeeds. Demo plugins pack via
``MINIHUB_PLUGIN_DIRS``; do **not** add them to public Host
``store_preinstall``.

End-device OTA REST: ``POST /api/devices/ota`` (:doc:`api/host_rest`). Hub
**firmware** update is a different path: :doc:`ota`.

Do not L1-inject ``devices[]`` on ``zigbee`` / ``matter`` / ``ble_mesh``, and
do not invent a Thread device list. See :doc:`plugins/develop_l1`.
