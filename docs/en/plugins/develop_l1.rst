Develop L1 (Config) Packs
=========================

:link_to_translation:`zh_CN:[中文]`

L1 modules are **configuration-only** ``.mhub`` packages. They do not ship
``main.js`` or a native ``.so``. Host loads them through ``minihub_pack`` and
**merges JSON fragments** into ``adapters.json`` at runtime (or on reload).

When to use L1
---------------

L1 **only patches an existing Host adapter block**. It does not register
points, run logic, or call the cloud.

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
  point ``props`` from Host interview or L2/L3 ``merge_props``, not
  ``adapters.json`` keys.
* Cloud APIs, a new adapter, or claiming an existing Zigbee point — use
  :doc:`develop_l2` or :doc:`develop_l3`.

Validation allows ``inject.adapter`` ``zigbee`` so you can set **provision
keys**, not so L1 can ship a device list.

Put ``inject`` in ``module.json`` only. There is no ``inject.json`` loader.
The pack directory only needs ``module.json``.

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

``inject.adapter`` names the key under ``adapters`` in ``adapters.json``.
On load, Host **merges** ``inject`` fields into that adapter block (creating it
if missing). See ``components/minihub_pack/minihub_pack.c``.

Pack and install
----------------

Scripts (scaffold → pack → catalog → serve):

.. code-block:: bash

   python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
   # demos live under examples/plugins/ (example: ble_mesh_pack)
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack ble_mesh_pack
   python3 tools/sync_store_catalog.py --from-store
   python3 tools/store_server.py --port 8080

.. code-block:: bash

   python3 tools/pack_store_module.py pack ble_mesh_pack
   # → $MINIHUB_PLUGINS_DEPLOY_DIR/ble_mesh_pack/ble_mesh_pack.mhub

On device (after ``store_server.py`` + catalog refresh):

.. code-block:: bash

   curl -X POST http://HUB/api/store/install \
     -H 'Content-Type: application/json' \
     -d '{"kind":"pack","id":"ble_mesh_pack"}'
   curl -X POST http://HUB/api/hub/reload

Verify with ``GET /api/packs`` and ``GET /api/config/adapters``.

IDE MCP for authors: ``python3 tools/minihub_plugin_mcp.py`` (scaffold / validate /
pack). Full script matrix: :doc:`overview`.

Do not add this demo to public Host ``store_preinstall``. OEM factory list:
``application/APP/manifest.yml`` (see :doc:`overview`).
