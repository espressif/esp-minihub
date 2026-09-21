Rules engine
============

:link_to_translation:`zh_CN:[中文]`

Host automation lives in ``rules.json`` (``minihub_rules``). A rule has
``when`` (trigger), optional ``if`` (extra conditions), and ``then`` (actions).
Scenes (``scenes.json``) are a **separate** document invoked by REST; they are
**not** a ``then`` type.

Implementation: ``components/minihub_rules/minihub_rules.c``. Persistence and
REST: ``GET /api/rules``, ``POST /api/rules/import`` (:doc:`api/host_rest`).

Document shape
--------------

.. code-block:: json

   {
     "rules": [
       {
         "id": "door_light",
         "name": "Hall light on door open",
         "enabled": true,
         "when": {
           "type": "point",
           "point_id": "sim.color_bulb",
           "parameter": "brightness",
           "op": "gt",
           "value": "50"
         },
         "if": [ { "point_id": "sim.meter_plug", "parameter": "power", "op": "eq", "value": "on" } ],
         "then": [
           { "type": "command", "point_id": "sim.color_bulb", "cmd": "turn_on" },
           { "type": "notify", "message": "Door opened" }
         ]
       }
     ]
   }

``when``
--------

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - ``type``
     - Behavior
   * - ``point``
     - Fire on Hub event ``point.updated``. Prefer ``parameter`` + ``op`` +
       ``value`` against ``props.params.NAME.value``. Legacy ``equals`` still
       matches the point summary ``value``.
   * - ``time``
     - Fire once per day at ``at`` (``HH:MM``). Needs SNTP.

``if`` is an array of extra conditions. Each item uses the same shape as a
point ``when`` (all must match).

Comparison ``op``
-----------------

Normalized in ``normalize_op``. Unknown tokens become ``eq``.

.. list-table::
   :header-rows: 1
   :widths: 16 28 56

   * - Canonical
     - Aliases
     - Meaning
   * - ``eq``
     - ``=`` ``==``
     - Equal (default)
   * - ``ne``
     - ``!=`` ``<>``
     - Not equal
   * - ``lt``
     - ``<``
     - Less than
   * - ``le``
     - ``<=`` ``≦``
     - Less or equal
   * - ``gt``
     - ``>``
     - Greater than
   * - ``ge``
     - ``>=`` ``≧``
     - Greater or equal

``then``
--------

Object or array. **Only** these four ``type`` values are executed. There is no
``scene`` action and no direct ``minihub_point_set``. Commands go through
``minihub_command_invoke`` (same path as ``POST /api/command``). Run a scene
with ``POST /api/scenes/invoke``, not from a rule ``then``.

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - ``type``
     - Fields
   * - ``command``
     - ``point_id`` + ``cmd`` (same names as REST; power uses ``turn_on`` /
       ``turn_off`` / ``toggle``).
   * - ``command_by_type``
     - ``cmd`` plus optional ``types`` CSV (default ``light,switch``). Each
       listed point must also pass Host’s light/plug filter (skips ``mock``,
       PIR/motion-like names). Typical use: “turn all lights off”.
   * - ``delay``
     - ``seconds``. Implemented with ``vTaskDelay`` on the **rule task**, so
       it **blocks** later ``then`` items and other rules on that task. Use
       sparingly; prefer scene ``delay_ms`` for sequenced commands.
   * - ``notify``
     - ``message``. Calls the product IM hook if linked; otherwise logs a
       warning.

Unknown ``then.type`` is logged and skipped.

Import and MCP
--------------

* REST import: ``POST /api/rules/import`` with the document itself or
  ``{"url":"http://…"}``.
* MCP: ``rules.list`` / ``rules.put``.

Related
-------

* Point model: :doc:`data_model`
* Scenes: :doc:`data_model` (``scenes.json``)
* REST: :doc:`api/host_rest`
* End-to-end flow: :doc:`architecture_overview`
* Event types on ``/ws/events``: :doc:`api/host_rest`
