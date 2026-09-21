Host REST API
=============

:link_to_translation:`zh_CN:[中文]`

Overview
--------

HTTP JSON API exposed by the Host web stack for LAN integration, automation, and
operations. Default port **80**. WebSocket GET /ws/events mirrors the same event bus
as the rule engine.

Implementation: ``components/minihub_web/minihub_web.c``. Replace ``HUB`` with the
device IPv4 (``eth got IP`` / ``wifi got IP`` in logs).

Store plugin install/RPC: :doc:`plugin_rest`. C command routing: :doc:`minihub_core`.

Conventions
-----------

* Request/response bodies are JSON unless noted (OTA upload uses multipart).
* When ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on (default), use the ``mh_sess``
  cookie or ``Authorization: Bearer``; otherwise **401**. Public:
  ``GET /api/auth/status``, ``POST /api/auth/setup``, ``POST /api/auth/login``.
  Optional token: ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` (NVS ``api_token``).
* Many writes return ``{"result":"ok"}`` on success; errors use HTTP status + plain
  text or JSON body.


URIs are registered in ``minihub_web_attach()`` (``components/minihub_web/minihub_web.c``).

POST /api/command
-----------------

.. container:: mh-api-block mh-api-command

   ``POST /api/command``

Route a command to the adapter that owns ``point_id`` (same as MCP ``command.invoke``).

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``point_id`` (string)
       Target point id, max 64 chars; must be registered.

   ``cmd`` (string)
       Command name (e.g. ``turn_on``), max 32 chars; adapter-specific.

   ``params`` (object, optional)
       Adapter-specific JSON. For catalog devices, set ``parameter`` to the
       param name (e.g. ``brightness``) so the adapter updates
       ``props.params`` on that one device point.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — empty body, invalid JSON, or missing ``point_id`` / ``cmd``
   * **HTTP 404** — ``minihub_command_invoke`` failed; body is ``esp_err_to_name``
   * **HTTP 401** — unauthenticated when auth is enabled

.. container:: mh-api-block mh-api-attention

   **Attention**

   ``params`` shape is adapter-specific. Point changes are pushed on WebSocket
   ``point.updated`` (see ``GET /ws/events``).

GET /api/points
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points``

Return a snapshot of the point registry.

.. container:: mh-api-block mh-api-parameters

   **Parameters (query string)**

   ``type`` (string, optional)
       Filter by point ``type`` (e.g. ``light``, ``sensor``).

   ``adapter`` (string, optional)
       Filter by owning adapter name.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON array: ``point_id``, ``type``, ``value``, ``adapter``,
     ``online``, and ``props`` (including ``params`` when the adapter filled them)

.. container:: mh-api-block mh-api-attention

   **Attention**

   Field limits match macros in :doc:`minihub_core`. One device is one point;
   live parameters are under ``props.params``. Type labels/widgets:
   ``GET /api/device_widgets``.

GET /api/device_widgets
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/device_widgets``

Type catalog for the Devices page and the rules editor. Not instance state.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"devices":[…]}``. Each device: ``device`` (type id, e.g.
     ``light``), ``image`` (class avatar URI or empty), ``params[]`` with
     ``parameter``, ``label``, ``widget`` (``toggle`` / ``slider`` /
     ``dropdown`` / ``text`` / ``hidden``), ``image`` (always empty; CSS
     widgets), ``style`` (JSON: min/max/step/unit/options), and optional
     ``point_id`` when a registered point of that type exists (first match).

.. container:: mh-api-block mh-api-attention

   **Attention**

   Catalog is protocol-agnostic. Transport ``point_id`` comes from registration
   (``protocol.device``). Hide a param with instance ``props.params.NAME.visible``,
   not a catalog column. Model tables:
   ``components/minihub_device_widget/README.md``.

POST /api/devices/control
-------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/devices/control``

Devices-page write: resolve a registered point, then invoke the same command
path as ``POST /api/command``.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``device`` (string)
       Data-model type (``light``, ``switch``, …).

   ``parameter`` (string)
       Catalog parameter name (``power``, ``brightness``, …).

   ``value`` (string or JSON)
       New value.

   ``point_id`` (string, optional)
       Concrete instance (e.g. ``sim.kitchen_lamp``). When present and that
       point exists with a matching ``type``, Host uses it. Otherwise falls
       back to first-match ``(type, parameter)`` resolve.

   ``seq`` (integer, optional)
       Sequence number for the report payload.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — command succeeded
   * **HTTP 400** — empty / truncated body, body larger than 1024 bytes,
     invalid JSON, missing fields, or ``value`` too long
   * **HTTP 404** — unknown catalog pair, or no registered point of that type

.. container:: mh-api-block mh-api-attention

   **Attention**

   The Devices UI always sends ``point_id`` from the live card so several
   devices of the same type stay independently controllable. Omitting
   ``point_id`` still uses first registered match (legacy clients). Automation
   may also use ``POST /api/command`` with an explicit ``point_id``.
   Host JSON-serializes ``value`` (strings stay quoted). Send numbers as JSON
   numbers; non-numeric dropdown options as strings.

POST /api/devices/ota
---------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/devices/ota``

Trigger **end-device** OTA for a registered point (not Hub firmware
``/api/ota``). Invokes adapter command ``ota`` for that point's adapter.
Plugins publish readiness under ``props.ota`` (see :doc:`../data_model`).

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``point_id`` (string, required)
       Target device point.

   ``path`` / ``file_path`` / ``url`` (string, optional)
       Image path or URL. When omitted, Host copies ``props.ota.file`` into
       command ``path`` if set, otherwise ``props.ota.url``. Matter points use
       the CSA Provider / Requestor / BDX path (Matter OTA image header
       required). Zigbee and BLE Mesh air paths may still return
       ``ESP_ERR_NOT_SUPPORTED``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — command accepted
   * **HTTP 400** — missing ``point_id``, or adapter returned
     ``ESP_ERR_NOT_SUPPORTED``
   * **HTTP 404** — unknown point
   * **HTTP 500** — other adapter errors

GET /api/points/history
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points/history``

Return recent value-change history for one point (Hub RAM ring buffer).

.. container:: mh-api-block mh-api-parameters

   **Parameters (query string)**

   ``point_id`` (string, required)
       Target point.

   ``limit`` (integer, optional)
       Max entries (default ~20; MCP allows up to 50).

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON array with timestamp and value-change fields.

POST /api/points/remove
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/remove``

Remove a point from the registry and persisted config when applicable.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``point_id`` (string, required)
       Point to remove.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — point not found

GET /api/adapters
-----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/adapters``

Runtime adapter **overview** for the Web UI (chips / enable flags). This is
**not** the full ``adapters.json`` document — that is
``GET /api/config/adapters``.

Implementation: ``handle_api_adapters`` →
``minihub_module_overview_adapters_json``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON object keyed by adapter name. Each value includes
     ``enabled`` (bool), ``auth`` (bool), and when known ``module_id``,
     ``module_name``, ``module_version``, ``adapter_version``.

GET /api/config/adapters
------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/config/adapters``

Return the full runtime ``adapters.json`` (LittleFS ``/data/config/adapters.json``).

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON object (see :doc:`../data_model`)

PUT /api/config/adapters
------------------------

.. container:: mh-api-block mh-api-command

   ``PUT /api/config/adapters``

Replace and persist the full ``adapters.json`` document.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   Root ``adapters.json`` object; fields in :doc:`../data_model`.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — typically ``{"result":"ok"}``

.. container:: mh-api-block mh-api-attention

   **Attention**

   Do not bake vendor cloud tokens into generic Host factory JSON; plugins store credentials in
   NVS after user login. PUT overwrites the whole file — GET, merge, then PUT from
   integrations.

POST /api/hub/reload
--------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/hub/reload``

Soft-reload Hub: re-read JSON, reload L1 packs and Store modules, restart adapters
and JS without full reboot. See :doc:`../architecture`.

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   None, or empty JSON body.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``

.. container:: mh-api-block mh-api-attention

   **Attention**

   MCP ``hub.reload`` is rate-limited to once per 5 s; REST has no identical limit but
   MCP may return ``HUB_NOT_READY`` during reload. Matter + Thread builds take longer.

GET /api/system
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/system``

Hub runtime summary for Web UI and ops scripts.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON; common fields: ``uptime``, ``version`` / ``features``,
     ``ip`` / ``network`` (varies by Kconfig).

POST /api/net/config
--------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/net/config``

Set Ethernet or Wi-Fi DHCP/static IP. Details in :doc:`../networking`.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``iface`` — ``eth`` or ``wifi``

   ``dhcp`` — boolean; ``true`` for DHCP

   ``ip``, ``netmask``, ``gw`` — static addresses when ``dhcp`` is false

   ``dns`` or ``dns_main`` / ``dns_backup`` — DNS servers

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``; config saved to NVS and applied

GET /api/rules · POST /api/rules/import
---------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/rules · POST /api/rules/import``

GET returns full ``rules.json``. POST imports from JSON body or ``{"url":"http://…"}``.
Point ``when`` / ``if`` prefer ``parameter`` + ``op`` + ``value`` (see
:doc:`../rules`).

GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke
-------------------------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke``

GET returns ``scenes.json``. import matches rules. invoke body: ``scene_id`` (required,
see :doc:`../data_model`); runs scene ``commands`` like sequential ``POST /api/command``;
**HTTP 200** on success.

GET /api/events
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/events``

Recent Hub event log (HTTP pull; not WebSocket). Same ``event_type`` names as
``GET /ws/events``.

GET /api/ota · POST /api/ota · POST /api/ota/upload
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/ota · POST /api/ota · POST /api/ota/upload``

GET returns OTA job status. POST body: ``url`` (required, HTTPS ``.bin``); **HTTP 200**
``{"result":"started"}``; **409** if already running. upload is a raw ``.bin`` with
``Content-Length``; not on MCP ``hub.api`` whitelist. Operator chapter: :doc:`../ota`.
Factory partitions: :doc:`../production`.

Protocol-stack REST (Kconfig-gated)
-----------------------------------

Present only when the stack is compiled into Host; JSON examples in :doc:`../matter`,
:doc:`../zigbee`, and :doc:`../ble_mesh`.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Prefix
     - Purpose
   * - ``/api/zigbee/*``
     - Join, install code, device upsert
   * - ``/api/ble_mesh/*``
     - Provisioning permission, device upsert
   * - ``/api/matter/*``
     - Controller state, commissioning, device list
   * - ``/api/thread/*``
     - ``POST /api/thread/start`` and ``/api/thread/stop``, plus Operational
       Dataset (``/api/thread/dataset``) and ``GET /api/thread/status``
   * - ``/api/radio/provision``
     - RF / provisioning coexistence status

Auth REST
---------

Public when Web login is on. ``GET /api/auth/status`` returns JSON
``login_auth``, ``password_set``, ``logged_in``. First boot:
``POST /api/auth/setup`` with JSON ``username`` (must be ``admin``) and
``password`` (6–64 chars). Later: ``POST /api/auth/login`` with the same fields;
``POST /api/auth/logout`` clears the cookie. Skins: :doc:`../web_console`.

Copy-paste (default login on). ``-c`` stores ``mh_sess``; later calls send it
with ``-b``. First password uses ``setup`` (409 if already set); later calls
use ``login``.

.. code-block:: bash

   curl -s -c /tmp/mh_cookie -X POST http://HUB/api/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"admin","password":"secret1"}'
   curl -s -b /tmp/mh_cookie http://HUB/api/system

Bearer, when ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` is set:
``-H "Authorization: Bearer TOKEN"`` instead of the cookie file.

PUT /api/dev/web/
-----------------

.. container:: mh-api-block mh-api-command

   ``PUT /api/dev/web/`` *relative-path*

Write a Host console file under ``/store/preinstall/web/`` (max 512 KiB).
Example: ``PUT /api/dev/web/login.html``. Requires the same auth as other
writes when login or a Bearer token is configured. Operator flow:
:doc:`../web_console`.

GET / PUT /api/dev/catalog/
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET`` / ``PUT /api/dev/catalog/devices.csv``

   ``GET`` / ``PUT /api/dev/catalog/parameters.csv``

Needs ``CONFIG_MINIHUB_WEB_UI``. PUT writes the model CSV under
``/data/config/model/`` (max 128 KiB). When **both** files are present, Host
flattens them to ``/data/config/devices.csv`` and reloads the widget registry
(``rebuilt: true``). Reload is atomic (broken CSV keeps the previous table;
corrupt rows are skipped). GET returns the uploaded model file, not the flatten.

Local helper: ``python3 tools/dev_sync.py --open``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200 GET** — ``text/csv``
   * **HTTP 200 PUT** — JSON ``result``, ``file``, ``rebuilt``, ``bytes``
   * **HTTP 400** — wrong name (only those two files) or invalid CSV
   * **HTTP 404 GET** — that model file not uploaded yet

GET /ws/events
--------------

.. container:: mh-api-block mh-api-command

   ``GET /ws/events``

WebSocket subscription to the Hub event bus. Each frame is JSON with
``event_type`` (string) plus payload fields. The socket forwards **any** name
published on the bus (``minihub_event_bus_publish`` / L3
``event_publish``). Plugins and OEM firmware may add their own types.

Host-published ``event_type`` values:

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - ``event_type``
     - When
   * - ``point.updated``
     - Point value/props changed (``minihub_point_set`` / merge). Payload
       includes ``point_id``, ``value``, ``props``, ``old_value``,
       ``new_value``. Rules listen to this type.
   * - ``point.registered``
     - New point in the registry (``data.point_id``).
   * - ``point.unregistered``
     - Point removed.
   * - ``command.invoked``
     - After ``minihub_command_invoke``.
   * - ``adapter.loaded``
     - Adapter registered/loaded.
   * - ``radio.permit_closed``
     - Zigbee permit-join window closed.
   * - ``wifi.provisioning``
     - Wi-Fi provisioning started (when that feature is compiled in).
   * - ``wifi.provision_failed``
     - Provisioning failed.
   * - ``wifi.provisioned``
     - Provisioning ended.

L2 JS ``host.event.subscribe`` accepts a name or ``*`` (all events). There is
no Host-side filter list: subscribe to the types you need.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Plugins must not register ``httpd`` URIs; publish points and Host forwards events.
   Unlike ``GET /api/events``, this is a live push stream.

MCP proxy
---------

Local agents may call most GET/POST endpoints via MCP ``hub.api`` (:doc:`mcp`) on a
whitelist subset. Authoritative URI list: ``minihub_web_attach()`` registration table.

