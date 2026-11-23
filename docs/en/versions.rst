Versions
========

:link_to_translation:`zh_CN:[中文]`

This guide documents |product_version| — the same string the device reports. Read
it together with the commit line in ``GET /api/system``: the version says what the
product author declared, the commit says which tree was built.

One number per device, two release trains
-----------------------------------------

``MINIHUB_VERSION`` is the device version, set once in
:file:`cmake/minihub_bundle/Kconfig.projbuild` (default ``0.1.0-dev``). The same string
is what the firmware reports as ``firmware_version``, what ``tools/deploy.sh`` names
the ``Firmwares/<chip>/<version>/`` folder with, and what the console shows in the
sidebar. It is not ``git describe``: the commit is reported separately, so a
rebuild without a version bump is visible as a different commit under the same
version.

The console ships on its own train. ``MINIHUB_WEB_VERSION``
(:file:`components/minihub_web/Kconfig.minihub`) is its **single** source: both the
firmware build and the deploy step derive from it, so a freshly flashed Hub and the
published bundle always agree. Write it without the leading ``v`` (only the deploy
folder name adds that). It defaults to ``MINIHUB_VERSION`` but is deliberately
separate: a web-only fix bumps it and leaves the firmware version alone. Do not
hand-edit a ``version.txt`` in the tree -- it is generated at build time.

So **a Hub can legitimately report two versions**, and a device that was updated in
two steps usually does. Before you compare a device against this guide, check both:

.. code-block:: bash

   curl -s -H "Authorization: Bearer $MINIHUB_TOKEN" http://HUB/api/system \
     | python3 -m json.tool   # version, commit
   # the console's own value is in window.mhWebVersion, shown in the sidebar

A firmware that is newer than the console it runs against is normal; a console
newer than the firmware usually means ``app-flash`` updated the app while
``/data/web`` kept the previous bundle. Update the console as its own step
(:doc:`web_console`) or flash the whole set (:doc:`production`).

What an update may offer
------------------------

The deploy server decides whether to offer a firmware by comparing
``cur_version`` (what the device reports) with the folder names it serves, so
**the version string must sort monotonically**. ``0.1.10`` sorting before
``0.1.9`` as text is enough to make a device believe it is already current:
zero-pad, or move to a scheme that compares correctly. Store packages are
compared by the catalog's own version field, and a downgrade is not accepted by
``POST /api/store/upgrade`` — use ``reinstall`` to push the same or an older build
(:doc:`api/plugin_rest`).

.. only:: not hide_l3

   Native modules are checked against the Host ABI instead of the product version:
   ``abi_version`` and ``core_api_*`` in ``module.json`` must bracket the Host's
   ``MINIHUB_MODULE_ABI_VERSION`` (:doc:`api/module_abi`). The ABI number changes
   only when a plugin can no longer be loaded safely by an older Host.

Where the notes live
--------------------

There is no collected changelog. Each published firmware carries its own
``Firmwares/<chip>/<version>/release_notes.txt`` (``tools/publish_firmware.py``
writes the file next to the image), and a Hub shows the notes of what it is being
offered through the update UI and ``GET /api/update/notes`` (:doc:`ota`). A product
that needs a changelog assembles it from those per-version files — nothing else
records what changed between two builds.

Pinned toolchain
----------------

The guide pins one ESP-IDF revision and one target, both in
:doc:`getting_started`. A re-pin is a guide change, not a patch note: it moves the
build instructions, the partition assumptions and sometimes the memory budget, so
treat "the guide moved to a new IDF" as a version boundary for your own port.

Related: :doc:`getting_started`, :doc:`production`, :doc:`ota`, :doc:`web_console`,
:doc:`api/deploy_index`, :doc:`limits`.
