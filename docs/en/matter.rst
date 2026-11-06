Matter
======

:link_to_translation:`zh_CN:[中文]`

MiniHub uses `Espressif's SDK for Matter <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_
as a **Matter controller** (commissioner), not as a simple end-device sample. Matter
sits above RF (Wi-Fi / Ethernet / Thread). Devices live under ``adapters.matter``
with optional per-device ``transport``: ``wifi``, ``ethernet``, or ``thread``.

Upstream:

* `Developing with the SDK <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html>`_
* `Matter Controller <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/matter_controller.html>`_
* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_

Build
-----

Enable ``CONFIG_MINIHUB_MATTER_ADAPTER``, export ``ESP_MATTER_PATH``, then build;
the script pulls in Matter when that option is ``=y``. For Matter-over-Thread
also enable ``CONFIG_MINIHUB_THREAD_ADAPTER`` (mutually exclusive with Zigbee);
Thread holds the 2.4 GHz radio, so the Hub's own uplink is then Ethernet
(:doc:`radios`).

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

Compose then includes ``matter.defaults``, ``matter_over_eth.defaults``, and
``matter_ble.defaults``; Thread adds ``thread.defaults``.

Thread dataset: ``GET/POST /api/thread/dataset``, then commission Thread end
devices.

Runtime configuration
---------------------

Enable in ``/data/config/adapters.json`` (factory JSON: ``application/minihub/configs/adapters.json``):

.. code-block:: JSON

   {
     "adapters": {
       "matter": {
         "enabled": true,
         "devices": [
           {
             "node_id": 2,
             "point_id": "matter.light.2",
             "endpoint_id": 1,
             "transport": "wifi"
           }
         ]
       }
     }
   }

JSON fields: :ref:`adapters-JSON`.

REST endpoints
--------------

* ``GET /api/matter/status`` — commissioner ready / pairing state and per-node
  ``subscriptions`` liveness (JSON)
* ``POST /api/matter/commission`` — start pairing only
* ``POST /api/matter/devices`` — commission (optional) + upsert into ``adapters.json``

Commission (pairing only)
^^^^^^^^^^^^^^^^^^^^^^^^^

**Network discovery** (device already on LAN / multi-admin):

.. code-block:: bash

   curl -s -X POST http://HUB/api/matter/commission \
     -H 'Content-Type: application/json' \
     -d '{"node_id":2,"setup_pin":20202021,"discovery":"network"}'

**BLE + device Wi-Fi** (first-time; requires ``pairing_code``, not ``setup_pin``):

.. code-block:: JSON

   {
     "node_id": 2,
     "pairing_code": "MT:...",
     "discovery": "ble_wifi",
     "wifi_ssid": "MyHome",
     "wifi_password": "secret"
   }

**Response**

.. code-block:: JSON

   { "status": "started" }

On error: ``status`` is an ``ESP_ERR_*`` name; optional ``message`` string.

Discovery values (``discovery`` field)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The Web add-device dialog always pairs over BLE and injects credentials, then
resolves the joined device with DNS-SD on the fabric:

* ``ble_thread`` (default when Wi-Fi toggle is off) — BLE + inject Hub Thread dataset.
  Requires ``CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION`` (default y, depends on the Thread
  adapter). When that option is n, ``GET /api/matter/status`` reports
  ``ble_thread: false`` and the dialog sends ``network`` instead.
* ``ble_wifi`` / ``first_time`` — BLE + inject **device** Wi-Fi credentials

``POST /api/matter/commission`` still accepts ``network`` (DNS-SD only / multi-admin)
and ``ble_ethernet`` for API clients; they are not offered in the overview dialog.
Only the ``ble_ethernet`` path asks CHIP to browse every discovery type at once;
the two dialog paths go through esp-matter's Wi-Fi / Thread pairing helpers.
An in-flight commissioning can be stopped with
``POST /api/matter/commission`` and ``{"cancel": true}`` (the dialog shows
**取消配网**), which answers ``status: "cancelled"``.

Commission + persist devices
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``POST /api/matter/devices`` with ``pairing_code`` **upserts the device immediately**
with ``commission_phase: "waiting"`` (overview shows **waiting to join**,
``等待入网中``), starts
commissioning, and returns ``commission: "started"`` without blocking the HTTP
handler. On success the phase is cleared and the card becomes a normal device; on
failure or the commission watchdog the card stays with
``commission_phase: "failed"`` (deletable). The watchdog is
``CONFIG_MINIHUB_MATTER_COMMISSION_WAIT_MS``, default **300 s** (60–900 s),
because a first DNS-SD browse on the fabric can take minutes — an older 180 s
figure you may find in notes is stale. Poll ``GET /api/matter/status`` for
``commission``: ``idle`` | ``running`` | ``ok`` | ``failed``. Omitting
``pairing_code`` still upserts without commissioning (``commission: "skipped"``).

.. code-block:: JSON

   {
     "pairing_code": "MT:...",
     "node_id": 2,
     "endpoints": [1],
     "discovery": "ble_thread",
     "transport": "thread",
     "merge": true,
     "reload": true
   }

* ``node_id`` — omit or < 2 to auto-allocate next id
* ``endpoints`` — array of numbers or comma string ``"1,2"``; default ``[1]``
* ``merge`` — default true; merge with existing ``devices[]``
* ``reload`` — default true; async hub reload after upsert / finalize
* ``delete`` — when true, remove by ``point_id`` instead (same as points/remove)

**Immediate response (commission started)**

.. code-block:: JSON

   {
     "result": "ok",
     "node_id": 2,
     "synced": 1,
     "reloaded": true,
     "commission": "started"
   }

**After success**: ``GET /api/matter/status`` shows ``commission: "ok"`` and the
overview card drops the waiting state.

**Delete example**

.. code-block:: JSON

   { "delete": true, "point_id": "matter.light.2", "reload": true }

Control
-------

After CASE session succeeds, control lights via standard Hub command API:

.. code-block:: bash

   curl -s -X POST http://HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"matter.light.2","cmd":"turn_on"}'

Supported today: OnOff cluster (``turn_on`` / ``turn_off`` / ``toggle``). That write is
optimistic — see :ref:`matter-state-reporting` for how the device's own value comes back.

.. _matter-state-reporting:

State reporting
---------------

``/api/command`` stores the requested value the moment the OnOff command is handed to
CHIP, so the card shows what MiniHub asked for, not what the device ended up doing.
The device's own state arrives over an attribute subscription.

For every commissioned node the Matter adapter arms **one** subscription covering the
OnOff attribute (``0x0000`` of cluster ``0x0006``) on all of that node's endpoints in
``adapters.json``. It is armed when the adapter starts, armed again when a commissioning
turns a pending row into a device (adapter ``resync``), and re-armed from the adapter
poll pass if a subscription died. Each report writes that endpoint's point (``on`` /
``off``) through the generic point registry. A report never overwrites a row whose
``commission_phase`` is still set, and never marks such a row online.

Report delivery is one callback slot: ``minihub_matter_set_attr_cb()``. The Matter
adapter registers it at ``init`` and never clears it, so today it is the only consumer.

Whether reports are arriving
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   curl -s http://HUB/api/matter/status

.. code-block:: JSON

   {
     "ready": true,
     "last_error": "subscribe connect failed: Error CHIP:0x00000032",
     "subscriptions": [
       { "node_id": 2, "endpoints": [1], "live": true, "reports": 3 }
     ]
   }

``subscriptions`` is present in both build configurations; the Matter-stub build emits
an empty array. The numbers above show the field shape, not a bench reading.

* ``live`` — that node's subscription is established. Liveness is tracked **per node**,
  not per endpoint: the ESP-Matter established / terminated callbacks carry only
  ``(node_id, subscription_id)``, so all endpoints of one node rise and fall together.
* ``reports`` — boolean reports delivered for that node. OnOff is the only attribute
  armed, so in practice these are OnOff reports; the counter itself is not filtered by
  cluster. It is cumulative per node and resets only when that node's row is torn down
  (``minihub_matter_drop_node``) or reallocated to a different node — re-arming a node
  that is not live does **not** clear it. Read this, not ``value``, as the proof that a
  report arrived: ``minihub_point_set()`` ignores an unchanged value, so ``value`` cannot
  tell "the device reported ``off`` " from "nothing happened".
* ``last_error`` — the most recent commissioner error, shown only while non-empty (for
  example the CHIP connect error above). It is **not** a per-node log: a later unrelated
  success that clears it (an OnOff command, a new commissioning) overwrites or empties it,
  so a failed arm stays visible only until then. The poll pass retries a node that never
  came up, but it runs only once a minute and counts a node's backoff when it arms, so with
  a stopwatch you see a retry on the next minute, then roughly 2, 4 and 8 minutes later,
  settling at the 10-minute cap — the nominal 30-second floor is never observable.
* A node whose endpoint set changed is torn down first (a real
  ``send_shutdown_subscription``) and then re-armed; a node that disappeared from
  ``adapters.json`` is re-associated the same way, because a re-commissioned device gets a
  **new** node id and the old row would otherwise stay armed forever.

Bench demo
^^^^^^^^^^

The observable that must appear — this is the acceptance condition, see the status note
below:

1. ``GET /api/matter/status`` shows the node with ``live`` true.
2. Reset the card, so the Hub's own write cannot be mistaken for a report:
   ``POST /api/hub/reload`` re-publishes the configured rows. Read the baseline value from
   ``GET /api/points`` and the baseline ``reports`` from ``GET /api/matter/status`` — the
   point list does not carry ``reports``.
3. Change the device **without** the Hub — its own button, a power cycle, or a second
   controller. Issue no ``/api/command`` while watching.
4. Read ``GET /api/matter/status`` and ``GET /api/points`` again. **Pass**: ``reports``
   went up and the point value moved to the device's real state. If the value moved while
   ``reports`` stayed flat, a Hub command wrote it and that run proves nothing.

**Pending a reachable device.** This procedure has not been run end to end anywhere. On
the development bench the commissioned node does not answer CASE, so ``live`` stayed
``false``, ``reports`` stayed ``0``, and no report ever arrived — steps 3 and 4 are
therefore unverified, including how long a report takes to show up. The subscribe
intervals in ``components/minihub_matter/matter_ctrl.cpp`` (``MATTER_SUB_MIN_INTERVAL`` =
5 s, ``MATTER_SUB_MAX_INTERVAL`` = 30 s) are **configured bounds** on how often the
device may report; they are not a measured button-press-to-card latency.

Limits
^^^^^^

Each cap below, with the line of code that sets it, is listed in :doc:`limits`.

* **Node and endpoint caps.** The subscription table is sized by
  ``CONFIG_MINIHUB_MATTER_MAX_NODES`` — default **128**, configurable 16–1024 — and
  the same symbol caps ``adapters.matter.devices[]``. A node past the cap is not
  accepted (``ESP_ERR_NO_MEM``, ``last_error`` says ``subscribe: table full``).
  The table size is not what you can control, though: when the Hub builds without
  its own Matter server (``CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER`` unset),
  ``matter_project_config.h`` sets ``CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES`` to
  **8**, and that is the ceiling on commissioned peers. The per-node endpoint list
  is 16, the same limit as one commissioning. Note the
  Hub-wide ``MINIHUB_MAX_POINTS`` = 64: every Matter **endpoint** is its own point,
  so 64 endpoints across all radios is the real ceiling on the point registry,
  well below what the Matter tables allow.
* **Boolean attributes only.** A report whose TLV does not decode as a bool is dropped.
  OnOff is the only attribute armed; level, color and cover position each need their own
  decode plus a per-cluster value mapping.
* **Re-read after each (re)arm.** ``minihub_matter_read_onoff()`` is called by the
  adapter right after every subscription arm, so a dropped subscription that clobbered
  the value self-heals rather than sticking at ``off``. It returns
  ``ESP_ERR_NOT_FINISHED`` and the value arrives through the same report callback.
  There is still no on-demand re-read API and no periodic poll beyond that
  minute-cadence re-arm: steady state is subscription-driven.
* **``online`` tracks that node's subscription.** The poll writes
  ``minihub_point_set_online(point, live)`` for a commissioned row, and a real report
  also flips it online, so a card reading online means that node's subscription is
  live (or reported recently). Only during the first window after start-up — before
  the poll's every-twelfth tick — is ``online`` still the commissioner-readiness
  value from registration, which is when ``live`` false with ``reports`` 0 can show.
  Check ``subscriptions`` before believing the card.
* **Sleepy devices (ICD) are not commissioned.** The controller explicitly disables
  ICD registration (``pairing.set_icd_registration(false)``), because esp-matter's
  default of registering an ICD fails commissioning outright. An ICD client is
  therefore expected to fail to join, not merely to under-report.
* **Multi-fabric** keeps the ``get_fabric_index()`` assumption inherited from the existing
  commissioning code.
* **No events.** ESP-Matter's subscribe takes an event-path list and an event-callback
  slot; MiniHub's subscription passes neither (zero event paths, a ``nullptr`` callback),
  so no Matter events are subscribed or delivered today.

Point identity and plugin extras
--------------------------------

Each commissioned Matter **endpoint** is **one** Host point (existing
``{transport}.matter_n{node}_ep{ep}`` — ``transport`` being ``thread``,
``wifi`` or ``ethernet`` — or the ``point_id`` in ``adapters.json``).
This round does **not** collapse endpoints into one point per node. After
register, Host reads EP0 Basic Information (cluster ``0x0028``, best-effort;
commission is not blocked on failure):

* ``props.node_id`` — decimal controller node id (like Zigbee ``ieee`` / Mesh
  ``uuid``)
* ``props.manufacturer`` / ``props.model`` — VendorName / ProductName
* ``props.sw_version`` — SoftwareVersionString, else decimal SoftwareVersion
* ``props.hw_version`` — HardwareVersionString or HardwareVersion
* ``props.category`` — **not** filled by Matter Host. UI falls back to the
  type-catalog label (``light`` → “灯”)

Identity uses ``minihub_point_merge_props`` only — **not** written into
``adapters.json``. Every endpoint of the same node gets the same Basic
Information.

Plugins match ``props.manufacturer`` on that ``point_id``:

* **Associate** — ``props.owner`` = plugin ``adapter`` name. Written by the
  plugin; no Host code sets it, and no cloud catalog is needed.
* **OTA flag** — the plugin merges ``props.ota`` when its own check says an image
  is ready. ``POST /api/devices/ota`` reads only ``file`` / ``url`` / ``version``
  from it.

Start OTA with adapter commands ``ota`` / ``start_ota`` (``path`` /
``file_path`` / ``url``), or Host ``minihub_matter_ota_offer``. The Hub is a
CSA **OTA Provider** (cluster ``0x0029``): it sends ``AnnounceOTAProvider`` to
the device **OTA Requestor** (``0x002A`` EP0), then **BDX** after QueryImage.
The image must be a Matter OTA file (CSA header ``0x1BEEF11E``). Local files
are served on loopback HTTP for the BDX downloader; ``http(s)`` URLs are
fetched directly. Requires ``CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER`` and
``CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED`` (on in ``matter.defaults``).
Matter-over-Thread end-devices are **Matter** points; see :doc:`thread`.

``.mhub`` packing needs ``MINIHUB_PLUGIN_DIRS``.

Who owns Wi-Fi?
---------------

CHIP Wi-Fi station is **off** on MiniHub controller builds
(``matter_over_eth.defaults``). MiniHub still connects Host STA via
``minihub_net_start_wifi()`` when credentials exist. Default-route selection only
picks the netif with IPv4 — see :doc:`networking`.

Terminology
-----------

Use **Matter controller** (ESP-Matter). **Commissioning** = pairing a device to the
fabric. See :doc:`faq`.
