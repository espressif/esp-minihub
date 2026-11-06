Fixed Limits
============

:link_to_translation:`zh_CN:[中文]`

This page collects every capacity the Host refuses to exceed. Each row names the
location of its value in the code (the ``Defined at`` column), that is, which file
and symbol you edit to change it.

.. minihub-limits:: core

.. minihub-limits:: rules

.. minihub-limits:: scenes

.. minihub-limits:: store

.. minihub-limits:: web

.. minihub-limits:: auth

.. minihub-limits:: matter

.. minihub-limits:: radios

.. minihub-limits:: update

.. minihub-limits:: JS

.. minihub-limits:: hub

How to change a limit
---------------------

The ``Defined at`` column points at one of two kinds of location, and the edit
differs:

* A row that points at ``Kconfig.minihub`` is a **default**. You can override it in
  your product without touching Core source: set the symbol again in your product's
  Kconfig fragment or ``sdkconfig.defaults`` and rebuild.
* A row that points at a ``#define`` in a ``.h`` file, or a ``char buf[N]`` in a
  ``.c`` file, is a **compile-time constant**. You must change the source and rebuild
  the firmware; most of these set a buffer or a task-stack size directly.

What to watch when changing
---------------------------

* The caps are interlocked, so do not raise just one. The 64-point registry is the
  tightest ceiling in the Hub, and Matter counts each endpoint as a point, so one
  multi-endpoint device takes several rows; before you enlarge the Matter tables,
  confirm the 64 points still suffice (see :doc:`matter`).
* Buffer constants such as the ``*_BODY_MAX`` symbols and ``char buf[N]`` are sized to
  fit a task stack. When you raise one, you usually must also raise the matching
  ``*_STACK_SIZE`` Kconfig, or the stack overflows. The JS worker stack and the Hub
  reload stack follow this rule.
* The documentation table also lists these values; when you change a constant in the
  code, update the document as well, or the docs build fails.
* Most of these constants live in the shared Core components, so a change affects
  every product built from that Core. Prefer a product-level Kconfig override over
  editing Core source.

Risks
-----

* Raising a count while leaving its buffer unchanged causes silent truncation: the
  event queue drops entries past 32, a document over the import cap is rejected as a
  whole, and OTA chunks are cut short.
* Changing a compile-time constant outside ``Kconfig.minihub`` is a Core change that
  touches every product; prefer the product-level override where one exists.
* Enlarging the Matter tables costs more PSRAM and lengthens commissioning, and
  raising point counts or buffers eats heap; on a small-flash part the ``storage``
  and ``modstore`` writable partitions may no longer fit (see :doc:`production`).

Related: :doc:`data_model` (points), :doc:`rules` (authoring),
:doc:`production` (partitions and flash), :doc:`api/host_rest` (request bodies).
