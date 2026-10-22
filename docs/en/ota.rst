Hub firmware update
====================

:link_to_translation:`zh_CN:[中文]`

**Hub application** firmware update for operators. End-device OTA:
:doc:`radios` and :doc:`api/host_rest`. Dual app slots ``ota_0`` / ``ota_1`` and
factory partition planning: :doc:`production`. OEM product folders and slot
sizes: :doc:`host_customization`. REST overview: :doc:`api/host_rest`.

NVS factory reset (hold the configured GPIO) does **not** wipe LittleFS
``/data`` or ``/store``. Use a full ``flash`` when the jig needs a clean
filesystem. App OTA likewise does **not** replace the Web console on
``/data/web/``; see :doc:`web_console`.

Slots and image size
--------------------

The inactive slot is written, then the device reboots into it. The image must
fit the slot. Matter builds need large slots; if the linker says the app does
not fit, grow ``ota_0`` / ``ota_1`` (see :doc:`production` and
:doc:`host_customization`).

Pull from the Store
-------------------

Automatic update does not use ``POST /api/ota``. ``minihub_update_check()`` and
``minihub_update_apply()`` request ``GET /api/firmware?mac=&chip=&cur_version=``
on the deploy root. The response lists that chip only. Fields and the version
decision are in :doc:`api/deploy_index`. The device keeps the highest version
whose targets match the local chip, and when that version is higher than
``CONFIG_MINIHUB_VERSION`` it calls ``minihub_ota_start_url()`` on that item's
``url`` (with ``mac`` and ``cur_version`` attached). The download checks the
64-digit hex ``sha256``. ``firmware_upgrade`` on ``/api/catalog`` only reports
the highest firmware version published for the chip. This install path does not
read it.

URL OTA and upload below are the manual paths. URL OTA must use **HTTPS**
that the device can reach (uplink: :doc:`networking`). The Store pull uses
the Store address configured on the device.

GET status
----------

``GET /api/ota`` returns JSON:

* ``state`` — ``0`` idle, ``1`` in progress, ``2`` success, ``3`` failed
* ``error`` — last error string (empty when none)
* ``progress`` — 0–100, or ``-1`` if unknown
* ``bytes_written`` — bytes written to the update slot
* ``source`` — ``none``, ``url``, ``upload``, or ``rainmaker``

URL OTA
-------

``POST /api/ota`` with a JSON body. **HTTP 200** starts the job; **409** if
another OTA is busy.

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://..."}'

Success body: ``{"result":"started"}``. Poll ``GET /api/ota`` for progress.

Upload OTA
----------

``POST /api/ota/upload`` accepts the raw ``.bin`` body. Send
``Content-Length`` (required). This path is **not** on the MCP ``hub.api``
allowlist; use REST (or a LAN client), not ``hub.api``.

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

**HTTP 200** ``{"result":"ok"}`` after the image is written (the device reboots
shortly). **409** if busy. Missing ``Content-Length`` is **400**.

Boot and rollback
-----------------

The new slot boots with the image pending verify. ``minihub_ota_init`` then
calls ``esp_ota_mark_app_valid_cancel_rollback``. If that boot reaches OTA
init, the new app stays. If the new app never starts, the bootloader returns
to the previous slot. There is no REST call to accept or reject the image.
If neither slot boots, recover with a full ``flash`` over UART download.
