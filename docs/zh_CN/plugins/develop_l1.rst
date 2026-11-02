开发 L1（配置包）
=================

:link_to_translation:`en:[English]`

L1 模块是 **纯配置** 的 ``.mhub`` 包：不含 ``main.js`` 或原生 ``.so``。
``minihub_pack_merge_into_adapters()`` 把它的 JSON 折进 Host **内存里** 那棵适配器树，
不会把 ``adapters.json`` 写回磁盘。所以这次合并只出现在 ``GET /api/config/adapters`` ，
``/data`` 上那份文件仍是出厂时那份 —— 开机和每次软重载都会重新应用一次这个包。

适用场景
--------

适配器默认参数需要送到已经出厂的设备时，用 L1。它按名字改一个适配器配置块：
这个名字在树里还不存在时，合并会 **新建** 一个，并置 ``enabled: true`` （串口日志
``created adapter '<name>' for pack '<id>'`` ）；已存在且写着 ``enabled: false`` 时，
这个包被跳过（ ``skip pack '<id>': adapter '<name>' disabled`` ）—— 停用中的适配器
不会被包打开。包里没有代码，所以不能登记点、跑逻辑或访问云。

**可以 inject：**

* BLE Mesh ``provision.uuid_filters`` / ``bind_models``
* Modbus 寄存器映射、MQTT broker 调参等「适配器默认参数」
* 跨产品复用的同一段 JSON，避免写进出厂 ``configs/``

**不要 inject：**

* ``zigbee`` / ``matter`` / ``ble_mesh`` 的 ``devices[]``。
  这些设备由射频入网（或配网）后由 Host 登记点（如
  ``zigbee.zb_7d4b``）。L1 往 ``devices[]`` 追加，等于在 JSON 里再造一台
  「纸面设备」。``thread`` **没有** ``devices[]`` （只做 BR）；不要用 L1
  给 Thread 发明设备列表。
* 厂商、品类、``owner``、``ota`` —— 那是点位顶层 ``props``，由 Host 访谈或
  L2 ``merge_props`` 写入，不是 ``adapters.json`` 字段。
* 云端 API、新适配器、关联已有 Zigbee 点 —— 用 :doc:`develop_l2`。

.. only:: not hide_l3

   L3 也可以用 ``merge_props`` 写这些字段，并增加新适配器。见 :doc:`develop_l3`。

校验允许 ``inject.adapter`` 为 ``zigbee`` 等，是为了可以改 **provision
类键**，不是允许用 L1 发设备列表。

``inject`` 只写在 ``module.json``，别处不认 —— 没有单独的 ``inject.json``
加载路径。因此包目录只要有这一个文件。

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

``inject.adapter`` 对应内存树里 ``adapters`` 下的键名。这个合并不是通用的深合并：
``provision`` 里只有 ``uuid_filters`` 与 ``bind_models`` 会追加到适配器原有内容之后
（ ``merge_provision()`` → ``merge_json_array_append()`` ），其余每个键都是整体替换。
也就是说除了这两个数组，包是在覆盖默认值而不是往里加。实现见
``components/minihub_pack/minihub_pack.c``。

打包与安装
----------

.. code-block:: bash

   python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack my_pack
   python3 tools/store_server.py --port 8080
   # → $MINIHUB_PLUGINS_DEPLOY_DIR/Stores/Common/my_pack/{my_pack.mhub,catalog.json}

``pack`` 自己就会在包旁边写 ``catalog.json`` ，只有手工放好的 ``.mhub`` 或改过条目之后
才需要 ``sync_store_catalog.py`` 。

设备上，在 Web UI（ ``http://HUB/`` ）中打开 ``商店``，把自定义商店 URL 设为
``store_server.py`` 打印出的地址，刷新目录并安装 ``my_pack``，Hub 会自动重载。
仓库内的 L1 示例是 ``examples/plugins/ble_mesh_pack``。

用 ``GET /api/packs`` 与 ``GET /api/config/adapters`` 验证。

插件作者的 IDE MCP： ``python3 tools/minihub_plugin_mcp.py`` （脚手架 / 校验 / 打包）。
完整脚本矩阵见 :doc:`overview` 。

不要把本演示写进公开 Host 的 ``store_preinstall`` —— 列在那里的东西会进入通用构建的
每一台设备。OEM 工厂列表见 ``application/APP/manifest.yml`` （:doc:`overview`）。
