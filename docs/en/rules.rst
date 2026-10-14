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
           "point_id": "sim.js_kitchen",
           "parameter": "brightness",
           "op": "gt",
           "value": "50",
           "delay_sec": 0
         },
         "if": [ { "point_id": "sim.js_plug", "parameter": "power", "op": "eq", "value": "on" } ],
         "then": [
           {
             "actions": [
               { "type": "command", "point_id": "sim.js_kitchen", "cmd": "turn_on" },
               { "type": "notify", "message": "Door opened" }
             ],
             "delay_after": 5
           }
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

Both ``point`` and ``time`` accept an optional ``delay_sec``: the rule waits
that many seconds after the trigger before the first step runs. A value of 0
means "run immediately" and is the default.

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

``then`` — steps
--------------------

``then`` is an **ordered list of steps**. Each step is an object with:

* ``actions`` — array of action objects executed together (max 4 per step).
* ``delay_after`` — seconds to wait **after** this step before the next step.
  Ignored on the last step; 0 means no delay.

Steps run serially: step 0 runs after ``when.delay_sec`` (if any), then each
next step starts after the previous step’s ``delay_after``. This replaces the
old inline ``delay`` action — a delay now belongs to the *preceding* step, never
as a standalone action. A legacy flat ``then`` (a bare action object or array,
optionally with inline ``delay`` actions) is still parsed into steps by the
firmware.

.. code-block:: text

   when (trigger) --[when.delay_sec]--> step0 actions --[delay_after]--> step1 actions --> ...

In the Web editor the wait is picked from a step’s action-type dropdown
(``Delay``), not from a separate field: the delay always settles as the trailing
wait of the step it belongs to, and a step carries at most one. Because a delay
is what separates two moments, ``+ Add step`` is offered only once the last step
ends with a delay.

Actions (``type``)
~~~~~~~~~~~~~~~~~~~

Only these three ``type`` values are executed (no ``scene`` action, no
``minihub_point_set``). Commands go through ``minihub_command_invoke`` (same
path as ``POST /api/command``). Run a scene with ``POST /api/scenes/invoke``,
not from a rule ``then``.

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - ``type``
     - Fields
   * - ``command``
     - ``point_id`` + ``cmd`` (same names as REST; power uses ``turn_on`` /
       ``turn_off`` / ``toggle``). Optional ``params`` for value commands
       (``brightness``, ``color_temp``, ``color``).
   * - ``command_by_type``
     - ``cmd`` plus optional ``types`` CSV (default ``light,switch``). Each
       listed point must also pass Host’s light/plug filter (skips ``mock``,
       PIR/motion-like names). Typical use: “turn all lights off”. Still
       executed, but the editor no longer offers this option — rules that
       already use it stay editable and keep running.
   * - ``notify``
     - ``message``. Calls the product IM hook if linked; otherwise logs a
       warning.

Unknown ``then.type`` is logged and skipped.

Execution is **non-blocking**: delays use one-shot ``esp_timer`` callbacks that
only enqueue a job on a dedicated executor task — commands never run inside the
timer callback. A monotonic generation counter is bumped on every reload, so any
delayed job armed before a re-import is dropped (no use-after-free after
reload).

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
