Rules engine
============

:link_to_translation:`zh_CN:[中文]`

Automation runs on the device, not in a cloud, so a Hub keeps working when the
internet is down. The rules live in ``rules.json`` and are evaluated by
``minihub_rules``. A rule has three parts: ``when`` triggers it, an optional
``if`` adds conditions, and ``then`` is what happens.

.. image:: ../_static/rules/rules_flow.svg
   :align: center
   :alt: MiniHub rules evaluation: point.updated → when → if → then → actions

`Interactive version <_static/rules/index.html>`_ — the cards tour the stages on
their own (hover pauses); each card carries one constraint.

Scenes look similar but are a separate document in ``scenes.json``. They are
fired by REST and are never a ``then`` type: a scene is a list of commands a
person invokes; a rule invokes itself.

Implementation: ``components/minihub_rules/minihub_rules.c``. Persistence and
REST: ``GET /api/rules``, ``POST /api/rules/import`` (:doc:`api/host_rest`).

Document shape
--------------

.. code-block:: JSON

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
               { "type": "command", "point_id": "sim.js_kitchen", "cmd": "turn_on" }
             ],
             "delay_after": 5
           },
           {
             "actions": [
               { "type": "notify", "message": "Door opened" }
             ]
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
     - Fire on the Hub event ``point.updated``. Prefer ``parameter`` + ``op`` +
       ``value``, which compares ``props.params.NAME.value``; without
       ``parameter`` the same comparison uses the point summary ``value``. Either
       way the rule is **edge-triggered**: it fires when the compared value
       changes into a matching state, so a device re-sync that re-publishes the
       same value does not re-fire it.
   * - ``time``
     - Fire once per day when the clock reads ``at`` (``HH:MM``, stored as UTC in
       the hub timezone chosen on the System page). Time rules are polled every
       second, so a rule fires within ~1 s of the minute boundary.
   * - ``once``
     - Fire a single time at ``at`` given as ``YYYY-MM-DD HH:MM`` (UTC, minute
       resolution). The rule is then latched as done until the rules are
       reloaded.

A time rule needs a plausible clock, not a finished SNTP sync: any system time
whose year is 2020 or later is accepted, so a LAN-only hub with a manually set
time still runs its schedules. Where the clock comes from and what an unsynced one
changes: :doc:`time`.

A time rule created before the hub timezone setting existed stored its ``at``
as UTC computed from the *browser* timezone at edit time. After a hub timezone
is chosen on the System page, the console reads and writes ``at`` in the hub
timezone, so such a pre-existing rule can fire that many hours off its original
wall-clock time: a browser on UTC+8 with a hub on UTC is 8 hours. Re-open and
re-save the rule to re-anchor it to the hub timezone.

Both ``point`` and ``time`` accept an optional ``delay_sec``: the rule waits
that many seconds after the trigger before the first step runs. A value of 0
means "run immediately" and is the default. The console has no field for it —
set it in ``rules.json`` and re-import; the in-step waits are the editor's only
delay controls.

``if`` is an array of extra conditions — up to **4**, all of which must match.
Each item uses the same shape as a point ``when`` (``point_id``, ``parameter``,
``op``, ``value``). An ``if`` condition is compared against the current point
state when the rule fires; it does not trigger anything by itself.

Comparison ``op``
-----------------

Normalized in ``normalize_op``. Unknown or missing tokens become ``eq``.

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

Both sides are strings coming from the point, so ``eq`` / ``ne`` compare loosely
first: ``on`` / ``true`` / ``1`` are one class and ``off`` / ``false`` / ``0``
another, in any case, then a numeric compare. The ordering ops need both sides
to parse as numbers — otherwise they fall back to a lexicographic string compare,
so ``"9" > "10"``. A threshold in JSON may be a string, a number, or a boolean.

``then`` — steps
--------------------

``then`` is an **ordered list of steps**. Each step is an object with:

* ``actions`` — array of action objects executed together (max 4 per step).
* ``delay_after`` — seconds to wait **after** this step's actions. When a step
  follows, the wait separates the two moments; on the last step nothing follows,
  so the wait only delays the end of the rule. 0 means no delay.

Steps run serially: step 0 runs after ``when.delay_sec`` (if any), and each
next step starts once the previous step's actions and ``delay_after`` are done.
To postpone the first (or only) step, set ``when.delay_sec`` — a trailing wait
on the last step does not move its actions. This replaces the old inline
``delay`` action: the wait now belongs to the step before it rather than being
an action of its own. A legacy flat ``then`` (a bare action object or array,
optionally with inline ``delay`` actions) is still parsed into steps by the
firmware.

.. code-block:: text

   when (trigger) --[when.delay_sec]--> step0 actions --[delay_after]--> step1 actions --> ...
   last step:  ... actions --[delay_after]--> rule ends

In the Web editor the wait is picked from a step’s action-type dropdown
(``Delay``), not from a separate field: the delay always settles as the trailing
wait of the step it belongs to, and a step carries at most one. Because a delay
is what separates two moments, ``+ Add step`` is offered only once the last step
ends with a delay.

Actions (``type``)
~~~~~~~~~~~~~~~~~~~

These ``type`` values are recognized (no ``scene`` action, no direct
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
   * - ``delay``
     - ``delay_sec`` (``seconds`` is accepted too). Legacy inline wait: inside
       a step it holds the following actions, and in a flat ``then`` it becomes
       a step boundary. The editor rewrites it to ``delay_after``.

Unknown ``then.type`` is logged and skipped.

Execution is **non-blocking**: delays use one-shot ``esp_timer`` callbacks that
only enqueue a job on a dedicated executor task — commands never run inside the
timer callback. A monotonic generation counter is bumped on every reload, so any
delayed job armed before a re-import is dropped (no use-after-free after
reload).

Where the work runs matters when you time a rule: ``point.updated`` matching
happens on the Hub event-bus consumer task, so a rule's ``if`` conditions are
evaluated there and only the actions move to the executor task. Time and
``once`` rules are polled by a third task, once per second.

Limits (fixed in firmware):

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - What
     - Cap and behavior when exceeded
   * - Rules per document
     - 16 — later entries are dropped silently
   * - Steps per rule
     - 5 — later steps are dropped (a warning is logged for the legacy flat form)
   * - Actions per step
     - 4 — later actions are dropped
   * - ``if`` conditions
     - 4 — later conditions are ignored
   * - Point-trigger index
     - 16 buckets × 8 rules — a rule whose bucket is full is **not** triggered and
       is reported in the log
   * - Pending delays
     - 8 timer slots — when they are all busy the next step runs **without** its
       delay, with a warning
   * - Queued action jobs
     - 16 — a full queue drops the job
   * - Edge-detection cache
     - 256 ``(point_id, parameter)`` pairs — beyond that, redundant updates may
       fire again
   * - ``rules.json``
     - 128 KiB for the import body, the fetched URL, and the scene import alike

Every cap above, with the line of code that sets it, is listed in :doc:`limits`.

Import and MCP
--------------

* REST import: ``POST /api/rules/import`` with the document itself or
  ``{"url":"http://…"}``.
* MCP: ``rules.list`` / ``rules.put``.

.. warning::

   Import **replaces** ``/data/config/rules.json`` with what you send — it does not merge
   into the rules already on the device. ``minihub_config_save_rules()`` writes the posted
   document and renames it over the live file, so every rule you left out is gone after the
   reload. GET the current document first, add to it, then import the result; the same
   applies to scenes. The rename is why an import that fails halfway leaves your old rules
   intact rather than a truncated file.

Related
-------

* Point model: :doc:`data_model`
* Scenes: :doc:`data_model` (``scenes.json``)
* REST: :doc:`api/host_rest`
* End-to-end flow: :doc:`data_flow`
* Event types on ``/ws/events``: :doc:`api/host_rest`
