Type catalog and UI
===================

:link_to_translation:`zh_CN:[中文]`

The Host **type catalog** drives the Devices page: which types exist, parameter
labels, widgets, and default bounds. The runtime point contract (one device =
one point, ``props.params``) is in :doc:`data_model`.

UI = type catalog ∩ live ``props.params`` (CSS widgets, not per-parameter SVGs).

**Manufacturer and category are not CSV parameters.** They live on top-level
``props`` (:doc:`data_model`):

* ``manufacturer`` — detail row for brand
* ``category`` (or ``product`` / ``product_name``) — product display name;
  the card badge prefers it. Widgets and the type filter still use
  ``point.type`` against this catalog.
* Do not add manufacturer, category, or OTA to ``parameters.csv``.

Built-in device types and parameters
------------------------------------

Source of truth: ``components/minihub_device_widget/model/*.csv``. Full tables:
``components/minihub_device_widget/STANDARD_TYPES.md``.

* :download:`devices.csv <../_static/catalog/model/devices.csv>`
* :download:`parameters.csv <../_static/catalog/model/parameters.csv>`
* :download:`ui_elements.csv <../_static/catalog/model/ui_elements.csv>`
* :download:`services.csv <../_static/catalog/model/services.csv>`
* :download:`STANDARD_TYPES.md <../_static/catalog/STANDARD_TYPES.md>`
* :download:`catalog.zip <../_static/catalog.zip>`

Built-in device types
^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 12 28 12 12 36

   * - Type id
     - Parameters (full set in the catalog)
     - Primary
     - Mandatory
     - Avatar
   * - ``light``
     - power, brightness, cct, hue, saturation, intensity
     - power
     - power
     - bulb icon (below)
   * - ``switch``
     - power, voltage_v, current_a, power_w, energy_wh, temperature, fault, indicator
     - power
     - power
     - switch / outlet icon (below)
   * - ``sensor_air_quality``
     - co2, pm25, pressure_hpa, battery
     - co2
     - co2
     - air-quality icon (below)
   * - ``sensor_contact``
     - contact, battery
     - contact
     - contact
     - contact icon (below)
   * - ``sensor_illuminance``
     - illuminance, battery
     - illuminance
     - illuminance
     - illuminance icon (below)
   * - ``sensor_motion``
     - motion, battery
     - motion
     - motion
     - motion icon (below)
   * - ``sensor_occupancy``
     - occupancy, battery
     - occupancy
     - occupancy
     - occupancy icon (below)
   * - ``sensor_smoke``
     - smoke, battery
     - smoke
     - smoke
     - smoke icon (below)
   * - ``sensor_tamper``
     - tamper, battery
     - tamper
     - tamper
     - tamper icon (below)
   * - ``sensor_temp_humidity``
     - temperature, humidity, battery
     - temperature
     - temperature
     - temperature / humidity icon (below)
   * - ``sensor_water``
     - leak, battery
     - leak
     - leak
     - water-leak icon (below)

Plugin ``devices.json`` declares **only the subset this device uses**. Demos:

* ``examples/plugins/color_bulb`` → ``type: light`` (power / brightness / hue /
  saturation; cct hidden with ``visible: false``)
* ``examples/plugins/meter_plug`` → ``type: switch`` (writable power; read-only
  voltage / current / watts)

The catalog ships the following sensor types: ``sensor_contact``,
``sensor_motion``, ``sensor_occupancy``, ``sensor_tamper``, ``sensor_water``,
``sensor_smoke``, ``sensor_illuminance``, ``sensor_temp_humidity``,
``sensor_air_quality``. Adapters may also register
other type strings such as ``unknown``; those points work for rules and REST, but
if the type is missing from the catalog the Devices page shows **type not
registered** with no standard widget card. Add the type to the CSV before you
expect catalog UI.

Built-in parameters and widgets
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Closed ``ui_element`` set: ``ui.toggle`` / ``ui.slider`` / ``ui.dropdown`` /
``ui.text`` / ``ui.hidden``. Web maps them to toggle / slider / dropdown / text /
hidden. Hue / saturation / CCT are **sliders**; rainbow or tone comes from CSS.

.. list-table::
   :header-rows: 1
   :widths: 14 12 10 18 14 32

   * - Parameter
     - Label
     - Data
     - Default bounds
     - UI
     - Used by
   * - ``power``
     - Power
     - Bool
     - —
     - toggle
     - light, switch
   * - ``brightness``
     - Brightness
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``cct``
     - CCT
     - Int
     - 2700–6500 K, step 100
     - slider
     - light
   * - ``hue``
     - Color
     - Int
     - 0–360 °, step 1
     - slider (rainbow track)
     - light
   * - ``saturation``
     - Saturation
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``intensity``
     - Intensity
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``voltage_v``
     - Voltage
     - Float
     - 0–250 V
     - text (read-only metering)
     - switch
   * - ``current_a``
     - Current
     - Float
     - 0–16 A
     - text (read-only metering)
     - switch
   * - ``power_w``
     - Active power
     - Float
     - 0–4000 W
     - text (read-only metering)
     - switch (watts; not on/off ``power``)
   * - ``energy_wh``
     - Energy
     - Float
     - 0–999999 Wh
     - text (read-only metering)
     - switch
   * - ``name``
     - Name
     - String
     - —
     - text
     - metadata; no device lists it by default → usually no control
   * - ``temperature``
     - Temperature
     - Float
     - -40–125 °C, step 0.1
     - text (read-only)
     - switch, sensor_temp_humidity
   * - ``humidity``
     - Humidity
     - Float
     - 0–100 %, step 0.1
     - text (read-only)
     - sensor_temp_humidity
   * - ``illuminance``
     - Illuminance
     - Float
     - 0–100000 lx, step 1
     - text (read-only)
     - sensor_illuminance
   * - ``co2``
     - CO2
     - Int
     - 0–5000 ppm, step 1
     - text (read-only)
     - sensor_air_quality
   * - ``pm25``
     - PM2.5
     - Float
     - 0–1000 µg/m³, step 0.1
     - text (read-only)
     - sensor_air_quality
   * - ``pressure_hpa``
     - Pressure
     - Float
     - 300–1100 hPa, step 0.1
     - text (read-only)
     - sensor_air_quality
   * - ``battery``
     - Battery
     - Int
     - 0–100 %, step 1
     - text (read-only)
     - sensor_* (all)
   * - ``contact``
     - Contact
     - Bool
     - —
     - text (read-only)
     - sensor_contact
   * - ``motion``
     - Motion
     - Bool
     - —
     - text (read-only)
     - sensor_motion
   * - ``leak``
     - Water leak
     - Bool
     - —
     - text (read-only)
     - sensor_water
   * - ``smoke``
     - Smoke
     - Bool
     - —
     - text (read-only)
     - sensor_smoke
   * - ``occupancy``
     - Occupancy
     - Bool
     - —
     - text (read-only)
     - sensor_occupancy
   * - ``tamper``
     - Tamper
     - Bool
     - —
     - text (read-only)
     - sensor_tamper

``ui.hidden`` = never draw at the type level. Instance ``visible: false`` =
hide **this** device's param while keeping it in ``props.params``. Read-only
sensors use instance ``access: "R"`` (shown, not draggable).

Icons and artwork
-----------------

Device **avatars** are SVG under
``components/minihub_device_widget/images/devices/``. The build packs them into
``/data/images`` on device. Web serves ``GET /img/devices/<name>``;
``GET /api/device_widgets`` also returns the URL.

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Type
     - File
     - Preview
   * - ``light``
     - ``images/devices/light.svg``
     - .. image:: ../_static/catalog/images/devices/light.svg
          :alt: light device icon
          :width: 64px
   * - ``switch``
     - ``images/devices/switch.svg``
     - .. image:: ../_static/catalog/images/devices/switch.svg
          :alt: switch device icon
          :width: 64px
   * - ``sensor_air_quality``
     - ``images/devices/sensor_air_quality.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_air_quality.svg
          :alt: air quality sensor icon
          :width: 64px
   * - ``sensor_contact``
     - ``images/devices/sensor_contact.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_contact.svg
          :alt: contact sensor icon
          :width: 64px
   * - ``sensor_illuminance``
     - ``images/devices/sensor_illuminance.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_illuminance.svg
          :alt: illuminance sensor icon
          :width: 64px
   * - ``sensor_motion``
     - ``images/devices/sensor_motion.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_motion.svg
          :alt: motion sensor icon
          :width: 64px
   * - ``sensor_occupancy``
     - ``images/devices/sensor_occupancy.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_occupancy.svg
          :alt: occupancy sensor icon
          :width: 64px
   * - ``sensor_smoke``
     - ``images/devices/sensor_smoke.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_smoke.svg
          :alt: smoke sensor icon
          :width: 64px
   * - ``sensor_tamper``
     - ``images/devices/sensor_tamper.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_tamper.svg
          :alt: tamper sensor icon
          :width: 64px
   * - ``sensor_temp_humidity``
     - ``images/devices/sensor_temp_humidity.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_temp_humidity.svg
          :alt: temperature / humidity sensor icon
          :width: 64px
   * - ``sensor_water``
     - ``images/devices/sensor_water.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_water.svg
          :alt: water-leak sensor icon
          :width: 64px

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
``primary_param``, ``mandatory_params``, optional ``image`` (class avatar under
``images/devices/``).

``parameters.csv`` columns: ``parameter``, ``label_zh``, ``label_en``,
``data_type``, ``properties``, ``min`` / ``max`` / ``step`` / ``unit``,
``ui_element``, ``options`` (JSON array for dropdown; each item carries a
``labels`` map rather than a flat ``label``), ``description``, ``css``, optional
``requires``. A leftover ``visible`` header is ignored; hide with instance
``visible: false``. Parameter ``image`` is unused today (controls are CSS).

The two label columns are what makes the console bilingual. ``/api/device_widgets``
serves one ``label`` per parameter, picked from the row's ``labels`` map by the
``lang`` query (``zh`` / ``en``, falling back to ``Accept-Language``, then to
``zh``). Adding a language touches five known places — the CSV column, the
generator's ``LABEL_COLS``, the flattener's column set (``PARAM_REQ_COLS``,
``dw_param_cols_t``, ``dw_build_style``), the codes ``dw_request_lang()`` accepts,
and the console's own language set (``i18n.js`` plus the switch options) — and
none of the label consumers: the entry struct, the API shape and the card and
rule renderers stay as they are.

``tools/check_catalog_i18n.py`` runs with the generator and grades the same
three ways, so a missing translation is visible at build time rather than
months later:

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

Type catalog remote update (needs ``CONFIG_MINIHUB_WEB_UI``):
``GET`` / ``PUT /api/dev/catalog/devices.csv`` and
``GET`` / ``PUT /api/dev/catalog/parameters.csv``. Both files must be present
before the Hub flattens them and calls reload. The reload is atomic and
failure-safe (bad rows skipped, unparseable file keeps the previous table,
missing/corrupt file self-heals from the embedded copy); see
:doc:`architecture_overview`.

How to use it when developing
-----------------------------

1. Add a device of an existing type (most common)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Do not edit Host CSV. Write plugin ``devices.json``; parameter names must already
exist in the catalog:

.. code-block:: json

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
* Presentation overrides, per parameter: ``icon`` (must reference a shipped
  ``./images/...`` icon), ``ui_element`` (one of ``ui.toggle`` / ``ui.slider`` /
  ``ui.dropdown`` / ``ui.text`` / ``ui.hidden``) and ``css`` (plain class names).
  Per device: ``icon``. Leave any of them out and the catalog default for the
  device ``type`` is used. ``generate_devices_property_from_json.py`` rejects
  unknown icons, widgets outside the closed set, and unsafe ``css``.

A single object (not an array) is still accepted for one-device plugins.

L3: regenerate ``device_desc.gen.h`` after editing JSON — do not hand-edit the
header. Demos: ``examples/plugins/color_bulb``, ``examples/plugins/meter_plug``.

2. Add a brand-new device type
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. Edit Host ``components/minihub_device_widget/model/devices.csv`` and
   ``parameters.csv`` (add ``images/devices/<id>.svg`` if needed)
2. ``python3 components/minihub_device_widget/tools/gen_device_widget.py --check``
3. Rebuild Host firmware

Or without reflashing: ``PUT /api/dev/catalog/devices.csv`` and
``PUT /api/dev/catalog/parameters.csv`` (both required before flatten + reload;
``python3 tools/dev_sync.py --open``).

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
     - Devices page: ``device`` = **type** (e.g. ``light``), plus ``parameter``,
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
* End-to-end flow: :doc:`architecture_overview`
* Type catalog component: ``components/minihub_device_widget/README.md``
* L3 plugins: :doc:`plugins/develop_l3`
