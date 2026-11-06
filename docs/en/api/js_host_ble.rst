JS plugin BLE host API
======================

:link_to_translation:`zh_CN:[中文]`

This namespace lives in ``components/minihub_js/minihub_js_ble.c`` over
``minihub_ble_gatt``. The table is read from the C bindings at build time.

.. minihub-JS-api:: ble

See :doc:`js_host` for the entry point.

Methods — host.ble
------------------

One shared, reference-counted passive advertisement scanner for all BLE vendor
plugins. Enable ``CONFIG_MINIHUB_JS_BLE`` (``menuconfig → JS plugins``); when off,
these methods throw ``host.ble disabled`` so ``setup(host)`` can degrade gracefully.

Implementation: ``components/minihub_js/minihub_js_ble.c`` over ``minihub_ble_gatt``.

host.ble.ensureScan()
~~~~~~~~~~~~~~~~~~~~~

Increments the scan user count and returns it. The first caller starts a
continuous scan (no UUID filter); later callers only take a reference. Call it
from ``setup(host)`` — do **not** start a scan per module, since restarting the
scanner clears the shared advertisement accumulator.

host.ble.releaseScan()
~~~~~~~~~~~~~~~~~~~~~~

Decrements the count and returns it. At zero the scanner stops. Optional: the
runtime releases references on module unload.

host.ble.getAds({ company?, uuid16?, max? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns an array of matching advertisements (at most ``max``, default 24).
``company`` filters ``mfgCompany``; ``uuid16`` matches either the advertised
UUID16 or the Service Data UUID16. Filtering happens in C, so a plugin only
pays for the objects it actually parses.

Each entry::

  { addr: "aabbccddeeff", addrType: 0, rssi: -62, name: "sensor",
    uuid16: 0xfdcd,                        // only when advertised
    mfgCompany: 0x0499,                    // only with Manufacturer Data
    mfgData: [0x99, 0x04, 0x05, ...],     // number[] of raw bytes
    svcUuid16: 0xfdcd,                     // only with Service Data
    svcData: [0xcd, 0xfd, ...] }           // number[] of raw bytes

``mfgData`` and ``svcData`` are small-integer arrays that **include the leading
two little-endian bytes** (company id / UUID16), exactly as the C Host API
delivers them. A C parser doing ``d = mfg_data + 2`` becomes
``var d = ad.mfgData.slice(2)`` in JS. There is no base64 layer.

Poll with ``host.adapter.poll`` rather than subscribing to an event: the event
queue is shallow and BLE advertisements arrive in bursts.
