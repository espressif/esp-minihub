JS plugin host API
==================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

A JS plugin is JavaScript running inside the Hub; its ``module.json`` marks it
with ``runtime: js``. It reaches Host services through QuickJS bindings, not
through a linker. Those bindings live in ``components/minihub_js/`` and are
compiled in when ``CONFIG_MINIHUB_JS_PLUGINS`` is set.

REST install/RPC: :doc:`plugin_rest`. C point/command model: :doc:`minihub_core`.

.. only:: not hide_l3

   ELF plugins use the vtable in :doc:`module_abi`.

The host object
---------------

What ``setup(host)`` receives, read out of the QuickJS bindings at build time, so a
method that was renamed or removed cannot stay on this page. The number after ``/``
is the argument count the C side registers; the table lists each namespace and the
methods under it.

.. minihub-JS-api::

Module entry
------------

Hub calls ``setup(host)`` once per enabled JS module on load and on each
``POST /api/hub/reload``. ``main.js`` must define ``setup``; otherwise the module is
skipped with a warning. Each module's top-level names stay in that module.

Use a distinct ``point_id`` prefix per module id in ``setup``; reload runs ``setup`` again.
Register **one point per device** (``module.device``), not one point per parameter.

Implementation
--------------

* ``components/minihub_js/minihub_js.c`` — core bindings (log, point, command,
  event, http, config, module, timer, adapter, time) and the timer globals.
* ``components/minihub_js/minihub_js_net.c`` / ``minihub_js_crypto.c`` — UDP and
  digest/HMAC/AES helpers.
* ``components/minihub_js/minihub_js_ble.c`` — the shared BLE advertisement scanner.
* ``components/minihub_js/minihub_js_mdns.c`` — one-shot LAN service discovery.
* ``components/minihub_js/minihub_js_mqtt.c`` — per-owner MQTT discovery clients.

Entry function
--------------

setup(host)
~~~~~~~~~~~

.. code-block:: javascript

   function setup(host) { ... }

Parameters
^^^^^^^^^^

``host`` (object)
    Host-injected capability object (see ``host.*`` methods below).

Returns
^^^^^^^

``undefined``.

API by source file
------------------

The reference below is split one page per C source file, mirroring the build.

.. toctree::
   :maxdepth: 1

   js_host_core
   js_host_net
   js_host_ble
   js_host_mdns
   js_host_mqtt
