JS 插件 MQTT Host API
=====================

:link_to_translation:`en:[English]`

此命名空间实现在 ``components/minihub_js/minihub_js_mqtt.c``，底层为
``components/minihub_protocol_adapters/adapter_mqtt``。下表在构建时从 C 绑定读出。

.. minihub-JS-api:: mqtt

命名空间入口见 :doc:`js_host`。

方法 — host.mqtt
----------------

用于**动态设备发现**的通配 MQTT 订阅：JS 插件订阅 discovery 主题、用
``client.onMessage()`` 注册处理回调，为看到的每个设备调用 ``host.point.register()``，
不再需要静态的
``inject.devices[]``。

需启用 ``CONFIG_MINIHUB_JS_MQTT``
（``menuconfig → JS plugins``），该项依赖 ``CONFIG_MINIHUB_MQTT_ADAPTER`` ，
后者又依赖 ``CONFIG_MINIHUB_MQTT_BROKER``。运行时还要
``adapters.mqtt_broker.enabled`` —— 同一个开关既启动 broker，也启动承载这些
订阅的 Hub 环回客户端；broker 不启用时 ``host.mqtt`` 保持未连接。
未启用时下面的方法抛出 ``host.mqtt disabled``，``setup(host)`` 可据此优雅降级。

所有插件共享适配器的**唯一** ``esp_mqtt_client``，但每个插件有自己的过滤器集合
与自己的队列。到达的消息会投递到该插件的处理回调（运行在 JS worker 上），一个
插件不会消耗掉另一个插件的消息。过滤器支持 MQTT ``+`` （单层）与 ``#`` （剩余多层）
通配符，并在 broker 重连后自动重新订阅。

若 Hub 上同时运行多个发现插件，请给每个插件都通过 ``host.mqtt.client(id)``
（见下）拿到各自的客户端并注册 ``onMessage``，否则两个插件会共用默认客户端而
互相消费消息。

host.mqtt.client(id)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回一个归属于某个**所有者**（0 … 7）的客户端对象。``id`` 为任意非空字符串；
同一个 ``id`` 返回同一个对象，因此两次 ``"tasmota"`` 调用共享同一过滤器集合与
队列。新的 ``id`` 会分配一个新的所有者，直到 8 个上限（第 9 个会抛异常）。返回
对象提供：

* ``subscribe(filter, opts?)``、``unsubscribe(filter)`` —— 注册该所有者的过滤器。
  两个客户端订阅同一过滤器只占用一条 broker 订阅。
* ``onMessage(handler)`` —— 注册 ``handler(topic, payload)``。当命中该所有者过滤器的
  消息到达时，Host 会在 JS worker 上调用 ``handler`` （无需轮询）。每个客户端只有一个
  处理回调，重复调用会替换它。
* ``publish(topic, payload, opts?)`` —— 与顶层 ``publish`` 一致。

  客户端没有 ``close()``：底层连接与整个 MQTT 运行时由 Host 拥有，而非插件；
  所有者的过滤器与队列会在运行时卸载时（``minihub_js_mqtt_reset()``）一并释放。

顶层 ``host.mqtt.subscribe`` / ``unsubscribe`` / ``onMessage`` 是内建
``"*default*"`` 客户端的薄封装，供单插件脚本使用。

.. code-block:: javascript

   var bus = host.mqtt.client("tasmota_esphome");
   bus.subscribe("tasmota/discovery/#");
   bus.subscribe("tele/+/LWT");
   bus.onMessage(function (topic, payload) {
     // 解析 topic / payload，随后注册点位
   });

host.mqtt.subscribe(filter, opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts.qos`` 为 0–2（默认 0），返回 ``true``。整个 JS 运行时共享最多 **32** 个
过滤器条目，最多 **8** 个插件可以持有条目；第 33 个条目、或第 9 个插件调用
``subscribe()`` 会抛异常。同一过滤器被两个插件订阅时，broker 侧只有一次订阅，
表里占两个条目。请跨插件规划过滤器预算：发现类插件会用掉几个通配过滤器，再加上
每个运行时学到的设备状态主题各一个。

host.mqtt.unsubscribe(filter)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

本插件确实持有该过滤器时返回 ``true``。其他插件的同名过滤器不受影响，broker
侧只在最后一个插件取消后才取消订阅。

host.mqtt.onMessage(handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

为本客户端注册 ``handler(topic, payload)``。当命中本客户端某个过滤器的消息到达时，
Host 会在 JS worker 上调用 ``handler``——无需 ``host.adapter.poll`` 或 ``host.timer``。
每个客户端只保留一个处理回调，再次调用 ``onMessage`` 会替换它。若消息在 ``onMessage``
注册前到达，它会留在环形缓冲中，待下一次派发时投递；因此请在订阅前（或订阅后立刻）
注册 ``onMessage``，避免与保留消息竞态。

处理回调背后的队列是有界的 **24 槽环形缓冲**，单条负载上限 **2 KB**，一条消息只
存一份，交给所有关心它的客户端各自读取。环形缓冲绕回尚未派发的消息时，那条消息
对该客户端而言被丢弃并计入 ``stats()``。超长或分配失败的消息同样丢弃。

.. code-block:: javascript

   host.mqtt.subscribe("tasmota/discovery/#");
   host.mqtt.subscribe("tele/+/LWT");
   host.mqtt.onMessage(function (topic, payload) {
     // 解析 topic / payload，然后注册点位
     var st = host.mqtt.stats();
     if (st.dropped) { host.log.warn("mqtt queue dropped " + st.dropped); }
   });

.. note::
   字节以普通 C 回调（绝不持有跨 esp-mqtt 任务边界的 ``JSValue``）写入按所有者划分的
   环形缓冲，再由 Host 在 JS worker 上调用 JS 处理回调，因此回调完全运行在 JS 任务上；
   Store 安装/升级销毁运行时时，处理回调会在 ``JS_FreeContext`` 之前被释放，不会悬空。

host.mqtt.publish(topic, payload, opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts.qos``（0–2，默认 0）与 ``opts.retain``（默认 0）。broker 未连接时返回
``false``，入队成功返回 ``true``。可在 ``host.command.on`` 处理器里控制已发现设备。

host.mqtt.isConnected()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回当前 broker 连接状态的布尔值。

host.mqtt.stats()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回 ``{ queued, dropped, filters, connected, capacity }``。``queued`` 与
``dropped`` 只统计本插件自己的积压和丢失，``filters`` 是所有插件合计占用的条目数。
``dropped`` 是自上次运行时重载以来的累计值。
