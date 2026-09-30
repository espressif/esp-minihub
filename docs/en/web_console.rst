Replace the Web console
=========================

:link_to_translation:`zh_CN:[中文]`

OEM-skinnable Host console (HTML/CSS/JS). It is **not** compiled into the app
binary. The build copies files onto the ``storage`` LittleFS image; the HTTP
server reads them at runtime from ``/data/web/``.

Source and device path
-----------------------

In-tree source (generic Host):

.. code-block:: text

   components/minihub_web/web/
     index.html      GET /
     login.html      GET /login
     manifest.json
     …               other static files as GET /web/ plus the file name

When ``CONFIG_MINIHUB_WEB_UI`` is on, ``application/minihub/main/CMakeLists.txt``
minifies that directory into the ``storage`` image as ``/data/web/``.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Browser
     - File on device
   * - ``GET /``
     - ``/data/web/index.html``
   * - ``GET /login``
     - ``/data/web/login.html``
   * - ``GET /web/`` plus file name
     - ``/data/web/`` plus the same name

If ``index.html`` is missing under ``/data/web/``, the firmware tries the legacy
path ``/store/preinstall/web/`` (devices flashed before this migration), then a
small ROM page (Chinese or English from ``Accept-Language``). That ROM page is
**not** OEM-editable; restore ``index.html`` on ``storage``. If ``login.html``
is missing, ``GET /login`` returns 404 (UTF-8 plain text).

``GET /`` and ``GET /web/…`` may require a session cookie when Web login is
enabled; ``GET /login`` stays public so the browser can show the form.

Kconfig and login
-----------------

* ``CONFIG_MINIHUB_WEB_UI`` (default on) — serve the console. Off: ``GET /`` is
  404; REST ``/api/`` still works.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` (default on, depends on Web UI) — first
  visit to ``/login`` sets the ``admin`` password (NVS ``minihub/web_pw``).
  Cookie ``mh_sess``: **5 minutes idle** with no authenticated request
  (GET polls count as activity and refresh idle). Hard cap **8 hours** from
  login. On expiry APIs return 401 and the console navigates to ``/login`` so a
  cached SPA cannot keep uninstalling modules or controlling devices. Off: no
  cookie and no Bearer.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — optional ``Authorization: Bearer``
  for scripts. Empty string: cookie only. NVS fallback key remains
  ``api_token``.

A replacement ``login.html`` must keep talking to:

* ``GET /api/auth/status`` — ``login_auth``, ``password_set``, ``logged_in``
* ``POST /api/auth/setup`` — first password (JSON ``username`` / ``password``;
  username must be ``admin``, password 6–64 chars)
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` must keep using the same ``/api/`` routes (points, command,
device widgets, Store, …). Changing HTML does not add new firmware APIs.

The Devices page groups **one card per registered point** (not one card per
parameter). It loads ``GET /api/device_widgets`` plus ``GET /api/points`` and
draws CSS widgets (``device_widgets.css``). A type missing from the catalog
shows a **type not registered** badge. Writes go to
``POST /api/devices/control`` (``device`` is the type; UI also sends the card's
``point_id`` so same-type instances stay distinct). Type-catalog CSVs can
be updated without a firmware rebuild: ``PUT /api/dev/catalog/…`` or
``python3 tools/dev_sync.py --open``.

``PUT /api/dev/web/`` is authenticated when login is on. Typical order: open
``/login`` once in a browser (or send Bearer), then curl with ``Cookie`` or
``Authorization``. JSON examples: :doc:`api/host_rest`.

Replace at build (factory / lab)
-----------------------------------

1. Edit files under ``components/minihub_web/web/`` (or copy a skin over that
   directory before build).
2. Rebuild so CMake restages ``storage``.
3. Flash the **storage partition**, not only the app:

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub build
      ./tools/build_firmware.py -app minihub -p PORT storage-flash

``app-flash`` keeps the existing ``/data``. **Field OTA** writes ``ota_0`` /
``ota_1`` only; it does **not** update ``/data/web/``. After a lab
``app-flash`` or field OTA, the device still serves the old HTML until
``storage-flash`` or a full factory ``flash``. Merge image: :doc:`production`.
OTA: :doc:`ota`.

The generic Host copies from ``components/minihub_web/web/`` only. There is no
``application/APP/web/`` overlay unless you change that product
``CMakeLists.txt``.

Replace on a running Hub (no reflash)
-------------------------------------

``PUT /api/dev/web/`` followed by a relative path writes the body under
``/data/web/`` (max 512 KiB). The relative path must not contain ``..``.

.. code-block:: bash

   # Console (hard-refresh the browser after)
   curl -X PUT http://HUB/api/dev/web/index.html \
     --data-binary @components/minihub_web/web/index.html

   # Login page
   curl -X PUT http://HUB/api/dev/web/login.html \
     --data-binary @components/minihub_web/web/login.html

If REST returns **401**, send the same ``Authorization: Bearer`` or ``mh_sess``
cookie as other Host APIs (:doc:`api/host_rest`).

Success JSON includes ``path`` (device VFS path) and ``bytes``. Nested names
such as ``css/app.css`` create parent directories. Reload ``http://HUB/`` or
``http://HUB/login`` (disable cache).

This is **not** ``PUT /api/dev/fs/`` (LCD JSON under ``/data/ui/``, gated by
``CONFIG_MINIHUB_DEV_FS_UPLOAD``).

Language layout
---------------

The console is **one** ``index.html`` / ``login.html`` plus one string file per
language (``web/i18n/zh_CN.json``, ``web/i18n/en.json``). The language is chosen
in the browser, not by the server:

``localStorage['minihub.lang']`` → browser language (``zh*`` → ``zh_CN``,
otherwise ``en``) → ``en``.

A language the user picked therefore wins over the browser default on the next
visit. ``localStorage`` is per origin, so each Hub remembers its own choice.
Switching writes the key and reloads the page; the catalog is refetched with
``?lang=``, so device and parameter names follow the same choice.

The page is hidden until its string file has loaded, so an English user never
sees a Chinese first frame. ``/web/i18n/`` and ``/web/js/i18n.js`` are the two
paths served **without** a session, because the login page is the first thing a
user sees and must translate itself; they carry UI strings only.

This is why the HTTP server does **not** map ``Accept-Language`` onto
``web/en/`` versus ``web/zh_CN/`` folders — there are no per-language page
trees to pick between. Only the ROM fallback page (before any web assets are
mounted) still selects zh/en from ``Accept-Language``.

A Store module's own ``web/`` UI is not covered by this mechanism yet.

Module UIs
----------

A Store module may ship ``web/`` under ``/store/modules/ID/web/`` (served as
``/modules/ID/web/…``). That is separate from the Host console. Plugin UI:
:doc:`plugins/develop_l2`.
