MCP API
=======

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Device-side MCP tools live in ``components/minihub_mcp_server``. Enable with
``CONFIG_MINIHUB_MCP_SERVER`` (requires the ``components/esp-claw`` submodule).
Local MCP clients talk to the Hub through the product-wired MCP transport.

Tools return JSON text in MCP results by default. Most tools require Hub ready
(``minihub_mcp_hub_ready()``); during boot or reload you may get ``HUB_NOT_READY``.

REST equivalents: :doc:`host_rest` and :doc:`plugin_rest`.


Implementation: ``components/minihub_mcp_server/minihub_mcp_server.c`` (``s_tools[]``)
and ``hub_api_gateway.c`` (``hub.api`` allowlist).

hub.api
-------

.. container:: mh-api-block mh-api-command

   ``hub.api``

Allowlisted REST proxy.

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

   JSON text of the REST response body; errors for denied paths (e.g.
   ``/api/ota/upload``, non-allowlisted module RPC ``method``).

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

   JSON array of point snapshots (``point_id``, ``type``, ``value``,
   ``adapter``, ``online``, ``props`` including ``params`` when set).

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

   Single point object (same fields as ``points.list``); ``INVALID_ARG`` / not found on error.

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

   JSON merging local Hub history and module ``device_log`` when available.

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

   Confirmation JSON; for power-capable types, ``on``/``off``/``true``/``false``/``1``/``0``
   map to ``turn_on`` / ``turn_off`` via ``command.invoke``.

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

   Same result JSON as ``POST /api/command``.

rules.list
----------

.. container:: mh-api-block mh-api-command

   ``rules.list``

No parameters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   Current ``rules.json`` content as JSON.

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

   Confirmation JSON after replace and rules-engine hot reload.

adapters.get
------------

.. container:: mh-api-block mh-api-command

   ``adapters.get``

No parameters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   Read-only ``adapters.json`` snippet JSON.

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

   ``custom_url`` and ``disable_default`` from NVS as JSON.

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

   Confirmation JSON.

hub.api allowlist
-----------------

Gateway rules in ``hub_api_gateway.c`` (``s_rules[]``):

**GET**

* ``/api/``, ``/api/system``, ``/api/update/notes``, ``/api/points``, ``/api/points/history``
* ``/api/rules``, ``/api/adapters``, ``/api/config/adapters``
* ``/api/location``, ``/api/modules``, ``/api/packs``, ``/api/scenes``, ``/api/events``
* ``/api/store/catalog``, ``/api/store/config``, ``/api/ota``
* ``/api/matter/status`` (Matter build)
* ``/api/thread/status``, ``/api/thread/dataset`` (Thread build)
* ``/api/radio/provision``, ``/api/zigbee`` (Zigbee build)
* ``/api/modules/*/preview``

**POST**

* ``/api/command``, ``/api/rules/import``, ``/api/scenes/import``, ``/api/scenes/invoke``
* ``/api/store/config``, ``/api/store/install``, ``/api/store/upgrade``,
  ``/api/store/uninstall``, ``/api/store/refresh``
* ``/api/hub/reload``, ``/api/points/remove``, ``/api/ota``
* ``/api/thread/start``, ``/api/thread/stop`` (Thread build)
* Matter / Zigbee / BLE Mesh / Thread build paths
* ``/api/modules/*/rpc`` — only if RPC ``method`` is allowlisted

**PUT**

* ``/api/config/adapters``

Module RPC allowlist (``s_rpc_allow[]``)
----------------------------------------

``status``, ``homes``, ``devices``, ``device_log``, ``prop_get``, ``prop_probe``,
``camera_preview``, ``sync``, ``logout``, ``login``, ``session``, ``verify``,
``verify_send``, ``login_retry``

Other RPC methods must use direct LAN REST, not ``hub.api``.

