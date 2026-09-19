L2 JS Host API
==============

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Store **L2** modules (``runtime: js``, ``main.js``) access Host services through QuickJS
bindings in ``components/minihub_js/minihub_js.c``. Requires ``CONFIG_MINIHUB_JS_PLUGINS``.

REST install/RPC: :doc:`plugin_rest`. C point/command model: :doc:`module_abi` and
:doc:`minihub_core`.

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

``{ status: number, body: string }``.

Attention
^^^^^^^^^

Response body cap **16384** bytes; ``main.js`` max **256 KiB**. URLs must pass Host
network policy.

Methods — host.config
---------------------

host.config.get()
~~~~~~~~~~~~~~~~~

Read-only ``adapters.json`` root object.

Returns
^^^^^^^

JavaScript object or ``null``.

Attention
^^^^^^^^^

JS cannot persist config; use REST ``PUT /api/config/adapters`` or L3 vtable
``config_save_adapters`` (:doc:`module_abi`).

See :doc:`../plugins/develop_l2`.
