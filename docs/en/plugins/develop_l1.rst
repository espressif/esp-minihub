Develop L1 (Config) Packs
=========================

:link_to_translation:`zh_CN:[中文]`

An L1 pack is a ``.mhub`` package with nothing in it but configuration: no
``main.js``, no native ``.so``. ``minihub_pack_merge_into_adapters()`` folds its
JSON into the adapter tree the Host works from **in memory**; it never writes
``adapters.json`` back. The merge is therefore visible in
``GET /api/config/adapters`` while the file on ``/data`` stays exactly what the
product shipped — the pack is applied again at boot and on every reload.

When to use L1
---------------

Use L1 when adapter defaults need to reach a device that has already been
flashed. It patches one adapter block by name. If that name is not in the tree
yet, the merge **creates** it with ``enabled: true`` (serial log
``created adapter '<name>' for pack '<id>'``); if the block exists and says
``enabled: false``, the pack is skipped (``skip pack '<id>': adapter '<name>'
disabled``) — a disabled adapter is never switched on by a pack. The package
carries no code, so it cannot register points, run logic or call the cloud.

**Do inject:**

* BLE Mesh ``provision.uuid_filters`` / ``bind_models``
* Modbus register maps, MQTT broker knobs, and similar adapter defaults
* The same JSON snippet across products, instead of baking it into factory
  ``configs/``

**Do not inject:**

* ``devices[]`` on ``zigbee`` / ``matter`` / ``ble_mesh``.
  Those devices join over the radio (or provisioning). Host then registers
  **one** point (e.g. ``zigbee.zb_7d4b``). Appending ``devices[]`` from L1
  invents a paper device. ``thread`` has **no** ``devices[]`` (BR only);
  do not invent a Thread device list from L1.
* ``manufacturer``, ``category``, ``owner``, ``ota`` — those are top-level
  point ``props`` from Host interview or L2 ``merge_props``, not
  ``adapters.json`` keys.
* Cloud APIs, a new adapter, or associating an existing Zigbee point — use
  :doc:`develop_l2`.

.. only:: not hide_l3

   L3 can also set those props with ``merge_props``, and can add a new
   adapter. See :doc:`develop_l3`.

Validation allows ``inject.adapter`` ``zigbee`` so you can set **provision
keys**, not so L1 can ship a device list.

``inject`` goes in ``module.json`` and nowhere else, because there is no
``inject.json`` loader. A pack directory therefore needs that one file.

In-tree demo: ``examples/plugins/ble_mesh_pack`` (export
``MINIHUB_PLUGIN_DIRS``). It appends one extra ``provision.bind_models``
entry so ``GET /api/config/adapters`` shows the merge. It does **not** add
Mesh ``devices[]``. Public Host ``store_preinstall`` stays empty.

Minimal ``module.json`` (``runtime: pack``):

.. code-block:: json

   {
     "id": "my_pack",
     "name": "My BLE Mesh defaults",
     "version": "1.0.0",
     "runtime": "pack",
     "enabled": true,
     "inject": {
       "adapter": "ble_mesh",
       "provision": {
         "uuid_match_disabled": true,
         "bind_models": [
           { "model_id": "0x1000", "name": "gen_onoff_srv" }
         ]
       }
     }
   }

``inject.adapter`` names the key under ``adapters``. The merge is not a general
deep merge: inside ``provision``, only ``uuid_filters`` and ``bind_models`` are
appended to whatever the adapter already had
(``merge_provision()`` → ``merge_json_array_append()``), and every other key is
replaced whole. A pack overrides a default rather than adding to it, except in
those two arrays. See ``components/minihub_pack/minihub_pack.c``.

Pack and install
----------------

Scripts (scaffold → pack → catalog → serve):

.. code-block:: bash

   python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack my_pack
   python3 tools/store_server.py --port 8080
   # → $MINIHUB_PLUGINS_DEPLOY_DIR/Stores/Common/my_pack/{my_pack.mhub,catalog.json}

``pack`` writes the ``catalog.json`` beside the package itself, so
``sync_store_catalog.py`` is only needed for a ``.mhub`` you placed by hand or
after editing an entry.

On the device, in the Web UI at ``http://HUB/``, open ``Store``, set the custom
Store URL to the address ``store_server.py`` printed, refresh the catalog, and
install ``my_pack``; the Hub reloads automatically. The in-tree L1 example is
``examples/plugins/ble_mesh_pack``.

Verify with ``GET /api/packs`` and ``GET /api/config/adapters``.

IDE MCP for authors: ``python3 tools/minihub_plugin_mcp.py`` (scaffold / validate /
pack). Full script matrix: :doc:`overview`.

Keep this demo out of the public Host ``store_preinstall`` list: whatever is
listed there reaches every unit of a generic build. OEM factory list:
``application/APP/manifest.yml`` (see :doc:`overview`).
