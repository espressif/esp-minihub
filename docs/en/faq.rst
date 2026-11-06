FAQs
====

:link_to_translation:`zh_CN:[中文]`

Where do I put a vendor cloud integration?
------------------------------------------

As a Store plugin (``.mhub``), not in Host Core. See :doc:`plugins/overview`.

How do I add my own Host product?
---------------------------------

See :doc:`application/index` then :doc:`host_customization`. Copy ``application/minihub`` and build with
``-app APP`` (or ``MINIHUB_APP``). ``build_firmware.py`` already syncs the
manifest; you do not need to run ``sync_host_manifest.py`` yourself.

A Store plugin installs but nothing appears
-------------------------------------------

Install success is not load success. Read the boot log for the module id; each of
these lines in ``components/minihub_module/module_loader.c`` is its own answer:

* ``JS module 'X' missing main.js`` (:file:`module_loader.c:901`) — the package has
  no ``main.js``; check ``module.json`` ``runtime`` is the literal ``JS``.
* ``module 'X' has no binary or builtin`` (:file:`module_loader.c:906`) — nothing for
  the Host to enter.
* ``skip module 'X' — adapter 'Y' already registered`` (:file:`module_loader.c:912`) —
  a bundled adapter owns that ``adapter`` name.
* ``module table full, skip …`` (:file:`module_loader.c:549`) or
  ``duplicate module id`` (:file:`module_loader.c:559`) — capacity or a repeated id.
* ``invalid module manifest`` (:file:`module_loader.c:599`) — ``module.json`` did not
  parse; validate before packing.
* No ``setup`` in ``main.js`` — the module is skipped with a warning
  (:doc:`api/js_host`).

Then check the build: JS modules need both ``CONFIG_MINIHUB_JS_PLUGINS`` and
``CONFIG_MINIHUB_DYNAMIC_MODULES``. Full ladder: :doc:`troubleshooting`.

Store install returns 400 / 404 / 409
-------------------------------------

* **400** — missing ``type`` / ``id``. ``type`` must be ``module`` or ``pack``
  (not ``adapter``). Multipart ``file=@*.mhub`` is **not** supported.
* **404** — catalog entry or artifact missing; the body says ``ESP_ERR_NOT_FOUND``, not
  the reason, so set ``custom_url`` then ``POST /api/store/refresh``
* **409** — already installed; use ``POST /api/store/upgrade`` or uninstall first
* Check ``GET /api/store/config`` for Store base URL

Every status and body this endpoint can produce: :doc:`api/errors`.

.. only:: not hide_l3

   Module fails to load (load_guard)
   ---------------------------------

   If a native module crashes during ``dlopen``, Host writes
   ``/store/modules/ID/.load_guard`` and skips it on the next boot. Clear with
   ``POST /api/modules``:

   .. code-block:: JSON

      { "id": "my_native", "clear_load_guard": true, "reload": true }

   Check the serial log for ABI errors. ELF ``module.json`` ``abi_version`` and
   ``core_api_*`` must bracket the Host (header ``MINIHUB_MODULE_ABI_VERSION``).
   See :doc:`api/module_abi`.

Hub reload loops or MCP says HUB_NOT_READY
------------------------------------------

MCP ``hub.reload`` is rate-limited to once per 5 s; REST ``POST /api/hub/reload``
is not. During reload, MCP may return ``HUB_NOT_READY``. Matter + Thread builds
may take longer while CHIP re-attaches.

REST returns 401 unauthorized
-----------------------------

When ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on (default), REST needs a valid
``mh_sess`` cookie or ``Authorization: Bearer``. Set the optional token with
``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` (empty by default, so nothing is shared
between units), which wins when non-empty; the NVS key ``api_token`` (namespace
``minihub``) is read only if that compiled string is empty.
Turn the option off to leave LAN REST open. Login HTML: :doc:`web_console`.

Where is adapters.json?
-----------------------

Runtime copy: ``/data/config/adapters.json`` on device. Factory JSON:
``application/minihub/configs/adapters.json`` or product-specific tree. This file
is runtime config in ``/data/config`` and is NOT covered by the online update
(only firmware, the web console, and device assets are); it ships in the
factory/storage image, or edit via REST: ``PUT /api/config/adapters``, then
``POST /api/hub/reload`` if needed.
Full JSON schemas: :doc:`data_model`.

How do I add a device type or change widgets?
---------------------------------------------

Edit Host ``components/minihub_device_widget/model/*.csv`` (closed
``ui_element`` set) and rebuild, **or** upload both CSVs on a running Hub
(``PUT /api/dev/catalog/devices.csv`` and ``parameters.csv``; REST only -- the Web console has no page for this).
Adapters still register **one point per device** and fill ``props.params``.
Do not add a C struct per type. Details: :doc:`type_catalog`.

How do I replace the Web console?
----------------------------------

Edit ``components/minihub_web/web/`` then ``storage-flash``, or update from the
UI with a custom plugin server (``System`` > ``Check for updates`` > apply web); the
developer REST route ``PUT /api/dev/web/`` on a running Hub is the manual
alternative. ``app-flash`` does not update
``/data/web``. Details: :doc:`web_console`.

How do I flash a factory image / run OTA?
-----------------------------------------

Factory image: :doc:`production` (``flash`` or ``build merge`` for jigs;
``app-flash`` keeps ``/data`` and ``/store``). Field OTA: :doc:`ota`.

Where should I start for open-source secondary development?
-----------------------------------------------------------

The sidebar is the full outline. Read :doc:`introduction` and :doc:`architecture`
first, then :doc:`vibe_coding` if you use an AI assistant. Open the chapter that
matches your deliverable (OEM Host, Store plugin, or API integration). Contribution
policy: :doc:`contributing`. Build HTML via ``docs/README.md``.
