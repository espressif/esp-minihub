Type catalog and UI
===================

:link_to_translation:`zh_CN:[中文]`

What drives the Overview page is the Host **type catalog**: CSV tables that list
types, parameter names, widgets and bounds. No per-type C struct is involved,
so a new type can still be added after a Hub has shipped. The built-in CSVs
under ``components/minihub_device_widget/model/`` are compiled into the
firmware, so editing them means a rebuild; the runtime table on the device,
``/data/config/devices.csv``, is read in preference to that embedded copy and
editing it needs no reflash.

Whether a type is controllable is read from the same catalog: the overview card
draws a power toggle, and the map marker takes a click, when the type offers a
writable ``power`` parameter (``dwHasPowerControl`` in
``components/minihub_web/web/js/device_widgets.js``). A type the catalog does not
list is judged by the point itself: a ``power`` parameter, or a boolean ``value``
summary. The instance overrides the type — ``visible: false`` or ``access: "R"``
on ``props.params.power`` removes the toggle.

The runtime half of the contract — one device is one point, live values under
``props.params`` — is in :doc:`data_model`.

A widget is drawn where the two halves meet. The catalog supplies the shape, the
live ``props.params`` supplies the value, and CSS does the drawing — not a
per-parameter SVG.

Manufacturer and category are absent from those tables on purpose. They describe
a particular unit, not a type, so they live on top-level ``props``
(:doc:`data_model`):

* ``manufacturer`` — detail row for brand
* ``category`` (or ``product`` / ``product_name``) — product display name;
  the card badge prefers it. Widgets and the type filter still use
  ``point.type`` against this catalog.
* Do not add manufacturer, category, or OTA to ``parameters.csv``.

Built-in device types and parameters
------------------------------------

Source of truth: ``components/minihub_device_widget/model/*.csv``. This page
renders the device types, parameters and UI elements from those CSVs.

* :download:`devices.csv <../_static/catalog/model/devices.csv>`
* :download:`parameters.csv <../_static/catalog/model/parameters.csv>`
* :download:`ui_elements.csv <../_static/catalog/model/ui_elements.csv>`
* :download:`services.csv <../_static/catalog/model/services.csv>` — reference only (RainMaker system services); not part of the widget registry
* :download:`catalog.zip <../_static/catalog.zip>`

Built-in device types
^^^^^^^^^^^^^^^^^^^^^

.. minihub-catalog:: devices

Plugin ``devices.json`` declares **only the subset this device uses**. The JS
demos show it:

* ``examples/plugins/color_bulb_js`` → ``type: light`` (power / brightness / hue /
  saturation; cct hidden with ``visible: false``)
* ``examples/plugins/meter_plug_js`` → ``type: switch`` (writable power; read-only
  voltage / current / watts)

The catalog ships the following sensor types: ``sensor_contact``,
``sensor_motion``, ``sensor_occupancy``, ``sensor_tamper``, ``sensor_water``,
``sensor_smoke``, ``sensor_illuminance``, ``sensor_temp_humidity``,
``sensor_air_quality``. Adapters may also register
other type strings such as ``unknown``; those points work for rules and REST. When
the type is missing from the catalog the detail panel still draws the parameter
rows — a name the parameter table knows keeps its label and its control, a name it
does not know becomes a read-only text row labelled with the raw name — but there
is no type-level parameter order, default bounds or avatar, and the browser console
logs ``type not in catalog``. Add the type to the CSV before you expect catalog UI.

Built-in parameters and widgets
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``ui_element`` values the catalog accepts are the ids in
``components/minihub_device_widget/model/ui_elements.csv`` — today ``ui.toggle`` /
``ui.slider`` / ``ui.dropdown`` / ``ui.text`` / ``ui.button`` / ``ui.hidden``. Web
maps them to toggle / slider / dropdown / text / button / hidden; the parameter
table below shows which ones today's rows use. Two legacy names,
``ui.hue-slider`` and ``ui.hue-circle``, are still accepted in the catalog and
both mean slider; plugin ``devices.json`` accepts neither, nor anything else
outside the closed set. Hue / saturation / CCT are **sliders**; rainbow or tone
comes from CSS.

.. figure:: ../_static/data_model/widget_styles.svg
   :alt: The parameter widget shapes: toggle, slider, dropdown, text, button and hidden

   What each ``ui_element`` draws.

.. minihub-catalog:: parameters

``ui.hidden`` = never draw at the type level. Instance ``visible: false`` =
hide **this** device's param while keeping it in ``props.params``. Read-only
sensors use instance ``access: "R"`` (shown, not draggable).
How a shape and a live value combine into one row is in :doc:`data_model`.

UI elements
+++++++++++

The closed set of UI elements, and what each draws:

.. minihub-catalog:: ui_elements

Read-only telemetry such as ``wifi_signal`` (WiFi RSSI, dBm) lives in the catalog
as a ``ui.text`` parameter. MQTT adapters (ESPHome, Tasmota) attach it to a device
point as an *instance* parameter rather than a built-in type column, and set
``access: "R"``; the detail panel then shows the value and offers no control. Add
any such read-only sensor the same way: a ``parameters.csv`` row with
``ui_element: ui.text`` plus ``access: "R"`` on the instance entry. ESPHome adds
it from its ``sensor.wifi_signal`` entity; for the one-point-per-device
registration contract see :doc:`data_model`.

Icons and artwork
-----------------

Device **avatars** are SVG under
``components/minihub_device_widget/images/devices/``. The build packs them into
``/data/images`` in the storage partition. Web serves the file named after the type
under ``/img/devices/``; ``GET /api/device_widgets`` also returns the URL. The icon
column of the type table above shows those files.

Parameter rows are **not** per-parameter SVGs: controls are CSS widgets from
``device_widgets.js`` / ``device_widgets.css``. Optional parameter icons live
under ``images/parameters/`` (may be empty); naming is in
``components/minihub_device_widget/README.md`` (Icons).

CSV columns
-----------

Two layers. Core never owns either CSV.

.. code-block:: text

   Host catalog (type)          Plugin instance (this device)
   model/devices.csv            devices.json
   model/parameters.csv         -> register one point
          |                            |
          v                            v
   GET /api/device_widgets      GET /api/points  (props.params)
          \                          /
           UI = catalog ∩ live params

``devices.csv`` columns: ``device_id``, ``parameters`` (CSV names),
``primary_param``, ``mandatory_params``, ``image`` (class avatar under
``images/devices/``). All five headers must be present when you upload the file;
the ``image`` cells may be empty.

``primary_param`` names the parameter that summarises a device of that type, so
an adapter can use it for the point's compact ``value``. The generated flat
registry repeats it on every row of the type, and
``minihub_device_widget_get()`` exposes it once per type; it is empty when the
type declares none. A non-empty value must be one of the type's ``parameters``:
the generator and the firmware both reject a name the type does not list.

``parameters.csv`` columns: ``parameter``, ``label_zh``, ``label_en``,
``data_type``, ``properties``, ``min`` / ``max`` / ``step`` / ``unit``,
``ui_element``, ``options`` (JSON array for dropdown; each item carries a
``labels`` map rather than a flat ``label``), ``description``, ``visible``,
``css``, ``image`` — these fifteen headers are required on upload — plus
optional ``requires``. ``visible`` must be there but its value is not read: the
runtime hides a parameter only from the instance's ``props.params.NAME.visible``.
Parameter ``image`` is flattened into the runtime table and is the first candidate
the firmware's control-icon resolver tries, before ``<device>_<parameter>.svg`` and
``<parameter>.svg``. Yet ``/api/device_widgets`` always answers ``""`` for it: a
parameter row uses the ``icon`` the plugin instance sets, and the control itself is
drawn by CSS.

Upload limits: 128 KiB per file, at most 24 columns and 256 rows, and column
headers match case-insensitively (parameter names do not).

The two label columns are what makes the console bilingual. ``/api/device_widgets``
serves one ``label`` per parameter, picked from the row's ``labels`` map by the
``lang`` query. A ``lang`` value the Hub does not know is served as ``zh``; with no
``lang`` at all it consults ``Accept-Language`` and defaults to ``en``. Adding a
language touches five known places — the CSV column, the
generator's ``LABEL_COLS``, the flattener's column set (``PARAM_REQ_COLS``,
``dw_param_cols_t``, ``dw_build_style``), the codes ``dw_request_lang()`` accepts,
and the console's own language set (``i18n.js`` plus the switch options) — and
none of the label consumers: the entry struct, the API shape and the card and
rule renderers stay as they are.

``components/minihub_device_widget/tools/check_catalog_i18n.py`` runs with the
generator and grades the same three ways:

* **error** — a ``label_zh`` / ``label_en`` column missing, an empty
  ``label_zh`` cell, an option item without a ``labels`` map, the console's two
  string files disagreeing on their key set, or a key the console asks for that
  no string file defines;
* **warning** — an empty ``label_en`` cell, or ``label_en`` equal to
  ``label_zh``. Proper nouns (``PM2.5``, ``HSV``, ``CCT``) legitimately read the
  same in both languages, so equality warns instead of failing;
* **runtime fallback** — a language the row does not provide serves
  ``label_zh``, then the parameter name. A warning degrades to Chinese; it never
  breaks a build or a page.

``requires`` is a precondition for a writable control. The default is one
parameter name: ``power`` means the same as ``{"all": ["power"]}``. The
control stays disabled while that parameter is explicitly off. Use an object
when more than one parameter is involved:

* ``{"all": ["power", "child_lock"]}`` — every listed parameter must be on
  (AND). One explicit off disables the control.
* ``{"any": ["power", "indicator"]}`` — at least one must be on (OR). The
  control is disabled only when every listed parameter is explicitly off.
* ``{"not": ["child_lock"]}`` — none of these may be on. One explicit on
  disables the control. ``not`` may stand alone or sit beside ``all`` or
  ``any`` (not both), for example ``{"all": ["power"], "not": ["child_lock"]}``.

A missing parameter, or a value that is not on/off, does not count as on or
off. An empty cell means no precondition. Read-only rows are never disabled
this way. ``power`` and ``indicator`` stay empty so they remain usable while
the device is off. A plugin ``devices.json`` param may set ``requires`` (a
name, or an ``all`` / ``any`` / ``not`` object) to override the catalog for
that device. In ``parameters.csv`` the object must be a quoted JSON cell.
The same name cannot appear twice, including once under ``all`` or ``any``
and again under ``not``.

Type catalog remote update -- the Web console has no page for this, so use the
REST developer route (still needs ``CONFIG_MINIHUB_WEB_UI`` to serve the files):
``GET`` / ``PUT /api/dev/catalog/devices.csv`` and
``GET`` / ``PUT /api/dev/catalog/parameters.csv``. Both files must be present
before the Hub flattens them and calls reload; uploading only one succeeds with
``rebuilt: false`` and a hint.

Each stage fails at a different granularity:

* A **PUT** is all-or-nothing. Duplicate ``device_id`` or ``parameter``, an
  unknown ``ui_element``, or a bad ``requires`` rejects the whole upload with
  **400** and changes nothing on disk.
* **Flattening** is also all-or-nothing: one device row naming a parameter that
  has no ``parameters.csv`` entry aborts the flatten.
* The flat registry read is where rows are skipped: a row whose fields look
  shifted, or whose style JSON has no usable ``widget``, is dropped and counted
  in a log line. If nothing survives, the file is rejected as a whole.
* A **rejected flat file still gets rewritten.** ``reload`` keeps serving the
  previous table, then seeds ``/data/config/devices.csv`` from the registry
  compiled into the firmware — so hand edits to that file are lost after a parse
  failure, and the same seeding runs at boot when the file is unreadable.

See :doc:`data_flow`.

How to use it when developing
-----------------------------

1. Add a device of an existing type (most common)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Do not edit Host CSV. Write plugin ``devices.json``; parameter names must already
exist in the catalog:

.. code-block:: JSON

   [
     {
       "device": "kitchen_lamp",
       "type": "light",
       "params": [
         { "name": "power", "init": false, "access": "RW" },
         { "name": "brightness", "init": "80", "access": "RW", "max": 80, "requires": "power" },
         { "name": "hue", "init": "0", "access": "RW", "requires": { "all": ["power"] } },
         { "name": "saturation", "init": "100", "access": "RW", "requires": { "any": ["power"] } },
         { "name": "cct", "init": "4000", "visible": false, "access": "RW", "requires": "power" }
       ]
     },
     {
       "device": "living_lamp",
       "type": "light",
       "params": [
         { "name": "power", "init": true, "access": "RW" },
         { "name": "brightness", "init": "40", "access": "RW", "max": 80, "requires": "power" }
       ]
     }
   ]

On register (one point per array element):

* ``point_id`` = ``your_protocol.device`` (e.g. ``sim.kitchen_lamp``)
* ``type`` = ``devices.json`` ``type`` (must be a catalog ``device_id``)
* Fill ``props.params`` from ``params`` (``init`` → ``value``; optional
  ``access`` / ``visible`` / ``min`` / ``max`` / ``step`` / ``unit`` / ``requires``)
* Presentation overrides, per parameter: ``icon`` (written ``./images/...``, and the
  file must already exist under ``components/minihub_device_widget/images/`` — a
  plugin cannot ship its own artwork), ``ui_element`` (one of ``ui.toggle`` /
  ``ui.slider`` / ``ui.dropdown`` / ``ui.text`` / ``ui.button`` / ``ui.hidden``) and
  ``css`` (plain class names). Per device: ``icon``. Leave any of them out and the
  catalog default for the device ``type`` is used.
  ``generate_devices_property_from_json.py`` rejects unknown icons, widgets outside
  the closed set, and unsafe ``css``.

A single object (not an array) is still accepted for one-device plugins.

.. only:: not hide_l3

   ELF: regenerate ``device_desc.gen.h`` after editing JSON — do not hand-edit the
   header. Scaffold an ELF source with ``tools/new_store_module.py --tier l3``.

2. Add a brand-new device type
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. Edit Host ``components/minihub_device_widget/model/devices.csv`` and
   ``parameters.csv`` (add ``images/devices/<id>.svg`` if needed)
2. ``python3 components/minihub_device_widget/tools/gen_device_widget.py --check``
3. Rebuild Host firmware

Or without reflashing -- REST, no Web page -- ``PUT /api/dev/catalog/devices.csv`` and
``PUT /api/dev/catalog/parameters.csv`` (both required before flatten + reload;
``python3 tools/dev_sync.py --open``).

.. warning::

   These two PUTs replace the **runtime** copy of the catalog on the device, one file at a
   time. The registry is only rebuilt once both are in place — a response with
   ``"rebuilt": false`` and a ``hint`` means the pair is still incomplete, and the console
   keeps rendering the old catalog until you send the other file. A malformed CSV is
   refused rather than loaded, so the live catalog is never left half-written; to go back,
   ``DELETE /api/dev/catalog/<file>.csv`` drops the stored copy and the next reload re-seeds
   the catalog built into the firmware. That fallback is the recovery path — a reboot alone
   does not undo a bad upload.

For **Zigbee** categories, also extend
``components/minihub_zigbee/config/zigbee_cluster_map.json`` (or overlay
``/data/config/zigbee_cluster_map.json``) so interview / reports / commands
wire to ``props.params`` without editing ``zigbee_adapter.c``. See
:doc:`zigbee`.

3. Read state and send control
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - API
     - Use
   * - ``GET /api/points``
     - Instances: points + ``props.params``
   * - ``GET /api/device_widgets``
     - Catalog: labels / widgets / default bounds / avatar URLs
   * - ``POST /api/devices/control``
     - Overview page: ``device`` = **type** (e.g. ``light``), plus ``parameter``,
       ``value``; UI also sends ``point_id`` for the instance
   * - ``POST /api/command``
     - Automation: explicit ``point_id`` + ``cmd``; optional ``params.parameter``
   * - ``turn_on`` / ``turn_off`` / ``toggle``
     - Common switch-class commands; other ``cmd`` names are adapter-specific

Rules that set ``parameter`` compare ``props.params.NAME.value``, not the
summary ``value``. See :doc:`data_model` and :doc:`api/host_rest`.

4. Who edits what
^^^^^^^^^^^^^^^^^

* **New type / default labels widgets bounds** → Host CSV (or remote catalog PUT)
* **New device of an existing type** → plugin ``devices.json``
* **Hide / tighten this device only** → instance ``visible`` / ``access`` /
  ``min`` / ``max``
* Do **not** add a Core C struct per type

Related
-------

* Points and Hub config: :doc:`data_model`
* REST examples: :doc:`api/host_rest`
* End-to-end flow: :doc:`data_flow`
* Type catalog component: ``components/minihub_device_widget/README.md``

.. only:: not hide_l3

   * ELF plugins: :doc:`plugins/develop_elf`
