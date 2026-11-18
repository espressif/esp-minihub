JS plugin MQTT host API
=======================

:link_to_translation:`zh_CN:[中文]`

This namespace lives in ``components/minihub_js/minihub_js_mqtt.c`` over the
loopback client in ``components/minihub_mqtt_broker`` (``mqtt_adapter.c`` /
``minihub_mqtt.h``). The table is read from the C bindings at build time.

.. minihub-JS-api:: mqtt

See :doc:`js_host` for the entry point.

Methods — host.mqtt
-------------------

Wildcard MQTT subscriptions for **dynamic device discovery**: JS plugins
subscribe to a discovery topic, register a handler with ``client.onMessage()``,
and call ``host.point.register()`` for each device they see — no static
``inject.devices[]`` list is needed.

Enable ``CONFIG_MINIHUB_JS_MQTT``
(``menuconfig → JS plugins``); it depends on ``CONFIG_MINIHUB_MQTT_ADAPTER``,
which depends on ``CONFIG_MINIHUB_MQTT_BROKER``. At run time the same
``adapters.mqtt_broker.enabled`` flag starts the broker and the Hub's own
loopback client that carries these subscriptions, so a disabled broker leaves
``host.mqtt`` disconnected. When the Kconfig is off, every method below throws
``host.mqtt disabled`` so ``setup(host)`` can degrade gracefully.

All plugins share the adapter's **single** ``esp_mqtt_client``, and each plugin
owns its own subscriptions and its own queue. Incoming messages are delivered to
that plugin's handler on the JS worker, so one plugin cannot consume another's
messages. Filters use MQTT ``+`` (one level) and ``#`` (remaining levels)
wildcards and are re-subscribed automatically after a broker reconnect.

If more than one discovery plugin runs on the Hub, give each its own client with
``host.mqtt.client(id)`` (below) and register ``onMessage`` on it — otherwise
both plugins would share the default client and cross-consume.

host.mqtt.client(id)
~~~~~~~~~~~~~~~~~~~~~

Returns a client object scoped to one **owner** (0 … 7). ``id`` is any non-empty
string; the same ``id`` returns the same object, so two calls with
``"tasmota"`` share one filter set and queue. A fresh ``id`` allocates a new
owner up to the 8-owner limit (the 9th throws). The returned object carries:

* ``subscribe(filter, opts?)``, ``unsubscribe(filter)`` — register this owner's
  filters. The same filter owned by two clients is one broker subscription.
* ``onMessage(handler)`` — register ``handler(topic, payload)``. When a message
  matching this owner's filters arrives, the Host calls ``handler`` on the JS
  worker (no polling needed). One handler per client; re-calling replaces it.
* ``publish(topic, payload, opts?)`` — same as the top-level ``publish``.

  The client has no ``close()``: the underlying connection and the whole MQTT
  runtime are owned by the Host, not the plugin, and every owner's filters and
  queue are released on runtime unload (``minihub_js_mqtt_reset()``).

The top-level ``host.mqtt.subscribe`` / ``unsubscribe`` / ``onMessage`` are a
thin convenience layer over a built-in ``"*default*"`` client, intended for
single-plugin scripts.

.. code-block:: javascript

   var bus = host.mqtt.client("tasmota_esphome");
   bus.subscribe("tasmota/discovery/#");
   bus.subscribe("tele/+/LWT");
   bus.onMessage(function (topic, payload) {
     // parse topic / payload, then register a point
   });

host.mqtt.subscribe(filter, opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts.qos`` is 0–2 (default 0). Returns ``true``. Up to **32** filter entries are
shared by the whole JS runtime, and up to **8** plugins can own them; a 33rd
entry, or a ``subscribe()`` from a 9th plugin, throws. The same filter subscribed
by two plugins costs one broker subscription and two entries. Budget your filters
across plugins: discovery plugins use a handful of wildcard filters plus one per
device whose state topic is learned at runtime.

host.mqtt.unsubscribe(filter)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns ``true`` when this plugin had that filter. Other plugins keep theirs, and
the broker is unsubscribed only after the last plugin drops it.

host.mqtt.onMessage(handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register ``handler(topic, payload)`` for this client. When a message matching one
of this client's filters arrives, the Host calls ``handler`` on the JS worker —
no ``host.adapter.poll`` or ``host.timer`` is needed. Only one handler is kept
per client; calling ``onMessage`` again replaces it. If a message arrives before
``onMessage`` is registered, it buffers in the ring until the next dispatch and
is then delivered, so register ``onMessage`` before (or right after) subscribing
to avoid a race with a retained message.

The queue behind the handler is a bounded **24-slot ring** with a **2 KB** payload
cap, holding one copy of each message for every client interested in it. When the
ring wraps onto a message not yet dispatched, that message is dropped for this
client and counted in ``stats()``. Oversized or unallocatable messages are
dropped too.

.. code-block:: javascript

   host.mqtt.subscribe("tasmota/discovery/#");
   host.mqtt.subscribe("tele/+/LWT");
   host.mqtt.onMessage(function (topic, payload) {
     // parse topic / payload, then register a point
     var st = host.mqtt.stats();
     if (st.dropped) { host.log.warn("mqtt queue dropped " + st.dropped); }
   });

.. note::
   Bytes cross the esp-mqtt → JS boundary as a plain C hook into a per-owner ring
   buffer (never a ``JSValue`` held on the esp-mqtt task). The Host then invokes
   the JS handler on the JS worker, so the handler runs entirely on the JS task
   and a Store install/upgrade that tears down the runtime is safe — the handler
   is freed before ``JS_FreeContext``.

host.mqtt.publish(topic, payload, opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts.qos`` (0–2, default 0) and ``opts.retain`` (default 0). Returns
``false`` when the broker is disconnected, ``true`` when queued. Use it from
a ``host.command.on`` handler to control a discovered device.

host.mqtt.isConnected()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns a boolean for the current broker connection.

host.mqtt.stats()
~~~~~~~~~~~~~~~~~~~~~~

Returns ``{ queued, dropped, filters, connected, capacity }``. ``queued`` and
``dropped`` count this plugin's own backlog and its own losses; ``filters`` counts
the entries held by all plugins together. ``dropped`` is cumulative since the last
runtime reload.
