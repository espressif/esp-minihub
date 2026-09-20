开发 L1（配置包）
=================

:link_to_translation:`en:[English]`

L1 模块是 **纯配置** 的 ``.mhub`` 包：不含 ``main.js`` 或原生 ``.so`` 。Host 通过
``minihub_pack`` 在运行时（或软重载时）把 JSON 片段 **合并** 进 ``adapters.json`` 。

适用场景
--------

L1 **只改已有 Host 适配器的配置块**，不登记点、不跑逻辑、不访问云。

**可以 inject：**

* BLE Mesh ``provision.uuid_filters`` / ``bind_models``
* Modbus 寄存器映射、MQTT broker 调参等「适配器默认参数」
* 跨产品复用的同一段 JSON，避免写进出厂 ``config/``

**不要 inject：**

* ``zigbee`` / ``matter`` / ``ble_mesh`` 的 ``devices[]``。
  这些设备由射频入网（或配网）后由 Host 登记点（如
  ``zigbee.zb_7d4b``）。L1 往 ``devices[]`` 追加，等于在 JSON 里再造一台
  「纸面设备」。``thread`` **没有** ``devices[]`` （只做 BR）；不要用 L1
  给 Thread 发明设备列表。
* 厂商、品类、``owner``、``ota`` —— 那是点位顶层 ``props``，由 Host 访谈或
  L2/L3 ``merge_props`` 写入，不是 ``adapters.json`` 字段。
* 云端 API、新适配器、认领已有 Zigbee 点 —— 用 :doc:`develop_l2` /
  :doc:`develop_l3`。

校验允许 ``inject.adapter`` 为 ``zigbee`` 等，是为了可以改 **provision
类键**，不是允许用 L1 发设备列表。

``inject`` 只写在 ``module.json``。没有单独的 ``inject.json`` 加载路径。
包目录只需 ``module.json``。

仓库内演示：``examples/plugins/ble_mesh_pack`` （需导出
``MINIHUB_PLUGIN_DIRS``）。它追加一条 ``provision.bind_models``，用
``GET /api/config/adapters`` 即可看到合并结果。 **不会** 往 Mesh
``devices[]`` 里加设备。公开 Host 的 ``store_preinstall`` 保持为空。

最小 ``module.json`` （``runtime: pack`` ）：

.. code-block:: json

   {
     "id": "my_pack",
     "name": "BLE Mesh 默认参数包",
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

``inject.adapter`` 对应 ``adapters.json`` 里 ``adapters`` 下的键名。加载时 Host
将 ``inject`` 字段 **合并** 到该适配器块（不存在则创建）。实现见
``components/minihub_pack/minihub_pack.c``。

打包与安装
----------

.. code-block:: bash

   python3 tools/pack_store_module.py pack ble_mesh_pack
   # → $MINIHUB_PLUGINS_DEPLOY_DIR/ble_mesh_pack/ble_mesh_pack.mhub
   python3 tools/sync_store_catalog.py --from-store

设备上 (先 ``store_server.py`` 并刷新目录)：

.. code-block:: bash

   curl -X POST http://HUB/api/store/install \
     -H 'Content-Type: application/json' \
     -d '{"kind":"pack","id":"ble_mesh_pack"}'
   curl -X POST http://HUB/api/hub/reload

用 ``GET /api/packs`` 与 ``GET /api/config/adapters`` 验证。

不要把本演示写进公开 Host 的 ``store_preinstall``。OEM 工厂列表见
``application/APP/manifest.yml`` （:doc:`overview`）。
