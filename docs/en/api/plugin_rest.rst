Store plugin REST API
=====================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

HTTP JSON API for developing, debugging, installing, and operating L1/L2 Store
modules; HTTP entry for module RPC.

Implementation: ``components/minihub_web`` + ``minihub_store`` + ``minihub_module``.
Requires ``CONFIG_MINIHUB_STORE`` and ``CONFIG_MINIHUB_DYNAMIC_MODULES``.

.. only:: not hide_l3

   The same API covers L3 native modules. ``CONFIG_MINIHUB_DYNAMIC_MODULES`` is
   required to load them.

Host REST: :doc:`host_rest`.

Conventions
-----------

* Install/upgrade/uninstall does not support multipart ``.mhub`` upload — run
  ``POST /api/store/refresh`` then install from catalog URL.
* ``type`` must be ``module`` (L2) or ``pack`` (L1).

.. only:: not hide_l3

   ``type: module`` also covers L3.

* Default ``reload: true`` triggers Hub soft reload — :doc:`../architecture`.


GET /api/modules
----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules``

List installed Store modules and load state.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON array with ``id``, ``name``, ``version``, ``enabled``,
     ``loaded``, ``runtime``, ``adapter``, etc.

   ``runtime`` is one of:

   * ``js`` — L2 QuickJS (``main.js``)

   The field is omitted if Host cannot classify the slot. L1 packs are listed
   by ``GET /api/packs``, not as ``runtime: pack``.

.. only:: not hide_l3

   ``runtime`` may also be ``native`` (L3 ``.so``) or ``hybrid`` (both native
   and ``main.js``; optional ``entry`` ``js`` or ``native``).

POST /api/modules
-----------------

.. container:: mh-api-block mh-api-command

   ``POST /api/modules``

Enable/disable, delete a module, or clear load_guard.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``id`` (string, required)
       Module id.

   ``enabled`` (bool)
       Enable or disable; exactly one of ``enabled``, ``delete``,
       ``clear_load_guard`` must be set.

   ``delete`` (bool)
       ``true`` removes ``/store/modules/ID/``.

   ``clear_load_guard`` (bool)
       Clear ``.load_guard`` so the next load is retried.

   ``reload`` (bool, optional)
       Default true; soft-reload Hub after success.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok","id":"…","reloaded":true}``
   * **HTTP 400** — bad body
   * **HTTP 500** — module API failure

POST /api/modules/ID/rpc
------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/modules/ID/rpc``

Invoke ``minihub_module_rpc`` when implemented.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``method`` (string, required)
       RPC method name (module-defined).

   ``params`` (object, optional)
       JSON arguments.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — module-specific JSON
   * **HTTP 404** — unknown ``method``
   * **HTTP 400/500** — bad args or internal failure

.. container:: mh-api-block mh-api-attention

   **Attention**

   MCP ``hub.api`` proxies only allowlisted ``method`` values (:doc:`mcp`). L2 JS modules
   register RPC in ``setup`` when they need it.

GET /api/modules/ID/preview
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules/ID/preview``

Optional module preview (e.g. camera frame); response type varies.

GET /api/packs · POST /api/packs
--------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/packs · POST /api/packs``

GET lists installed L1 packs. POST enable/disable/delete (like ``POST /api/modules``
without ``clear_load_guard``).

GET /api/store/catalog
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/catalog``

Current catalog entries from configured Store URL.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON array with ``type``, ``id``, ``version``, ``url``, etc.

POST /api/store/refresh
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/refresh``

Re-download ``catalog.json`` from configured Store URL.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — URL unreachable or catalog missing

POST /api/store/install
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/install``

Download ``.mhub`` from catalog and unpack under ``/store/modules/ID/`` or L1 pack dir.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``type`` (string, required)
       ``module`` or ``pack``.

   ``id`` (string, required)
       Catalog id.

   ``reload`` (bool, optional)
       Default true.

   ``upgrade`` (bool, optional)
       Upgrade path instead of fresh install.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — missing fields
   * **HTTP 404** — missing artifact
   * **HTTP 409** — already installed
   * **HTTP 500** — download/unpack/ABI failure

POST /api/store/upgrade · POST /api/store/uninstall
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/upgrade · POST /api/store/uninstall``

upgrade replaces with catalog version; uninstall removes entry. Body: ``type`` + ``id``
required; optional ``reload`` (default true).

GET / POST /api/store/config
----------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/store/config``

GET returns ``custom_url``, ``disable_default``, effective Store base. POST writes NVS.

.. container:: mh-api-block mh-api-parameters

   **Parameters (POST JSON body)**

   ``custom_url`` — Store root URL; empty clears override

   ``disable_default`` — skip built-in default Store URL when true

Development and production
--------------------------

* Pack workflow: :doc:`../plugins/pack_and_catalog`
* Factory preinstall: :doc:`../production`

.. only:: not hide_l3

   * ABI mismatch on install: :doc:`module_abi`

