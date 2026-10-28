System Time
===========

:link_to_translation:`zh_CN:[中文]`

The clock decides when scheduled rules fire and what timestamp a point update
keeps in history.

Where the time comes from
-------------------------

Core never sets the clock. The default product does, once per boot, in
``application/minihub/main/main.c``:

.. code-block:: c

   esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
   esp_sntp_setservername(0, "pool.ntp.org");
   esp_sntp_init();

That is a plain SNTP poll: it needs a route and outbound UDP/123. It starts in
parallel with the network, not after it, so a Hub that boots with no path to an
NTP server stays at its 1970 start time — a LAN with no NTP source is enough to
keep it there. Move the call behind your own "uplink is up" point, point it at an
internal server, or drop it in a product that syncs another way.

There is no real-time clock behind it. Every boot starts from the epoch and has
to sync again; nothing carries the previous time across a reboot or an OTA.

What an unsynced clock looks like
---------------------------------

* Log lines and the history store keep whatever time the device had at that
  moment. History rows written before the sync land keep their early timestamps
  — they are not rewritten later, so a chart can start in 1970 and jump.
* ``uptime_s`` in ``GET /api/system`` is reliable regardless: it counts from boot
  with ``esp_timer``, not from the wall clock.
* Nothing else in the Hub reads a wrong clock destructively: point values, the
  Store and the radios are time-independent.

Scheduled and ``once`` rules are the part that waits for a plausible clock. The
engine compares its own ``gmtime`` against the year 2020 and skips the whole time
pass below that (:file:`components/minihub_rules/minihub_rules.c`). It deliberately
does **not** wait for SNTP to report *completed*: on a network with no reachable
NTP server the status stays unset forever, and requiring it once left every time
rule dead. A clock you set yourself — from a product hook or a plugin — is enough,
as long as the year is real.

Timezone
--------

Offsets are fixed: no place names, no DST, and UTC by default. Whole hours run
from ``UTC-12`` to ``UTC+14``, and the half and quarter hours that places actually
use are in the list too: ``UTC-9:30``, ``UTC-3:30``, ``UTC+3:30``, ``UTC+4:30``,
``UTC+5:30``, ``UTC+5:45``, ``UTC+6:30``, ``UTC+8:45``, ``UTC+9:30``,
``UTC+10:30``, ``UTC+12:45``. ``POST /api/system/timezone`` stores the choice and applies it with
``setenv("TZ")`` plus ``tzset()`` (:file:`components/minihub_core/minihub_tz.c`),
which changes what the console *shows*.

Schedules do not follow it. A time rule is stored as UTC wall clock and matched
with ``gmtime_r``, so moving the zone moves the display and leaves the firing
instant where it was. Write the hour you mean in UTC.

Checking the time
-----------------

No endpoint returns the current wall clock today: ``GET /api/system`` reports
uptime and memory, not ``time()``. Until it does, two practical checks:

* look for the SNTP line in the log after boot, and
* set a ``once`` or scheduled rule one minute ahead and watch whether it fires —
  a rule that never fires while point triggers do is the clock, not the rule.

Related: :doc:`rules` (what a rule may match on), :doc:`networking` (how the Hub
gets a route in the first place), :doc:`limits` (the intervals the engine polls).
