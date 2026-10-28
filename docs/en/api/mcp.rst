MCP API
=======

:link_to_translation:`zh_CN:[中文]`

Overview
--------

A Hub can present itself as an MCP server. An assistant can then drive a device
that has already been flashed. The tools live in
``components/minihub_mcp_server`` and are switched on with
``CONFIG_MINIHUB_MCP_SERVER`` (requires the ``components/esp-claw`` submodule). A
local MCP client reaches the Hub over the transport the product wires up.

Every tool answers as JSON text inside the MCP result. Most of them need a Hub
that has finished booting (``minihub_mcp_hub_ready()``), so a call made during
boot or reload can come back ``HUB_NOT_READY``.

REST equivalents: :doc:`host_rest` and :doc:`plugin_rest`. Every ``code`` a tool can
return: :doc:`errors`.


Implementation: ``components/minihub_mcp_server/minihub_mcp_server.c`` (``s_tools[]``)
and ``hub_api_gateway.c`` (``hub.api`` allowlist).

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
       e.g. ``/api/points``.

   ``query`` (string, optional)
       Query string without ``?``.

   ``body`` (string, optional)
       JSON string for POST/PUT.

.. container:: mh-api-block mh-api-returns

   **Returns**

   JSON text of the response. Denied paths answer ``DENIED``, allowlisted paths
   with no local handler answer ``NOT_IMPLEMENTED``, and a module RPC whose
   ``method`` is not allowlisted is refused.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Example — list points:

.. code-block:: json

   { "method": "GET", "path": "/api/points" }

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

   ``{"ok":true}``. For power-capable types, ``on``/``off``/``true``/``false``/
   ``1``/``0`` run ``turn_on`` / ``turn_off`` through the adapter; any other
   value is written to the point registry only.

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
gateway builds its own JSON from the same core calls. ``GET /api/ota`` here adds
``ok`` and ``busy`` next to ``state`` / ``error`` / ``progress`` /
``bytes_written`` / ``source`` (:doc:`host_rest`), and reads come back without
the HTTP status codes the REST route would use — a refusal is an ``error``
object inside a successful MCP result.

**GET — implemented**

* ``/api/system``, ``/api/points``, ``/api/points/history``, ``/api/ota``
* ``/api/rules``, ``/api/adapters``, ``/api/config/adapters``, ``/api/modules``
* ``/api/store/catalog``, ``/api/store/config``, ``/api/radio/provision``
* ``/api/matter/status`` (Matter build)
* ``/api/zigbee`` (Zigbee build) — a Zigbee summary built by the gateway; the
  Host HTTP server has no matching route

**POST — implemented**

* ``/api/command``, ``/api/rules/import``, ``/api/hub/reload``,
  ``/api/points/remove``, ``/api/ota``
* ``/api/store/config``, ``/api/store/install``, ``/api/store/upgrade``,
  ``/api/store/uninstall``, ``/api/store/refresh``
* ``/api/matter/commission``, ``/api/matter/devices`` (Matter build)
* ``/api/zigbee/permit_join`` (Zigbee build)
* ``/api/ble_mesh/permit_provision`` (BLE Mesh build)
* ``/api/modules/*/rpc`` — only when the RPC ``method`` is allowlisted

.. warning::

   The write half of this list means an MCP client is an **operator, not an observer**.
   Through ``hub.api`` it can replace the whole rules document, remove points, uninstall
   Store packages, start an OTA and reload the Hub. ``CONFIG_MINIHUB_MCP_SERVER`` is one
   switch with no read-only mode, and the allowlist is a compiled table
   (``s_rules[]``), so there is nothing to configure per client: whoever holds the token
   holds all of it. Enable the server only for an agent you trust to reconfigure the house.

**PUT — implemented**

* ``/api/config/adapters``

**Allowlisted, no local handler**

These answer ``NOT_IMPLEMENTED``. Use direct LAN REST (:doc:`host_rest`):

* GET: ``/api/``, ``/api/update/notes``, ``/api/location``, ``/api/packs``,
  ``/api/scenes``, ``/api/events``, ``/api/modules/*/preview``
* GET: ``/api/thread/status``, ``/api/thread/dataset`` (Thread build)
* POST: ``/api/scenes/import``, ``/api/scenes/invoke``
* POST: ``/api/thread/start``, ``/api/thread/stop``, ``/api/thread/dataset``
  (Thread build)

Module RPC allowlist (``s_rpc_allow[]``)
----------------------------------------

``status``, ``homes``, ``devices``, ``device_log``, ``prop_get``, ``prop_probe``,
``camera_preview``, ``sync``, ``logout``, ``login``, ``session``, ``verify``,
``verify_send``, ``login_retry``

Other RPC methods must use direct LAN REST, not ``hub.api``.

