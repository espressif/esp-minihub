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

The Hub always runs from ``ota_0``. There is one other app partition,
``vibe_mode`` (``app,factory``), which holds a retained maintenance image. That
image is the only code allowed to write an app slot.

1. URL, upload and deploy-server updates download the bare OTA ``.bin`` to
   ``/Firmware/firmware.bin`` on the external LittleFS volume. The running
   firmware does not write flash.
2. The device records which volume was used in NVS (``minihub/vibe_dev``),
   hashes the staged file, writes ``firmware.bin.sha256`` beside it, stores the
   SHA in ``minihub/vibe_sha``, arms ``minihub/vibe_pending`` once, then selects
   ``vibe_mode``. ESP-IDF handles an ``app,factory`` target by erasing
   ``otadata``, so the next boot starts Vibe Mode.
3. Vibe Mode copies only when ``vibe_pending`` is set, or when
   ``CONFIG_MINIHUB_VIBE_FORCE_GPIO`` is held (``-1`` disables the button). Before
   any flash write it reads the staged ``esp_app_desc`` and compares it with
   ``ota_0``: a differing ``project_name`` is rejected (for example ``minihub``
   must not replace ``minihub_lcd``; the check is skipped if ``ota_0`` has no
   valid descriptor); equal ``app_elf_sha256`` values skip the copy. When a
   copy is required, the file SHA is recomputed and must match NVS (or the
   sidecar for force-GPIO recovery).
4. Otherwise Vibe Mode writes the image with ``esp_ota_begin()`` /
   ``esp_ota_write()`` / ``esp_ota_end()`` into ``ota_0``. ``esp_ota_end()``
   verifies the image. An incomplete write leaves an invalid slot and the next
   run copies again.
5. Vibe Mode rewrites ``otadata`` to select ``ota_0`` and restarts. It does this
   even when no image is present, the run was not armed, or the copy fails.
   Without that step the bootloader would keep selecting the factory app on
   every boot.

External media
--------------

The SDMMC card or USB mass-storage device carries a LittleFS volume, mounted at
``/vibe``. It is not FAT, and a PC does not mount it as a normal drive; the Hub
writes the image there itself during a download. ``CONFIG_MINIHUB_VIBE_*``
selects which devices are probed and, for SDMMC, the bus width and GPIOs.
``minihub/vibe_dev`` holds ``sdmmc`` or ``usb`` after the first successful
mount. Vibe Mode falls back to probing both devices when that key is absent.

``minihub/vibe_result`` holds the last hand-off outcome (``copied``,
``unchanged``, ``copy failed``, ``no firmware``, ``no media``). A failed copy is
retried on the next boot up to three times; after that Vibe Mode stays running
and waits for the media to be replaced.

The new image must fit in ``ota_0``. An oversized image is rejected before any
flash is erased (:doc:`production`).

Pull from the OTA server
-------------------------

Triggered manually in the Web UI: on the ``System`` page, the Hub queries the
configured OTA server (deploy root) for an update matching its chip; when one is
available, the firmware version row shows a "new version vX" badge, and clicking
``Update firmware`` downloads and stages that image. This path does **not** go
through the plugin Store, nor does it use the manual URL OTA
(``POST /api/ota``).

Web UI steps:

1. Open ``http://HUB/`` and go to the ``System`` page.
2. If the firmware version row shows a "new version vX" badge, an update for the
   device's chip is available on the OTA server; click the badge for release notes,
   then click ``Update firmware`` to start the download.
3. Progress shows in the OTA status bar. When staging finishes the device
   restarts into Vibe Mode, then restarts again into the updated ``ota_0`` and
   the version updates.

The Hub pulls only images matching the current chip and picks the highest version
above ``CONFIG_MINIHUB_VERSION``. The download is **not** digest-verified: the
deploy index's ``sha256`` is only recorded so a same-version rebuild can be
detected and re-fetched. Vibe Mode validates the image with ``esp_ota_end()``
on copy, and skips the copy when staged and ``ota_0`` share the same
``app_elf_sha256``. Console and asset downloads verify ``sha256`` before
replacing a file. The deploy index fields and version-selection
logic are in :doc:`api/deploy_index`. This path does not read the
``firmware_upgrade`` field of ``/api/catalog`` (that field only reports the
highest published firmware for the chip).

A fetch runs once: there is no resume and no retry, and a non-200 HTTP answer or a
stalled read is reported as a failure.

URL OTA and upload OTA below are the manual paths; you can also trigger them from
the ``System`` page in the Web UI by entering a firmware URL or choosing a ``.bin``
file. The device does not police the URL scheme: plain ``http`` works on a lab LAN.
For anything a third party can reach, use HTTPS — certificate validation needs
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
* ``bytes_written`` — bytes staged on the external volume
* ``source`` — ``none``, ``url``, ``upload``, or ``rainmaker``

``2`` means the image was staged and the hand-off to Vibe Mode started. The device
restarts after approximately 800 ms, so clients often observe ``state`` return to
``0`` only on the updated firmware. Poll ``GET /api/system`` for the running
version.

URL OTA
-------

Web UI: on the ``System`` page, enter a firmware URL in the firmware-update area
and click ``Start update``.

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
  back as **400** ``invalid JSON``.
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

Web UI: on the ``System`` page, choose a ``.bin`` file in the firmware-update
area and click ``Upload``; the device restarts after staging. This path is not on
the MCP ``hub.api`` allowlist.

REST alternative (auxiliary): ``POST /api/ota/upload`` accepts the raw ``.bin``
body. Send ``Content-Length`` (required).

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

**HTTP 200** ``{"result":"ok"}`` after the image is staged (the device restarts
shortly). **409** if busy. Missing ``Content-Length`` is **400**.

Boot and recovery
-----------------

There is no A/B application slot pair and no ESP-IDF OTA rollback: both app
slots would be needed for that, and ``ota_0`` is the only running slot. Recovery
is Vibe Mode instead. A power cut during the copy leaves ``ota_0`` without a
valid image; the bootloader falls back to the factory app, and Vibe Mode copies
the staged image again.

``CONFIG_BOOTLOADER_APP_ROLLBACK`` with confirm-on-startup is enabled for the
single slot, and ``minihub_ota_init()`` confirms the app on the first boot after
a hand-off. There is no application-level self-test and no REST call to accept
or reject firmware: once the new firmware reaches ``app_main`` it is kept. To go
back, stage a known-good build with the upload or URL path, or re-flash over UART
download (:doc:`production`).

Cloud OTA through RainMaker is rejected: its default callback writes the
inactive OTA slot, which no longer exists. Use the URL or upload path, or the
deploy-server pull path.
