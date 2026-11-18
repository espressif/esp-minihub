Troubleshooting
===============

:link_to_translation:`zh_CN:[中文]`

Work top-down: the network first, then the Hub, then the feature. Each section is a
symptom, the check that separates the causes, and the fix. Log strings are quoted as
the firmware prints them, so you can grep the serial output for them.

The console and the REST API have no log-level switch. Verbosity is a build-time
ESP-IDF setting, and a few subsystems raise their own tags at start-up
(:file:`components/minihub_zigbee/zigbee_stack.c:2296`); the serial console is the
only live log channel, so rebuild with a lower threshold if a message you need is
compiled out.

The console page never loads
----------------------------

Work the ladder in order; each rung has its own evidence.

.. list-table::
   :header-rows: 1
   :widths: 26 40 34

   * - Check
     - Evidence
     - If it fails
   * - Link is up
     - ``eth got IP`` or ``wifi got IP`` on the serial log
     - No cable power/no PoE, or Wi-Fi never associated — see below
   * - Address is the one you are typing
     - The same log line prints it; the Hub registers **no hostname**
     - Use the printed address; a DHCP lease can move after a reboot
   * - The right uplink won
     - ``minihub_net_refresh_default_netif()``: Ethernet wins while it has IPv4
     - Both up links live and you are on the wrong subnet — the Ethernet address is
       the reachable one
   * - Console files exist
     - ``GET /web/index.html`` returns HTML; files live on the ``storage`` partition
     - ``storage-flash`` the console, or ``PUT /api/dev/web/`` on a running Hub
   * - Port 80 is the Hub
     - ``curl -s http://HUB/api/auth/status`` answers JSON
     - Another HTTP server owns the address, or the device is in provisioning SoftAP
   * - Session is alive
     - ``GET /api/auth/status``; a page redirects to ``/login`` when it is off
     - Idle sessions expire in 300 s and never outlive 8 h — log in again

Device is up but no Wi-Fi network appears
-----------------------------------------

* The serial log shows the provisioning path (an open access point named
  ``PROV_MiniHub``, channel 1) instead of ``wifi got IP`` → the stored credentials are
  empty. Join that AP and post the credentials to ``POST /api/net/wifi`` through the
  console it serves.
* Association retries but never completes → wrong password, hidden SSID, or a channel
  the SoC cannot use. ``esp_wifi`` reports the reason; read it from the log.
* Wi-Fi is configured but the address never appears → DHCP pool exhausted or a
  client-isolation AP. Confirm with ``GET /api/system`` that ``local_ip`` is the
  address you expect.
* BLE provisioning does not exist in this Host. Wi-Fi comes in through the web console
  or ``POST /api/net/wifi``.

A rule does not fire
--------------------

* A ``when`` match is edge-triggered: the rule fires when the point **enters** the hit
  state. A value that stays equal produces no ``point.updated``, so nothing to match —
  drive it off and on, or trigger it from the console.
* Repeated identical updates are collapsed by the debounce window
  (``CONFIG_MINIHUB_POINT_DEBOUNCE_MS``, default 1000 ms).
* ``if`` conditions are checked against live points and never trigger anything: a rule
  whose ``when`` fires while an ``if`` fails simply does not run.
* Time and ``once`` rules poll once per second but need a plausible clock — see
  :doc:`time`. A Hub that never reached an NTP server fires no schedule, and it fails
  silently: point-triggered rules still work.
* A reload was in progress: MCP answers ``HUB_NOT_READY``, and a queue of 16 action
  jobs drops what does not fit.
* The document never loaded: ``GET /api/rules`` returns what is live; the engine logs
  a rejected document. Import sizes are capped, and an over-cap document is refused as
  a whole.

A device card is missing or greyed
----------------------------------

* The point exists but is hidden: ``visible`` is decided by the instance
  (``devices.json`` → ``props.params``), and ``access: "R"`` renders a value with no
  control.
* The type or parameter is not in the catalog: the UI intersects the type catalog with
  the live point, so a parameter the catalog does not know renders nothing. Upload both
  CSVs if you extended the model (:doc:`type_catalog`).
* ``enabled: false`` is skipped only when it is explicitly false.
* The adapter registered the point under a different ``point_id`` than the rule or card
  expects: one device is one point, ``<protocol>.<device>``, with parameters inside
  ``props.params``.

Store refresh or install is stuck
---------------------------------

* ``POST /api/store/refresh`` while a refresh runs still answers **200** with a pending
  status: poll ``GET /api/store/refresh`` state instead of firing again.
* 400 / 404 / 409 on install each mean one thing (:doc:`faq`).
* An unreachable deploy server is slow by design: the probe runs off the HTTP task so
  the console keeps answering, which also means the failure is not visible until you
  read the hint status.
* ``GET /api/store/config`` shows the base URL actually in use; a trailing slash or a
  wrong ``chip`` filter empties the catalog without an error.

An update failed or the Hub came back on the old version
--------------------------------------------------------

* The image is staged on external LittleFS media, then Vibe Mode copies it into ``ota_0``.
  A checksum, SHA or size failure is reported during that copy; recovery is Vibe Mode
  copying the staged image again. If the app no longer fits the slot, grow ``ota_0`` in
  the product `partitions.csv <https://gitlab.espressif.cn/ae_group/minihub/esp-minihub/-/blob/master/application/minihub/partitions.csv>`__.
* The Hub is unreachable for a few seconds: it enters Vibe Mode 800 ms after staging,
  then restarts again after the copy, and the bootloader confirms the new firmware.
* The device stays in Vibe Mode → ``ota_0`` is not a valid image. Keep the media inserted
  and reboot so Vibe Mode retries, or read ``minihub/vibe_result`` and the serial log.
* The Web console did not change after a firmware update: firmware OTA never replaces
  ``/data/web/``; the console is a separate package (:doc:`web_console`).

A plugin does not load
----------------------

The boot log line names the reason; each one is documented in :doc:`faq`, including
``JS module 'X' missing main.js``, ``skip module 'X' — adapter 'Y' already registered``
and ``module table full``.

When you ask for help
---------------------

Attach these five, and the round trip is usually one message:

1. ``GET /api/system`` response (version, ``idf_version``, ``commit``, chip, memory).
2. The serial log from power-on to the failure, not just the last line.
3. The document or package involved: ``GET /api/rules``, ``GET /api/adapters``, or the
   ``module.json`` of the Store package.
4. What you did, in the order you did it, and the exact symptom.
5. Whether it reproduces after ``POST /api/system/reboot`` — a symptom that needs a
   reboot to reappear is a load-order or timing problem, and that changes where to look.

Related: :doc:`faq`, :doc:`networking`, :doc:`rules`, :doc:`ota`, :doc:`time`,
:doc:`limits`, :doc:`api/errors`.
