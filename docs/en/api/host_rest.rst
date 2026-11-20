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
Status codes and error bodies: :doc:`errors`.

Conventions
-----------

* Request/response bodies are JSON, except ``POST /api/ota/upload``: a raw
  ``.bin`` body with ``Content-Length`` (no multipart, ``Content-Type`` is not
  inspected).
* When ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on (default), use the ``mh_sess``
  cookie or ``Authorization: Bearer``; otherwise **401**. Public:
  ``GET /api/auth/status``, ``POST /api/auth/setup``, ``POST /api/auth/login``.
  Optional token: ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN``, **empty by default**, which
  wins when it is non-empty; the NVS key ``api_token`` (namespace ``minihub``) is read
  only when the compiled string is empty, so out of the box a device authenticates a
  script by the per-device NVS token or by a console session, not by a shared string.
* Many writes return ``{"result":"ok"}`` on success; errors use HTTP status + plain
  text or JSON body.


Route map
---------

Every route the running Host registers, read out of
``minihub_web_attach()`` (``components/minihub_web/minihub_web.c``) while the guide
builds: the URI, the method, whether the handler authenticates the request, and the
Kconfig gate it sits behind. The sections below document the ones a plugin or an
integration calls directly.

.. minihub-routes:: pages

.. minihub-routes:: auth

.. minihub-routes:: devices

.. minihub-routes:: config

.. minihub-routes:: store

.. minihub-routes:: update

.. minihub-routes:: automation

.. minihub-routes:: modules

.. minihub-routes:: radios

.. minihub-routes:: net

.. minihub-routes:: dev

``GET /ws/events`` is a WebSocket, not an ``httpd_uri_t``, so it is not in the map —
see its own section at the end of this page.

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
   * **HTTP 404** — the target point is not registered
   * **HTTP 500** — the adapter rejected the command
   * **HTTP 401** — unauthenticated when auth is enabled

.. container:: mh-api-block mh-api-attention

   **Attention**

   The body is read into a 256-byte buffer, so keep the request small. Use
   ``POST /api/devices/control`` (1 KiB) or a module RPC for larger payloads.
   ``params`` shape is adapter-specific. Point changes are pushed on WebSocket
   ``point.updated`` (see ``GET /ws/events``).

GET /api/points
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points``

Return a snapshot of the point registry.

.. container:: mh-api-block mh-api-parameters

   **Parameters (query string)**

   ``point_id`` (string, optional)
       Return that one point object instead of the whole array; **404** if it is
       not registered.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — without ``point_id``, a JSON array of ``point_id``,
     ``type``, ``value``, ``adapter``, ``online``, ``updated_ms``, and ``props``
     (including ``params`` when the adapter filled them); with ``point_id``, a
     single object with the same fields

.. container:: mh-api-block mh-api-attention

   **Attention**

   Field limits match macros in :doc:`minihub_core`. One device is one point;
   live parameters are under ``props.params``. Type labels/widgets:
   ``GET /api/device_widgets``.

GET /api/device_widgets
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/device_widgets``

Type catalog for the Overview page and the rules editor. Not instance state.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"devices":[…],"parameters":{…}}``. Each device:
     ``device`` (type id, e.g. ``light``), ``image`` (class avatar URI or
     empty), ``params[]`` with ``parameter``, ``label``, ``widget`` (``toggle`` /
     ``slider`` / ``dropdown`` / ``text`` / ``button`` / ``hidden``), ``image``
     (always empty; CSS widgets), ``style`` (the catalog row verbatim:
     ``widget``, ``labels``, ``min`` / ``max`` / ``step`` / ``unit``, ``access``,
     ``options``, ``css``, ``ui_element``, ``requires``), and optional
     ``point_id`` when a registered point carries that type and parameter (first
     match).
   * The top-level ``parameters`` map is keyed by parameter name and covers every
     row of the uploaded ``model/parameters.csv``, including names no device type
     uses. It is what lets the console label a parameter of a type the catalog
     never lists. On a stock Hub with no uploaded model CSV, it comes back empty.

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
       Concrete instance (e.g. ``sim.kitchen_lamp``). Host uses it only when the
       point exists, its ``type`` equals ``device``, **and** that point carries
       ``parameter`` in ``props.params``; otherwise it falls back to the first
       ``(type, parameter)`` match.

   ``seq`` (integer, optional)
       Sequence number for the report payload; default 0, echoed in the payload
       and in ``params.seq``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok","path":…,"parameter":…,"payload":…}``, where
     ``path`` is the point actually addressed (useful when you omitted
     ``point_id``) and ``payload`` is the report JSON the adapter received
   * **HTTP 400** — empty / truncated body, body over 1 KiB, invalid JSON,
     missing fields, or a serialized ``value`` of 256 bytes or more
   * **HTTP 404** — no registered point carries that ``(type, parameter)``, or
     the adapter reported ``ESP_ERR_NOT_FOUND``
   * **HTTP 500** — out of memory, a report payload over 384 bytes, or another
     adapter failure
   * **HTTP 401** — unauthenticated while login auth is on

.. container:: mh-api-block mh-api-attention

   **Attention**

   Resolution is driven by the live point, not the catalog: any
   ``(type, parameter)`` a registered point actually carries is accepted, even a
   parameter the type catalog never lists. So a mistyped parameter on a real
   point is a 404 only because no point carries it.

   The command name sent to the adapter is ``toggle`` when ``value`` is a JSON
   boolean and ``set`` otherwise — ``{"value":true}`` and ``{"value":false}``
   both dispatch ``toggle``, so use a string (``"on"`` / ``"off"``) when the
   adapter needs to tell them apart. Host JSON-serializes ``value`` (strings
   stay quoted); send numbers as JSON numbers and non-numeric dropdown options
   as strings. ``parameter`` is copied into a 64-byte buffer, so a longer name
   is truncated.

   The Devices UI always sends ``point_id`` from the live card so several
   devices of the same type stay independently controllable. Automation may also
   use ``POST /api/command`` with an explicit ``point_id``.

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
       Image path or URL; only the **first** of the three that is present is
       used. When none is given, Host copies ``props.ota.file`` into command
       ``path`` if set, otherwise ``props.ota.url``. Body is capped at 512 bytes.

Support differs by radio, and it is not a matter of the device answering:

* **Matter** — goes through the CSA OTA Provider / Requestor and BDX, but only
  when the build enables both ``CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED`` and
  ``CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER`` (on in the Matter feature config).
  A local file path is parsed for a Matter OTA image header and rejected without
  one; a plain ``http(s)`` URL is handed to the requestor unchecked.
* **Zigbee** and **BLE Mesh** — the air path is not implemented today, so these
  always return ``ESP_ERR_NOT_SUPPORTED``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``, meaning the offer was handed to the
     adapter, not that the device flashed anything
   * **HTTP 400** — missing ``point_id``, or the adapter returned
     ``ESP_ERR_NOT_SUPPORTED``
   * **HTTP 404** — unknown point
   * **HTTP 500** — other adapter errors. Note that an empty request is **not**
     rejected up front: the command still goes out with no image, and the
     adapter's ``ESP_ERR_INVALID_ARG`` comes back as 500. Same for Matter's
     ``ESP_ERR_INVALID_STATE`` while the commissioner is not ready.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Of ``props.ota``, the Host code path reads only ``file``, ``url`` and
   ``version``; ``available`` and ``status`` exist between the plugin and the
   console. The console shows an upgrade entry from ``available`` — a plugin that
   sets only ``url`` is still updatable over REST.

GET /api/points/history
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points/history``

Return recent ``value`` changes for one point. Core keeps a **global** ring of
160 entries across all points (a write is skipped when the value string is
unchanged); reads filter it by ``point_id``.

.. container:: mh-api-block mh-api-parameters

   **Parameters (query string)**

   ``point_id`` (string, required)
       Target point.

   ``limit`` (integer, optional)
       Max entries: default 48; values 1–128 are accepted, but Core replays at
       most 64 per read. The 160-entry ring is shared, so a busy point pushes
       other points' history out (MCP ``points.history`` caps at 50).

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"items":[…]}``, newest first; each entry has ``ts_ms``,
     ``old``, ``new``, and ``source`` (always ``local``). ``old`` / ``new`` are
     truncated to 31 characters.
   * **HTTP 400** — missing ``point_id``
   * **HTTP 404** — point not registered

POST /api/points/remove
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/remove``

Remove a point from the registry and persisted config when applicable.

.. warning::

   This is the one point operation that survives a reboot: the persisted
   registration goes with it, so the point comes back only if its adapter or pack
   registers that ``point_id`` again.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``point_id`` (string, required)
       Point to remove.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — point not found

POST /api/points/rename
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/rename``

Set the display name of a registered point and persist it. The name is written
to ``props.name`` on the point and to the device-name store, so the next boot
restores it.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body, max 512 bytes)**

   ``point_id`` (string, required)
       Registered point.

   ``name`` (string, required)
       Display name; truncated to 48 characters.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — missing fields, empty or invalid name
   * **HTTP 404** — point not registered

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
     ``enabled`` (bool), ``supported`` (bool), ``auth`` (bool), and when known
     ``module_id``, ``module_name``, ``module_version``, ``adapter_version``.
     ``supported`` is false when this firmware was built without that stack.
     A name the Hub does not know (a Store module) is ``supported``.

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

Soft-reload Hub: re-read JSON, reload JSON packs and Store modules, restart adapters
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

   * **HTTP 200** — JSON with ``name``, ``version``, ``idf_version``, ``commit``,
     ``uptime_s``, ``chip`` (``chip_cores`` / ``chip_revision``, ``flash_size``),
     ``ram`` (``internal_free`` / ``internal_total`` / ``psram_free`` /
     ``psram_total``), ``storage`` and ``modstore`` usage, ``local_ip``,
     ``web_base``, ``net`` (``eth`` / ``wifi``, the latter with ``ssid``,
     ``password``, and ``has_password``), ``timezone`` (``id``, ``offset_min``,
     ``zones``), and
     ``display_name`` (sidebar label shared by Web and LCD; empty means the UI
     default).

   Wall clock is not among these fields: ``uptime_s`` counts from boot, so this
   answer tells you how long the Hub has been up, not what time it thinks it is
   (:doc:`../time`).

POST /api/system/reboot · POST /api/system/reset
------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/system/reboot · POST /api/system/reset``

Reboot restarts after about half a second; reset schedules a factory reset.
Both require the JSON body ``{"confirm": true}``.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}`` (reboot answers before it restarts)
   * **HTTP 400** — body missing or ``confirm`` is not ``true``

.. warning::

   ``reset`` is the factory reset: it clears rules / scenes / adapters, every
   Store-installed module and pack, and all of NVS — Wi-Fi credentials, the Store URL
   and the console password included. What it leaves alone is the factory preinstall
   and the Web console. The full scope, and the reason a reset unit is unprotected
   until it has a password, are in :doc:`../production`.

GET / POST /api/system/timezone
-------------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/system/timezone``

GET returns ``id``, ``offset_min``, and the accepted ``zones`` list. POST body
is ``{"id":"UTC+8"}`` or ``{"id":"UTC+5:30"}``; offsets from ``UTC-12`` to
``UTC+14`` at whole-hour, half-hour or quarter-hour steps (minutes are 00, 15, 30
or 45), with ``UTC`` for zero. The choice is stored in NVS and applied immediately.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — the same status JSON as GET
   * **HTTP 400** — unknown ``id``

GET / POST /api/system/display_name
-----------------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/system/display_name``

GET returns ``{"display_name":"..."}``. POST body is
``{"display_name":"Lab Hub"}``: 1 to 48 printable bytes, no control characters.
Stored in ``/data/config/display_name.json`` and shared by the Web sidebar and
the LCD user row.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"display_name":"..."}``
   * **HTTP 400** — missing, empty, or invalid ``display_name``

GET /api/location
-----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/location``

Approximate location from the public IP, served from a cache. Geolocation
and the map are built only when ``CONFIG_MINIHUB_LOCAL_MAP`` is on
(default off). With it off the handler returns ``ok: false`` and does not
contact a geolocation or map service. ``?refresh=1`` starts a background
refresh when the option is on; the response is still the cached value.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``ok`` is false until the first lookup succeeds, and false
     when ``CONFIG_MINIHUB_LOCAL_MAP`` is off. When ``ok`` is true the body
     also has ``lat``, ``lon``, ``city``, ``region``, ``country``, ``address``,
     ``map_provider`` (``amap`` or ``osm``), and ``public_ip``. ``local_ip``
     is included when the option is on and the Hub has an address.

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

POST /api/net/wifi
------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/net/wifi``

Set the Wi-Fi STA credentials from the console. The Hub saves them and connects
now when the STA is active; during SoftAP provisioning or before Wi-Fi starts,
they apply on the next start.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body, max 512 bytes)**

   ``ssid`` (string, required)
       1–32 characters.

   ``password`` (string, optional)
       Up to 64 characters. Empty keeps the stored password unless ``open`` is
       true.

   ``open`` (boolean, optional)
       Connect to an open network with no password.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``; on failure ``{"result":"fail","error":"…"}``

GET /api/rules · POST /api/rules/import
---------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/rules · POST /api/rules/import``

GET returns full ``rules.json``. POST imports from JSON body or ``{"url":"http://…"}``.
An inline body may carry the whole document (up to 128 KiB); ``url`` must be
``http`` or ``https`` and shorter than 512 characters.
Point ``when`` / ``if`` prefer ``parameter`` + ``op`` + ``value`` (see
:doc:`../rules`).

GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke
-------------------------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke``

GET returns ``scenes.json``. import matches rules. invoke body: ``scene_id`` (required,
see :doc:`../data_model`); runs the scene's ``steps`` in order. Actions within a step
run in sequence (equivalent to ``POST /api/command``); each step can repeat with
``loop`` and wait with ``delay_after_ms``, bounded by ``max_duration_ms``. A scene
runs in its own worker task, and a new invoke or a config reload cancels the
running scene. **HTTP 200** on success. The legacy ``commands`` array still loads.

GET /api/events
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/events``

Recent Hub event log (HTTP pull; not WebSocket). Same ``event_type`` names as
``GET /ws/events``.

GET /api/update/notes
---------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/update/notes``

Authenticated read of the local Host firmware version, firmware note, Web
version, and Web note. Does not contact the deploy server. A missing cache
or file is ``""`` and the response is still **HTTP 200**.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``firmware_version``, ``firmware_note``, ``web_version``, ``web_note``

Update check and apply
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/update/hints · POST /api/update/check · GET /api/update/check · POST /api/update/apply``

These are the REST calls behind the ``System`` page update badge. ``hints``
returns the cached offers as ``firmware``, ``web``, ``assets`` (objects or
``null``) and ``plugins`` (array); nothing is fetched here.
``POST /api/update/check`` starts a background probe and answers
``{"result":"pending"}``, or ``"unavailable"`` if one is already running;
``GET /api/update/check`` reports ``"pending"`` or ``"idle"``. ``apply`` takes
``{"type":"firmware"|"asset"|"web","force":false}`` (256-byte body) and pulls
from the configured Store base URL. ``force`` reinstalls the version already in
use instead of skipping it.

The firmware index or file list is fetched inside the request, so ``apply`` can
take seconds. With ``type: firmware`` it only starts the background OTA task
documented under ``POST /api/ota``; ``asset`` and ``web`` write the files before
the response is sent.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok","pending":0}`` for apply; the count is
     always 0 because the handler reports the apply result, not a queue depth
   * **HTTP 400** — unknown ``type``, empty body, or invalid JSON
   * **HTTP 500** — no deploy URL, or the download/apply failed

GET /api/ota · POST /api/ota · POST /api/ota/upload
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/ota · POST /api/ota · POST /api/ota/upload``

GET returns OTA job status. POST body: ``url`` (required, HTTPS ``.bin``); **HTTP 200**
``{"result":"started"}``; **409** if already running. upload is a raw ``.bin`` with
``Content-Length``; not on MCP ``hub.api`` whitelist. A successful job stages the
image on external LittleFS media and hands off to Vibe Mode, which copies it into
``ota_0``; the device restarts twice. Operator chapter: :doc:`../ota`.
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
``POST /api/auth/logout`` clears the cookie. ``POST /api/auth/change`` takes
``old_password`` and ``new_password`` (6–64 chars) and answers **401** when the
old password is wrong. Skins: :doc:`../web_console`.

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

Write a Host console file under ``/data/web/`` (max 512 KiB).
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

WebSocket subscription to the Hub event bus. Every frame is JSON with at least
``event_type`` and ``timestamp_ms``. ``point.updated`` then adds
``point_index``, ``old_value`` and ``new_value``; every other type carries the
publisher's JSON in ``data``, and has no ``data`` key when the publisher passed
none. The socket forwards **any** name published on the bus
(``minihub_event_bus_publish``); plugins and OEM firmware may add their own.

.. only:: not hide_l3

   ELF modules publish with ``event_publish``.

Host-published ``event_type`` values:

.. list-table::
   :header-rows: 1
   :widths: 30 34 36

   * - ``event_type``
     - When
     - Extra fields
   * - ``point.updated``
     - The point's ``value`` string or its ``props`` JSON changed
       (``minihub_point_set`` and the merge helpers). Rules listen to this type.
     - ``point_index``, ``old_value``, ``new_value``. There is **no** ``data``,
       so the frame carries no ``point_id``: resolve the index against
       ``GET /api/points``, and note that it is a position in the registry, so
       it shifts when points are removed.
   * - ``point.registered``
     - A point was added. Re-registering an existing ``point_id`` updates it in
       place and publishes nothing.
     - ``data``: ``point_id``, ``type``, ``adapter``
   * - ``point.unregistered``
     - A point was removed — one frame per point, including the bulk removal
       done by a reload or ``minihub_point_unregister_by_adapter``.
     - ``data``: ``point_id``
   * - ``command.invoked``
     - Just before the command reaches the adapter handler, so it does **not**
       mean the command succeeded. Not published when the point or the adapter
       handler is missing.
     - ``data``: ``point_id``, ``cmd``
   * - ``adapter.loaded``
     - An adapter finished ``init`` and ``start`` and its command handler was
       registered.
     - ``data``: ``adapter``
   * - ``radio.permit_closed``
     - The Zigbee permit-join window closed.
     - ``data``: ``adapter`` (``zigbee``), ``reason`` when the stack supplies one
   * - ``hub.reloaded``
     - A soft reload finished — including the reloads triggered by install,
       enable, disable, or uninstall, which add and drop points.
     - ``data``: ``ok``
   * - ``store.module.installed``
     - A ``.mhub`` finished installing or upgrading.
     - ``data``: ``id``, ``type``, ``upgrade``
   * - ``device_widgets.updated``
     - Icons or per-parameter styles for one device type were synced.
     - ``data``: ``device``
   * - ``hub.ready``
     - Host start-up completed.
     - none

.. container:: mh-api-block mh-api-attention

   **Attention**

   ``point.updated`` is coalesced. While ``CONFIG_MINIHUB_POINT_DEBOUNCE_MS``
   (default 1000, 0 disables) has not elapsed since the last publish of a point,
   a new write updates the stored value but emits no event; a trailing-edge pass
   then publishes the final value. So the stream shows the settled value, not
   every transition — and history recording follows the same rule (see
   ``GET /api/points/history`` above).

The ESP-IDF provisioning manager publishes ``wifi.provisioning``,
``wifi.provision_failed`` and ``wifi.provisioned``. The shipped build configures
Wi-Fi from the open ``PROV_MiniHub`` AP instead, so those three appear only on a
product that calls ``minihub_provision_start()`` (:doc:`../networking`).

JS ``host.event.subscribe`` accepts a name or ``*`` (all events). There is
no Host-side filter list: subscribe to the types you need.

.. container:: mh-api-block mh-api-attention

   **Attention**

   Plugins must not register ``httpd`` URIs; publish points and Host forwards events.
   Unlike ``GET /api/events``, this is a live push stream.

MCP proxy
---------

Local agents reach a fixed subset through MCP ``hub.api`` (:doc:`mcp`). That
gateway has its own handlers, so its allowlist is not the same set as the routes
registered here. Authoritative URI list: ``minihub_web_attach()`` in
``components/minihub_web/minihub_web.c``.

