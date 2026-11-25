Replace the Web console
=========================

:link_to_translation:`zh_CN:[中文]`

The console is HTML, CSS, and JavaScript. It is not linked into the app binary:
the build stages it on the ``storage`` LittleFS image, and HTTP serves
``/data/web/`` at run time. Changing the style updates ``storage``;
``app-flash`` or firmware OTA alone does not replace the console. Web has its
own OTA: ``POST /api/update/apply`` with ``"type":"web"`` (pulls ``/api/web``).
Dev writes use ``PUT /api/dev/web/`` below. See :doc:`api/host_rest`.

Source and device path
-----------------------

In-tree source (generic Host):

.. code-block:: text

   components/minihub_web/web/
     index.html      GET /
     login.html      GET /login
     manifest.json
     …               other static files as GET /web/ plus the file name

With ``CONFIG_MINIHUB_WEB_UI``, ``application/minihub/main/CMakeLists.txt``
minifies that tree into ``storage`` as ``/data/web/``.

Custom UI: set ``MINIHUB_CUSTOM_WEB_SRC`` (``menuconfig`` → MiniHub → Web UI) to
a repo-relative or absolute path; leave empty for the default. The path must
contain ``index.html`` or the build fails. :file:`tools/minify_web_assets.py`
stamps ``CONFIG_MINIHUB_WEB_VERSION``.

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

If a file is missing under ``/data/web/``, the firmware tries
``/store/preinstall/web/``. ``GET /`` then falls back to a ROM page (language from
``Accept-Language``); that page is not OEM-editable — restore ``index.html`` on
``storage``. Missing ``login.html``: ``GET /login`` returns 404 (UTF-8 plain text).

``GET /`` checks the session cookie; ``GET /login`` is public. ``/web/`` and
``/img/`` need no session; ``/api/`` is the gate.

Kconfig and login
-----------------

* ``CONFIG_MINIHUB_WEB_UI`` (default on) — serve the console. Off: ``GET /`` is
  404; REST ``/api/`` still works.
* ``CONFIG_MINIHUB_WEB_ASSET_GZIP`` / ``_PLAIN`` / ``_BOTH`` (default gzip) —
  how JS/CSS/HTML/JSON are stored under ``/data/web/``:

  * **gzip** — compressible files as ``.gz`` to save flash; clients must send
    ``Accept-Encoding: gzip`` or the Hub returns 406.
  * **plain** — plain files only, for clients that omit gzip.
  * **both** — plain and ``.gz`` together.

  ``login.html``, ``version.txt``, ``manifest.json``, and ``release_notes.txt``
  stay plain. The store keeps ``Webs/<ver>/plain/`` and ``…/gzip/``. CI emits
  ``minihub-web-*-plain.zip`` / ``*-gzip.zip`` (no ``.stamp``). OTA strips those
  prefixes into ``/data/web/`` and syncs only the tree that matches this choice.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` (default on, depends on Web UI) — first
  ``/login`` sets the ``admin`` password (NVS ``minihub/web_pw``). Cookie
  ``mh_sess``: **5 minutes** idle (only document navigations such as ``GET /``
  renew it; background ``fetch`` does not); hard cap **8 hours** from login.
  On expiry APIs return 401 and the console goes to ``/login``. Off: no cookie
  and no Bearer.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — optional ``Authorization: Bearer``;
  default ``espressif`` (shared development default — change per product).
  Resolved once at boot: a non-empty Kconfig value ignores NVS
  ``minihub/api_token``; clear it for cookie-only.

A replacement ``login.html`` must call:

* ``GET /api/auth/status`` — ``login_auth``, ``password_set``, ``logged_in``
* ``POST /api/auth/setup`` — first password (JSON ``username`` / ``password``;
  username ``admin``, password 6–64 chars)
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` uses the same ``/api/`` set (points, command, device widgets,
Store, …). HTML-only changes do not add firmware APIs.

Overview shows **one card per registered point**. It loads
``GET /api/device_widgets`` and ``GET /api/points`` with ``device_widgets.css``.
A type missing from the catalog still draws detail rows; the browser console
logs the miss (:doc:`type_catalog`). Writes use ``POST /api/devices/control``
(``device`` is the type, plus ``point_id``). Update type-catalog CSVs with
``PUT /api/dev/catalog/…`` or ``python3 tools/dev_sync.py --open`` without a
firmware rebuild.

With login on, ``PUT /api/dev/web/`` requires auth: open ``/login`` or send
Bearer, then curl with ``Cookie`` or ``Authorization``. JSON:
:doc:`api/host_rest`.

Replace at build (factory / lab)
-----------------------------------

1. Edit ``components/minihub_web/web/`` (or overlay that directory before build).
2. Rebuild so CMake restages ``storage``.
3. Flash the **storage** partition:

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub build
      ./tools/build_firmware.py -app minihub -p PORT storage-flash

``app-flash`` keeps existing ``/data``. Field firmware OTA writes ``ota_0`` only
and does not update ``/data/web/``. Merge image: :doc:`production`. OTA:
:doc:`ota`.

The generic Host copies from ``components/minihub_web/web/`` only. There is no
``application/APP/web/`` overlay unless that product ``CMakeLists.txt`` adds one.

Replace on a running Hub (no reflash)
-------------------------------------

UI path: configure a plugin server, then ``System`` > ``Check for updates`` >
web update (``POST /api/update/apply`` with ``"type":"web"``).

Dev path: ``PUT /api/dev/web/<relative path>`` writes the body under
``/data/web/`` (max 512 KiB; no ``..``).

.. code-block:: bash

   curl -X PUT http://HUB/api/dev/web/index.html \
     --data-binary @components/minihub_web/web/index.html

   curl -X PUT http://HUB/api/dev/web/login.html \
     --data-binary @components/minihub_web/web/login.html

On **401**, send ``Authorization: Bearer`` or ``mh_sess``
(:doc:`api/host_rest`).

Success JSON includes ``path`` and ``bytes``. Nested names such as
``css/app.css`` create parent directories. Reload ``http://HUB/`` or
``http://HUB/login`` with cache disabled.

This is not ``PUT /api/dev/fs/`` (LCD ``/data/ui/``, gated by
``CONFIG_MINIHUB_DEV_FS_UPLOAD``).

.. warning::

   Each PUT replaces one file. A partial upload can mix HTML and JS from
   different builds. Upload every changed file, or ``storage-flash`` to restore
   the image console. ``app-flash`` leaves ``/data/web`` unchanged; factory reset
   erases the whole storage partition.

Language layout
---------------

One ``index.html`` / ``login.html``, plus ``web/i18n/zh_CN.json`` and
``web/i18n/en.json``. Language is chosen in the browser:

``localStorage['minihub.lang']`` → browser language (``zh*`` → ``zh_CN``, else
``en``) → ``en``.

A manual choice overrides the browser default; storage is per origin. Switching
writes the key and reloads; catalog requests use ``?lang=``. The page stays
hidden until strings load.

``Accept-Language`` does not select per-language page trees; it selects the ROM
fallback page and human-readable API error text (including the missing-
``login.html`` 404).

A Store module's own ``web/`` is outside this mechanism.

Module UIs
----------

A Store module may ship ``web/`` under ``/store/modules/ID/web/`` (served as
``/modules/ID/web/…``), separate from the Host console. See
:doc:`plugins/develop_js`.
