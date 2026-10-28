Hub firmware update
====================

:link_to_translation:`zh_CN:[中文]`

These steps update the **Hub application** itself. Updating an end device is a
different path (:doc:`radios`, :doc:`api/host_rest`). Slot planning belongs to
:doc:`production`; product folders and slot sizes to :doc:`host_customization`.

App OTA does not replace the Web console under ``/data/web/``; update the console
as a separate step (:doc:`web_console`). For factory reset and full ``flash``, see
:doc:`production`.

How it works
------------

The dual-slot write, reboot switch, and rollback are implemented by ESP-IDF; the
mechanism is in the `ESP-IDF OTA Upgrade guide
<https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/system/ota.html>`_.
On minihub you trigger the update and watch it; the rest is ESP-IDF's.

.. image:: ../_static/ota_update/update_flow.svg
   :align: center
   :alt: MiniHub firmware update: cached hints → background probe → start the download → write the free slot → validate → switch slot, reboot, confirm

`Interactive version <_static/ota_update/index.html>`_ — the cards tour the six
steps on their own (hover pauses); the cyan return line is the rollback path.

The new image must fit the slot. Matter builds need larger slots; when the
linker reports the app does not fit, grow ``ota_0`` / ``ota_1`` (see
:doc:`production` and :doc:`host_customization`).

Pull from the OTA server
-------------------------

Triggered manually in the Web UI: on the **System** page, the Hub queries the
configured **OTA server (deploy root)** for an update matching its chip; when one
is available, the firmware version row shows a "new version vX" badge, and clicking
**Update firmware** pulls the firmware from that server and flashes it. This path
does **not** go through the plugin Store, nor does it use the manual URL OTA
(``POST /api/ota``).

Web UI steps:

1. Open ``http://HUB/`` and go to the **System** page.
2. If the firmware version row shows a "new version vX" badge, an update for the
   device's chip is available on the OTA server; click the badge for release notes,
   then click **Update firmware** to start the download and flash.
3. Progress shows in the OTA status bar; the device reboots automatically when done,
   and the version updates.

The Hub pulls only images matching the current chip and picks the highest version
above ``CONFIG_MINIHUB_VERSION``. The downloaded firmware is **not**
digest-verified: the image is checked by ESP-IDF when the write finishes
(``esp_ota_end`` validates the header and sequence checksum), and the deploy
index's ``sha256`` is only recorded so a same-version rebuild can be detected and
re-flashed. Console and asset downloads are the ones that verify ``sha256``. The
deploy index fields and version-selection logic are in
:doc:`api/deploy_index`. This path does not read the ``firmware_upgrade`` field of
``/api/catalog`` (that field only reports the highest published firmware for the
chip).

A fetch runs once: there is no resume and no retry, and a non-200 HTTP answer or a
stalled read is reported as a failure. The write target is the inactive app slot —
5 MiB in the shipped 16 MB layout, so an oversized image fails mid-write rather
than up front (:doc:`production`).

URL OTA and upload OTA below are the manual paths; you can also trigger them from
the **System** page in the Web UI by entering a firmware URL or choosing a ``.bin``
file. The device does not police the URL scheme: plain ``http`` works on a lab LAN.
For anything a third party can reach, use **HTTPS** — certificate validation needs
the trust-store bundle (``CONFIG_MBEDTLS_CERTIFICATE_BUNDLE``), which is on in the
default build (uplink: :doc:`networking`).

GET status
----------

The Web UI OTA status bar already shows progress live; REST clients can poll
``GET /api/ota``.

``GET /api/ota`` returns JSON:

* ``state`` — ``0`` idle, ``1`` in progress, ``2`` success, ``3`` failed (a
  number, not a string)
* ``error`` — last error string (empty when none), kept to 95 characters
* ``progress`` — 0–100, or ``-1`` if unknown
* ``bytes_written`` — bytes written to the update slot
* ``source`` — ``none``, ``url``, ``upload``, or ``rainmaker``

``2`` is easy to miss: after a successful write the device restarts about 800 ms
later, so a poller usually sees ``1`` and then ``0`` on the new firmware. Poll
``GET /api/system`` (or the version in ``GET /api/ota`` status after reconnect) to
confirm which slot came up.

URL OTA
-------

Web UI: on the **System** page, enter a firmware URL in the firmware-update area
and click **Start update**.

REST alternative (auxiliary): ``POST /api/ota`` with a JSON body. **HTTP 200**
starts the job; **409** (plain-text body ``busy``) if another OTA is already
running.

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://..."}'

Success body: ``{"result":"started"}``. Poll ``GET /api/ota`` for progress.

Two things about the URL you pass in:

* The body is read into a 256-byte buffer, so the JSON, quotes and key included,
  leaves roughly 245 characters for the URL. A longer body is truncated and comes
  back as **400** ``invalid json``.
* The device appends ``mac=``, ``chip=``, and ``cur_version=`` to the URL before
  fetching. That is handy for per-device gating, but it changes the query string —
  a pre-signed or expiring CDN URL will no longer match its signature. Serve such
  URLs from a path that ignores extra query parameters, or use the deploy-server
  pull path.

Upload OTA
----------

This is the direct-to-device push path: connect straight to the device (for
example, open ``http://HUB/`` on its LAN) and push the firmware ``.bin`` to it
directly, with no OTA server in between.

Web UI: on the **System** page, choose a ``.bin`` file in the firmware-update
area and click **Upload**; the device reboots after the write. This path is not on
the MCP ``hub.api`` allowlist, so use the Web UI or a LAN client, not ``hub.api``.

REST alternative (auxiliary): ``POST /api/ota/upload`` accepts the raw ``.bin``
body. Send ``Content-Length`` (required).

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

**HTTP 200** ``{"result":"ok"}`` after the image is written (the device reboots
shortly). **409** if busy. Missing ``Content-Length`` is **400**.

Boot and rollback
-----------------

The shipped build enables ``CONFIG_BOOTLOADER_APP_ROLLBACK`` with
**confirm-on-startup**: ESP-IDF marks the new firmware valid automatically, just
before ``app_main`` runs. Rollback therefore only covers firmware that will not
start at all — the bootloader falls back to the other slot when the new one is
still pending verification.

There is no application-level self-test and no REST call to accept or reject
firmware: once the new firmware reaches ``app_main`` it is kept, even if the Hub
misbehaves afterwards. To go back, push a known-good build with the upload or URL
path above (the app-slot write does not compare versions), or re-flash over UART
download (:doc:`production`).
