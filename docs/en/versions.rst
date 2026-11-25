Versions
========

:link_to_translation:`zh_CN:[中文]`

Read the device version together with the commit line in ``GET /api/system``: the
version is what the product author set, the commit is which tree was built.

One number per device, three release trains
-------------------------------------------

``MINIHUB_VERSION`` is the device version (firmware ``firmware_version`` and
``Firmwares/<chip>/<version>/`` for ``tools/deploy.sh``).
Set it in :file:`configs/sdkconfig/common.defaults`.
The Kconfig symbol lives in :file:`cmake/minihub_bundle/Kconfig.projbuild`.
It is not ``git describe``: the commit is reported separately.

The console version is ``MINIHUB_WEB_VERSION``
(:file:`components/minihub_web/Kconfig.minihub`). The firmware build and
``tools/deploy.sh`` both read it. The deploy folder is ``Webs/v<ver>/``; the stamped
``version.txt`` and ``window.mhWebVersion`` omit the leading ``v``. It defaults to
``MINIHUB_VERSION``. ``version.txt`` is generated at build time and is not hand-edited
in the tree.

The LCD Brookesia UI (``/data/ui``) version is ``MINIHUB_LCD_UI_VERSION``
(:file:`components/minihub_lcd/Kconfig.minihub`). The Host build stamps
``/data/ui/version.txt``; ``tools/deploy.sh`` publishes ``LCD_UI/v<ver>/``, served as
``GET /api/ui``. ``POST /api/update/apply`` with ``"type":"ui"`` writes
``/data/ui``; reboot after apply. It defaults to ``MINIHUB_VERSION``.

A Hub can report firmware, Web, and LCD UI versions at once. Check each when comparing
a device to this guide:

.. code-block:: bash

   curl -s -H "Authorization: Bearer $MINIHUB_TOKEN" http://HUB/api/system \
     | python3 -m json.tool   # version, commit

Firmware, Web, and LCD UI can update independently. Each surface only offers its
own channels plus firmware:

* Web console (:doc:`web_console`): firmware, Web, and device assets.
* LCD System page: firmware and LCD UI (check / upgrade); reboot after a UI apply.
* REST: ``POST /api/update/apply`` with ``"type":"firmware"|"web"|"ui"|"asset"``
  (automation; the Web console does not expose ``ui``).

Full flash: :doc:`production`.

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

Canonical release notes live in the programming guide under
:doc:`releases/index`. Each deploy package still carries a short
``release_notes.txt`` that points at that page (Hub update UI and
``GET /api/update/notes``). Firmware notes live in
:file:`application/<name>/release_notes.txt` and are copied next to the app
image at build time; ``tools/deploy.sh`` publishes them under
``Firmwares/<chip>/<version>/``. Web and LCD UI notes live with those asset
trees and publish under ``Webs/<version>/`` or ``LCD_UI/<version>/``.

Pinned toolchain
----------------

The guide pins one ESP-IDF revision and one target, both in
:doc:`getting_started`. A re-pin is a guide change, not a patch note: it moves the
build instructions, the partition assumptions and sometimes the memory budget, so
treat "the guide moved to a new IDF" as a version boundary for your own port.

Related: :doc:`getting_started`, :doc:`production`, :doc:`ota`, :doc:`web_console`,
:doc:`api/deploy_index`, :doc:`limits`, :doc:`releases/index`.
