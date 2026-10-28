RainMaker
=========

可选的 ESP RainMaker 桥（:doc:`source_tree` 中的
``components/minihub_rainmaker``）默认关闭，用 ``CONFIG_MINIHUB_RAINMAKER``
打开。打开后，中枢会把自己注册成一个类型为 ``Gateway`` 的 RainMaker 节点，并把
它的 :doc:`data_model` 点位桥接成 RainMaker 设备，这样装了 ESP RainMaker App
的手机就能看到并控制它们。

账号绑定（仅限自测）
--------------------

.. warning::
   下面的 On-Network 绑定路径仅供自测和评估使用。如果你打算在真实产品中
   使用 RainMaker 功能，请联系 sales@espressif.com 详细沟通，以便安排正式的
   节点烧录、证书与量产流程。请勿在量产中依赖这条自测 claim 路径。

1. 打开 ``CONFIG_MINIHUB_RAINMAKER`` 与
   ``CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE``。
2. 编译并烧录 Host，然后给中枢配网（见 :doc:`networking`）。首次云端 claim
   需要能访问互联网。
3. 中枢启动后会在日志打印 ``RainMaker Node ID: <id>``；拿到 IP 并完成节点
   claim 后，会打印 ``On-Network user mapping READY``，并通过 mDNS 服务
   ``_esp_rmaker_chal_resp._tcp`` 在端口 ``8081`` 对外提供（不是 ``80``，
   因为 MiniHub Web 控制台占用 ``80``）。无需 PoP（proof-of-possession）。
4. 打开 ESP RainMaker 手机 App，用「On-Network」（本地网络）方式添加设备。
   App 会通过 mDNS 发现中枢，并把该节点映射到你的 RainMaker 账号。
5. 映射完成后，中枢及其桥接的子设备会出现在你的 RainMaker 账号下。
