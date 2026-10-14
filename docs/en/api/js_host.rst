L2 JS Host API
==============

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Store **L2** modules (``runtime: js``, ``main.js``) access Host services through QuickJS
bindings in ``components/minihub_js/minihub_js.c``. Requires ``CONFIG_MINIHUB_JS_PLUGINS``.

REST install/RPC: :doc:`plugin_rest`. C point/command model: :doc:`minihub_core`.

.. only:: not hide_l3

   L3 uses the vtable in :doc:`module_abi`.

Module entry
------------

Hub calls global ``setup(host)`` once per enabled JS module on load and on each
``POST /api/hub/reload``. ``main.js`` must define ``setup``; otherwise the module is
skipped with a warning.

Use a distinct ``point_id`` prefix per module id in ``setup``; reload runs ``setup`` again.
Register **one point per device** (``module.device``), not one point per parameter.

Implementation
--------------

* ``components/minihub_js/minihub_js.c`` — QuickJS bindings

Entry function
--------------

setup(host)
~~~~~~~~~~~

.. code-block:: javascript

   function setup(host) { ... }

Parameters
^^^^^^^^^^

``host`` (object)
    Host-injected capability object (see ``host.*`` methods below).

Returns
^^^^^^^

``undefined``.

Methods — host.log
------------------

host.log.info / warn / error
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: javascript

   host.log.info(msg)
   host.log.warn(msg)
   host.log.error(msg)

Write ESP-IDF logs with tag ``minihub_js``.

Parameters
^^^^^^^^^^

``msg`` (string)
    Log message.

Returns
^^^^^^^

``undefined``.

Methods — host.point
--------------------

host.point.register(obj)
~~~~~~~~~~~~~~~~~~~~~~~~

Register or update a point in the Hub registry.

Parameters
^^^^^^^^^^

``obj.point_id`` (string, required)
    Max 64 chars.

``obj.type`` (string, optional)
    Default ``sensor``.

``obj.adapter`` (string, optional)
    Default ``js``; should match ``module.json`` ``adapter``.

``obj.value`` (string, optional)
    Initial value.

``obj.online`` (boolean, optional)
    Default false.

``obj.props`` (object, optional)
    Full props JSON copied into the point (Core duplicates). Prefer
    ``props.params.<name> = { value, access?, visible?, min?, max?, … }`` for
    Devices-page widgets. Manufacturer / category are top-level:
    ``manufacturer``, ``category`` (see :doc:`../data_model`).

``obj.params`` (object, optional)
    Shorthand: Host wraps as ``{ "params": <obj> }`` when ``props`` is omitted.

Returns
^^^^^^^

``undefined``; failures **throw** (QuickJS exception).

host.point.get(point_id)
~~~~~~~~~~~~~~~~~~~~~~~~

Read point snapshot.

Returns
^^^^^^^

``{ point_id, type, value, adapter, online, props }`` or ``null``.
``props`` is the JSON object (or ``null``).

host.point.set(point_id, value, props?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Update ``value`` (and optional ``props``) and publish ``point.updated``.

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    Target point.

``value`` (string, required)
    Summary value (e.g. ``on`` / ``off``).

``props`` (object, optional)
    When present, **replaces** the point's full props object. Prefer
    read-modify-write via ``host.point.get`` then ``set``, or keep extras
    under top-level keys (``ota``, ``ieee``, …) so adapter telemetry that
    merges only ``props.params`` does not wipe them.

Returns
^^^^^^^

``undefined``.

host.point.mergeProps(point_id, props_patch, value?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Shallow-merge top-level keys from ``props_patch`` into the live point
(``minihub_point_merge_props``). Use for identity / ``ota`` without wiping
``props.params``.

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    Target point.

``props_patch`` (object, required)
    Top-level keys to merge (e.g. ``{ manufacturer: "Espressif", category: "Color Bulb", ota: { available: true } }``).

``value`` (string, optional)
    New summary value; omit to keep the current summary.

Returns
^^^^^^^

``undefined``; failures **throw**.

host.point.mergeParams(point_id, params, value?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Replace ``props.params`` while keeping other top-level keys
(``minihub_point_merge_params``). Prefer this for telemetry updates.

Returns
^^^^^^^

``undefined``; failures **throw**.

Methods — host.event
--------------------

host.event.subscribe(event_type, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Subscribe to Core event-bus notifications (wraps
``minihub_event_bus_subscribe``). ``event_type`` may be a concrete name or
``*`` (all events). Host names and payload notes:
:doc:`host_rest` (``GET /ws/events``). Common examples: ``point.updated``,
``point.registered``.

Parameters
^^^^^^^^^^

``event_type`` (string, required)
    Event name, or ``*`` for all events.

``handler`` (function, required)
    Called as ``handler(evt)``. For ``point.updated``, ``evt`` includes
    ``point_id``, ``value``, ``props``, ``old_value``, ``new_value``.
    For ``point.registered``, ``evt.data`` carries ``point_id``.

Returns
^^^^^^^

``undefined``.

.. code-block:: javascript

   host.event.subscribe("point.updated", function (evt) {
     if (evt.point_id === "zigbee.zb_0a1b") {
       host.point.mergeProps(evt.point_id, {
         ota: { available: true, version: "1.2.3", url: "https://…" }
       });
     }
   });

Methods — host.timer
--------------------

Timers run JS callbacks on the Host worker task (QuickJS is single-threaded).
They are the only way to get periodic/delayed work inside a JS module — use
them for polling or debounce. The same four functions are also injected as
globals (``setTimeout`` / ``clearTimeout`` / ``setInterval`` / ``clearInterval``).

host.timer.setTimeout(fn, ms) / host.timer.setInterval(fn, ms)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: javascript

   var id = host.timer.setTimeout(function () { host.log.info("fired once"); }, 1000);
   var poll = host.timer.setInterval(function () { /* read sensor */ }, 5000);
   host.timer.clearTimeout(id);
   host.timer.clearInterval(poll);

Parameters
^^^^^^^^^^

``fn`` (function, required)
    Callback with no arguments. Runs on the JS worker task.

``ms`` (number, required)
    Delay in milliseconds (``setInterval``: period). Values ``<= 0`` become
    ``1`` ms. Argument order ``(fn, ms)`` or ``(ms, fn)`` is accepted.

Returns
^^^^^^^

Integer id (1-based). Each module can have at most 16 live timers; exceeding
that **throws**.

Notes
^^^^^

* ``setup`` may arm timers; they keep running until ``clear*`` or reload.
* A long-running callback blocks other timers and jobs — keep callbacks short.
* ``POST /api/hub/reload`` clears all timers and runs ``setup`` again.

Methods — host.command
----------------------

host.command.on(adapter, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register a JS command handler for ``adapter`` (should match ``module.json``
``adapter`` / point ``adapter``). Core ``minihub_command_invoke`` routes UI/REST
commands for those points into ``handler``.

Parameters
^^^^^^^^^^

``adapter`` (string, required)
    Adapter name.

``handler`` (function, required)
    Called as ``handler({ point_id, cmd, params })``. ``params`` is a JS object
    or ``null``. Return ``false`` to signal failure; throw → Core ``ESP_FAIL``.

Returns
^^^^^^^

``undefined``.

host.command.invoke({ point_id, cmd, params? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Routes via ``minihub_command_invoke``; same as REST ``POST /api/command``.

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    Target point.

``cmd`` (string, required)
    Command name.

``params`` (object, optional)
    JSON parameters (e.g. ``{ parameter, value }`` for Devices control).

Returns
^^^^^^^

``undefined``.

See also
^^^^^^^^

Demo with three controllable lights: ``examples/plugins/color_bulb_js/``.

Methods — host.http
-------------------

host.http.request(opts)
~~~~~~~~~~~~~~~~~~~~~~~

HTTP(S) via ``esp_http_client``.

Parameters
^^^^^^^^^^

``opts.url`` (string, required)
    Full URL; ``https://`` uses TLS.

``opts.method`` (string, optional)
    ``GET`` (default), ``POST``, ``PUT``.

``opts.body`` (string, optional)
    POST/PUT body.

``opts.timeout_ms`` (number, optional)
    Default 15000.

Returns
^^^^^^^

``{ status: number, body: string, headers?: { location?: string, "set-cookie"?: string, "extension-pragma"?: string } }``.

Attention
^^^^^^^^^

Response body cap **65536** bytes; ``main.js`` max **1 MiB**. URLs must pass Host
network policy. Optional ``headers`` object sets request headers (e.g. ``Cookie``,
``Content-Type``). Auto-redirect is **disabled** so callers can capture
``Location`` / ``Set-Cookie`` (passport login). ``DELETE`` is accepted in addition
to GET/POST/PUT.

Methods — host.net
------------------

IPv4 UDP helpers for LAN protocols. Binary payloads use **base64**.
Implementation: ``components/minihub_js/minihub_js_ext.c``.

host.net.udp.open(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~

``opts.broadcast`` (bool, optional) enables ``SO_BROADCAST``.

Returns a small integer socket handle (slot index).

host.net.udp.sendto(fd, { ip, port, data_b64 })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

host.net.udp.recvfrom(fd, { timeout_ms, max_len? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns ``{ ip, port, data_b64 }`` or ``null`` on timeout.

host.net.udp.close(fd)
~~~~~~~~~~~~~~~~~~~~~~

Methods — host.crypto
---------------------

Digest / HMAC / AES helpers. Digests return **lowercase hex**; HMAC and AES
return **base64**. Inputs accept a string or ``{ data }`` / ``{ data_b64 }``.

* ``host.crypto.md5(input)``
* ``host.crypto.sha1(input)``
* ``host.crypto.sha256(input)``
* ``host.crypto.hmacSha256({ key_b64, data_b64 })`` → base64
* ``host.crypto.aes128CbcEncrypt({ key_b64, iv_b64, data_b64 })`` → base64 (PKCS7)
* ``host.crypto.aes128CbcDecrypt({ key_b64, iv_b64, data_b64 })`` → base64

Demo: ``examples/plugins/color_bulb_js/``.

Methods — host.config
---------------------

host.config.get()
~~~~~~~~~~~~~~~~~

Read-only ``adapters.json`` root object.

Returns
^^^^^^^

JavaScript object or ``null``.

host.config.save(adaptersRoot)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Persist the full ``adapters.json`` object (``minihub_config_save_adapters``).

Parameters
^^^^^^^^^^

``adaptersRoot`` (object, required)
    Entire adapters document to write.

Returns
^^^^^^^

``undefined``; failures **throw**.

Methods — host.module
---------------------

host.module.rpc(moduleId, method, params?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Invoke another loaded module's RPC (``minihub_module_invoke_rpc``), e.g. one
Store module calling into another for shared helpers.

Parameters
^^^^^^^^^^

``moduleId`` (string, required)
    Target module id.

``method`` (string, required)
    RPC method name.

``params`` (object, optional)
    JSON parameters.

Returns
^^^^^^^

Parsed JSON object (or ``null``). Failures **throw**.

host.module.registerRpc(method, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register an RPC handler for **this** module id (only valid inside ``setup``).
Web UI and ``POST /api/modules/<id>/rpc`` call ``handler(method, params)``.

.. only:: not hide_l3

   On **hybrid** modules, JS handlers become the primary RPC; methods not
   registered in JS fall through to the native ``minihub_module_rpc``.

Parameters
^^^^^^^^^^

``method`` (string, required)
    Method name (e.g. ``login``, ``sync``).

``handler`` (function, required)
    Return a JSON-serializable object. Return ``false`` or throw → RPC failure.

Returns
^^^^^^^

``undefined``.

See :doc:`../plugins/develop_l2`.
