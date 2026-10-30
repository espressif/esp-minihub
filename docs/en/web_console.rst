Replace the Web console
=========================

:link_to_translation:`zh_CN:[中文]`

The console is ordinary HTML, CSS and JavaScript, so an OEM can replace it
without touching firmware source. It is not compiled into the app binary: the
build copies the files onto the ``storage`` LittleFS image, and the HTTP server
reads them at run time from ``/data/web/``. A re-skin therefore restages
``storage``, not the app. Flashing only ``app-flash`` or pushing a field OTA
leaves the old console in place. Two routes do write the console onto a running
Hub: the dev upload route further down, and the deploy-root update path
(``POST /api/update/apply`` with ``"type":"web"``, which pulls the published
``/api/web`` index) — see :doc:`api/host_rest`.

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

To bundle a custom UI instead of the in-tree console, set
``MINIHUB_CUSTOM_WEB_SRC`` (``menuconfig`` → MiniHub → Web UI) to a repo-relative
path or an absolute path; leave it empty for the default. The path must contain
an ``index.html`` or the build fails. The same :file:`tools/minify_web_assets.py`
step stamps the bundle version from ``CONFIG_MINIHUB_WEB_VERSION``, so a custom
skin still inherits the single source of truth for the web version.

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

For any ``/web/`` file missing under ``/data/web/``, the firmware tries the
legacy root ``/store/preinstall/web/`` (devices flashed before this migration)
before giving up. ``GET /`` falls back again to a small ROM page (Chinese or
English from ``Accept-Language``); that ROM page is **not** OEM-editable, so
restore ``index.html`` on ``storage``. If ``login.html`` is missing, ``GET /login``
returns 404 (UTF-8 plain text).

Only ``GET /`` checks the session cookie; ``GET /login`` stays public so the
browser can show the form. The whole ``/web/`` asset tree (HTML, JS, CSS, i18n
JSON) and ``/img/`` icons are served **without** a session on purpose: they carry
no credentials or device data, and gating them would let an expiring session
serve the login page into a ``<script>``/``<link>`` tag mid-load. The ``/api/``
endpoints are the real gate.

Kconfig and login
-----------------

* ``CONFIG_MINIHUB_WEB_UI`` (default on) — serve the console. Off: ``GET /`` is
  404; REST ``/api/`` still works.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` (default on, depends on Web UI) — first
  visit to ``/login`` sets the ``admin`` password (NVS ``minihub/web_pw``).
  Cookie ``mh_sess``: **5 minutes idle**. Only a document navigation
  (``GET /``, a page reload) slides that window — ``minihub_auth_check`` is
  read-only, so the console's background ``fetch`` polling does **not** keep the
  session alive and an auto-refreshing tab still signs out. Hard cap **8 hours**
  from login, never extended. On expiry APIs return 401 and the console
  navigates to ``/login`` so a cached SPA cannot keep uninstalling modules or
  controlling devices. Off: no cookie and no Bearer.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — optional ``Authorization: Bearer``
  for scripts. ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` defaults to ``espressif``
  for every product, so a fresh build already answers to that token; it is a
  shared development default, not a per-device secret, so set your own value per
  product. Resolution is one-shot at boot and the compiled string wins: with it
  non-empty the NVS key ``minihub/api_token`` is **not** read at all, so setting
  it there does nothing until the Kconfig value is emptied — which is also how
  you get a cookie-only build.

A replacement ``login.html`` must keep talking to:

* ``GET /api/auth/status`` — ``login_auth``, ``password_set``, ``logged_in``
* ``POST /api/auth/setup`` — first password (JSON ``username`` / ``password``;
  username must be ``admin``, password 6–64 chars)
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` must keep using the same ``/api/`` routes (points, command,
device widgets, Store, …). Changing HTML does not add new firmware APIs.

The Overview page groups **one card per registered point** (not one card per
parameter). It loads ``GET /api/device_widgets`` plus ``GET /api/points`` and
draws CSS widgets (``device_widgets.css``). A type missing from the catalog still
draws its parameter rows in the device detail; the browser console logs the miss
(:doc:`type_catalog`). Writes go to
``POST /api/devices/control`` (``device`` is the type; UI also sends the card's
``point_id`` so same-type instances stay distinct). Type-catalog CSVs can
be updated without a firmware rebuild via REST (``PUT /api/dev/catalog/…``) or
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

The Web console can also be updated from the UI: with a custom plugin server
configured, open **System → Check for updates** and apply the web update -- the
Hub pulls the new bundle (``POST /api/update/apply`` with ``"type":"web"``). The
``PUT /api/dev/web/`` route below is the developer/manual alternative for lab
work:

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

.. warning::

   Each PUT replaces **one file**. Nothing about this route knows that ``index.html`` and
   ``js/app.js`` belong to the same build, so a partial push leaves a console made of two
   generations — usually one that loads and then fails in the browser console, because the
   HTML and the JS no longer agree. Push every file you changed, or reflash the storage
   partition (``storage-flash``) to get back to the console that shipped with the image:
   ``app-flash`` writes only the app partitions and leaves ``/data/web`` as it is, and a
   factory reset erases the whole storage partition including these files.

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
sees a Chinese first frame. That works while signed out because the whole
``/web/`` tree — including ``/web/i18n/`` and ``/web/js/i18n.js`` — is served
without a session; those files carry UI strings only.

The HTTP server does **not** map ``Accept-Language`` onto ``web/en/`` versus
``web/zh_CN/`` folders — there are no per-language page trees to pick between.
``Accept-Language`` does still choose the language of two things that are not
pages: the ROM fallback page, and the human-readable strings the API puts in
error bodies and in the missing-``login.html`` 404.

A Store module's own ``web/`` UI is not covered by this mechanism yet.

Module UIs
----------

A Store module may ship ``web/`` under ``/store/modules/ID/web/`` (served as
``/modules/ID/web/…``). That is separate from the Host console. Plugin UI:
:doc:`plugins/develop_l2`.
