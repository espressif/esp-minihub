MCP API
=======

:link_to_translation:`zh_CN:[中文]`

Overview
--------

A flashed Hub can serve MCP on the LAN so an assistant can read and drive points
and call allowlisted Host REST. ``CONFIG_MINIHUB_MCP_SERVER`` is on by default.
Tools live in ``components/minihub_mcp_server``; HTTP transport comes from the
``components/esp-claw`` submodule.

The server listens on HTTP port ``18791`` at path ``/mcp``. It starts after
IPv4 is available on Wi-Fi STA, Ethernet, or the provisioning SoftAP. LAN
discovery uses mDNS: service type ``_mcp._tcp``, hostname ``esp-minihub``,
instance name ``ESP-MiniHub``.

Every tool returns JSON text. Most tools require a ready Hub; a call during
boot or ``hub.reload`` may return ``HUB_NOT_READY``.

REST equivalents: :doc:`host_rest` and :doc:`plugin_rest`. Tool ``code`` values:
:doc:`errors`.

How to use
----------

Build and flash
~~~~~~~~~~~~~~~

1. ``git submodule update --init components/esp-claw``
2. ``CONFIG_MINIHUB_MCP_SERVER`` is on by default (``y``)
3. After flash, serial shows ``MCP server ready: http://esp-minihub.local:18791/mcp``

URL
~~~

* STA / Ethernet: ``http://<Hub_IPv4>:18791/mcp`` or ``http://esp-minihub.local:18791/mcp``
* SoftAP (SSID ``PROV_MiniHub``): ``http://192.168.4.1:18791/mcp``
* mDNS: type ``_mcp._tcp``, instance ``ESP-MiniHub``, host ``esp-minihub.local``

Client
~~~~~~

Any MCP client that speaks streamable HTTP. Example config::

   {
     "mcpServers": {
       "minihub": {
         "type": "streamable-http",
         "url": "http://HUB_IP:18791/mcp"
       }
     }
   }

Calls
~~~~~

1. ``hub.routes``
2. ``points.list`` (pass ``""`` for unused ``type`` / ``adapter``)
3. ``command.invoke`` or ``points.set``
4. ``hub.api`` — always pass ``method``, ``path``, ``query``, and ``body``::

      {
        "method": "GET",
        "path": "/api/system",
        "query": "",
        "body": ""
      }

   Common GETs: ``/api/system``, ``/api/points``, ``/api/store/catalog``,
   ``/api/store/config``, ``/api/ota``.

hub.api
-------

.. container:: mh-api-block mh-api-command

   ``hub.api``

Allowlisted REST-style access. The gateway runs the call in-process
(``hub_api_call``); it does not forward to ``minihub_web``'s HTTP server, so it
works with no LAN client and returns JSON directly.

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``method`` (string, required)
       ``GET``, ``POST``, or ``PUT``.

   ``path`` (string, required)
       Allowlisted path, e.g. ``/api/points`` or ``/api/system``.
       Not ``/api/status``, ``/api/version``, or ``/api/info``.

   ``query`` (string, always send)
       Query string without ``?``. Pass ``""`` when unused — the MCP SDK
       requires every registered property on each call.

   ``body`` (string, always send)
       JSON string for POST/PUT. Pass ``""`` on GET.

.. container:: mh-api-block mh-api-returns

   **Returns**

   JSON text of the response. Denied paths answer ``DENIED`` with ``hint`` and
   ``try`` (starter paths). Full allowlist: ``hub.routes``. Allowlisted paths
   with no local handler answer ``NOT_IMPLEMENTED``. A module RPC whose
   ``method`` is not allowlisted is refused.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Example — Hub status and list points:

.. code-block:: JSON

   { "method": "GET", "path": "/api/system", "query": "", "body": "" }

.. code-block:: JSON

   { "method": "GET", "path": "/api/points", "query": "", "body": "" }

points.list
-----------

.. container:: mh-api-block mh-api-command

   ``points.list``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``type`` (string, optional)
       Filter by point type.

   ``adapter`` (string, optional)
       Filter by adapter id.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"points":[…]}``; each snapshot has ``point_id``, ``type``,
   ``value``, ``adapter``, ``online``, and ``props`` including ``params`` when
   set.

points.get
----------

.. container:: mh-api-block mh-api-command

   ``points.get``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)
       Target point id.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"point":{…}}`` with the same fields as ``points.list``;
   ``INVALID_ARG`` or ``NOT_FOUND`` on error.

points.history
--------------

.. container:: mh-api-block mh-api-command

   ``points.history``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``limit`` (integer, optional)
       Default 20, max 50.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"point_id":…,"items":[…]}``, newest first. Each item has
   ``ts_ms``, ``old``, ``new``, ``label``, and ``source`` (``local`` or
   ``cloud``). The local Hub ring is merged with the owning adapter's
   ``device_log`` when that RPC exists; a cloud row is dropped when it has the
   same ``label`` as an existing row within 2 seconds. Reasons for a failed
   ``device_log`` call land in ``cloud_error``.

points.set
----------

.. container:: mh-api-block mh-api-command

   ``points.set``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``value`` (string, required)
       String value written to the point registry.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true}``. When the point's ``props.params`` includes ``power``,
   ``on``/``off``/``true``/``false``/``1``/``0`` run ``turn_on`` / ``turn_off``
   through the adapter; any other value is written to the point registry only.

command.invoke
--------------

.. container:: mh-api-block mh-api-command

   ``command.invoke``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``cmd`` (string, required)

   ``params_json`` (string, optional)
       JSON object string for adapter params. For catalog devices, include
       ``parameter`` so the adapter updates ``props.params`` on that one point.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true}``; ``INVOKE_FAILED`` when the adapter rejects the command.

rules.list
----------

.. container:: mh-api-block mh-api-command

   ``rules.list``

No parameters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   The current ``rules.json`` object with ``"ok":true`` added.

rules.put
---------

.. container:: mh-api-block mh-api-command

   ``rules.put``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``rules_json`` (string, required)
       Full rules document string.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"count":N}`` after the rules engine accepts and stores the
   document; ``VALIDATION_FAILED`` / ``RELOAD_FAILED`` / ``SAVE_FAILED`` when it
   does not.

adapters.get
------------

.. container:: mh-api-block mh-api-command

   ``adapters.get``

No parameters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   Read-only ``adapters.json`` object with ``"ok":true`` added.

hub.routes
----------

.. container:: mh-api-block mh-api-command

   ``hub.routes``

No parameters. Returns the compile-time ``hub.api`` allowlist (method + path)
from ``s_rules[]``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"note":…,"routes":[{"method":"GET","path":"/api/system"},…]}``.

hub.reload
----------

.. container:: mh-api-block mh-api-command

   ``hub.reload``

No parameters; calls ``minihub_hub_reload()``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   Confirmation JSON.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Rate-limited to once per 5 s.

store.config.get
----------------

.. container:: mh-api-block mh-api-command

   ``store.config.get``

No parameters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"custom_url":…,"disable_default":…,"default_url":…}``.

store.config.set
----------------

.. container:: mh-api-block mh-api-command

   ``store.config.set``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``custom_url`` (string)
       ``http(s)://host:port``; empty clears override.

   ``disable_default`` (boolean)
       Skip built-in Store URL when true.

.. container:: mh-api-block mh-api-returns

   **Returns**

   The same shape as ``store.config.get``.

hub.api allowlist
-----------------

Gateway rules in ``hub_api_gateway.c`` (``s_rules[]``). A path passes the
allowlist first, then needs a local dispatch branch; without one the call
answers ``NOT_IMPLEMENTED`` even though the path is listed.

An implemented path is **not** a byte-for-byte copy of the REST answer: the
gateway builds its own JSON from the same core calls. ``GET /api/system``
returns ``ok``, ``name``, ``version``, ``idf_version``, ``commit``, ``uptime_s``,
``chip``, ``local_ip``, and ``web_base`` (a subset of Host REST
:doc:`host_rest` — no RAM / storage / net detail). ``GET /api/update/notes``
returns the same object as Host REST
(``firmware_version`` / ``firmware_note`` / ``web_version`` / ``web_note``).
``GET /api/ota`` here adds ``ok`` and ``busy`` next to ``state`` / ``error`` /
``progress`` / ``bytes_written`` / ``source``, and reads come back without the
HTTP status codes the REST route would use — a refusal is an ``error`` object
inside a successful MCP result.

**GET — implemented**

* ``/api/system``, ``/api/update/notes``, ``/api/points``, ``/api/points/history``,
  ``/api/ota``
* ``/api/rules``, ``/api/scenes``, ``/api/adapters``, ``/api/config/adapters``,
  ``/api/modules``
* ``/api/store/catalog``, ``/api/store/config``, ``/api/radio/provision``
* ``/api/matter/status`` (Matter build)
* ``/api/zigbee`` (Zigbee build) — a Zigbee summary built by the gateway; the
  Host HTTP server has no matching route

.. warning::

   Writes through ``hub.api`` can replace rules, remove points, uninstall Store
   packages, start an OTA, reload the Hub, and reboot the device.
   ``CONFIG_MINIHUB_MCP_SERVER`` is one switch with no read-only mode; the
   allowlist is compile-time ``s_rules[]`` and is not per-client.

**POST — implemented**

* ``/api/command``, ``/api/rules/import``, ``/api/scenes/import``,
  ``/api/scenes/invoke``, ``/api/hub/reload``, ``/api/system/reboot``
  (body ``{"confirm":true}``), ``/api/points/remove``, ``/api/ota``
* ``/api/store/config``, ``/api/store/install``, ``/api/store/upgrade``,
  ``/api/store/uninstall``, ``/api/store/refresh``
* ``/api/matter/commission``, ``/api/matter/devices`` (Matter build)
* ``/api/zigbee/permit_join`` (Zigbee build)
* ``/api/ble_mesh/permit_provision`` (BLE Mesh build)
* ``/api/modules/*/rpc`` — only when the RPC ``method`` is allowlisted

**PUT — implemented**

* ``/api/config/adapters``

**Allowlisted, no local handler**

These answer ``NOT_IMPLEMENTED``. LAN REST: :doc:`host_rest`.

* GET: ``/api/``, ``/api/location``, ``/api/packs``, ``/api/events``,
  ``/api/modules/*/preview``
* GET: ``/api/thread/status``, ``/api/thread/dataset`` (Thread build)
* POST: ``/api/thread/start``, ``/api/thread/stop``, ``/api/thread/dataset``
  (Thread build)

Module RPC allowlist (``s_rpc_allow[]``)
----------------------------------------

``status``, ``homes``, ``devices``, ``device_log``, ``prop_get``, ``prop_probe``,
``camera_preview``, ``sync``, ``logout``, ``login``, ``session``, ``verify``,
``verify_send``, ``login_retry``

Other RPC methods are not allowlisted. LAN REST: :doc:`host_rest`.

