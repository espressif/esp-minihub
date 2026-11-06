Store plugin REST API
=====================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

HTTP JSON API for developing, debugging, installing, and operating JSON/JS Store
modules; HTTP entry for module RPC.

Implementation: ``components/minihub_web`` + ``minihub_store`` + ``minihub_module``.
Requires ``CONFIG_MINIHUB_STORE`` and ``CONFIG_MINIHUB_DYNAMIC_MODULES``.

.. only:: not hide_l3

   The same API covers ELF native modules. ``CONFIG_MINIHUB_DYNAMIC_MODULES`` is
   required to load them.

Host REST: :doc:`host_rest`. Status codes and error bodies: :doc:`errors`.

Conventions
-----------

* There is **no** upload endpoint for a ``.mhub``: run ``POST /api/store/refresh``
  so the catalog is current, then install by ``id`` and the Hub downloads the
  ``url`` from the catalog.
* ``type`` must be ``module`` (JS) or ``pack`` (JSON).

.. only:: not hide_l3

   ``type: module`` also covers ELF.

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

   * ``JS`` — JS QuickJS (``main.js``)

   The field is omitted if Host cannot classify the slot. JSON packs are listed
   by ``GET /api/packs``, not as ``runtime: pack``.

.. only:: not hide_l3

   ``runtime`` may also be ``native`` (ELF ``.so``) or ``hybrid`` (both native
   and ``main.js``; optional ``entry`` ``JS`` or ``native``).

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
       Enable or disable. Set one of ``enabled``, ``delete``,
       ``clear_load_guard``; if several are present, ``delete`` wins over
       ``enabled``, which wins over ``clear_load_guard``.

   ``delete`` (bool)
       ``true`` removes the package directory ``/store/modules/ID/`` (binary,
       ``module.json``, ``ui.json``, ``.load_guard``) **and** the module's slot in
       ``/data/config/adapters.json``. Module-private NVS keys (accounts, tokens)
       are not touched.

   ``clear_load_guard`` (bool)
       Delete ``.load_guard`` so the next load is attempted. Only a file left at
       ``loading`` blocks auto-load — that means the previous boot crashed inside
       the module entry; ``ok`` never blocks.

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

Call the RPC handler the module registered through
``host.module.registerRpc`` (:doc:`js_host`).

.. only:: not hide_l3

   Native modules export ``minihub_module_rpc`` instead.

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

   ``params`` must be a JSON object, and the body is capped at 16 KiB. The call
   runs on the JS worker off the HTTP task, so a slow RPC does not stall other
   API requests. MCP ``hub.api`` proxies only allowlisted ``method`` values
   (:doc:`mcp`). A JS module registers RPC with
   ``host.module.registerRpc`` in ``setup`` when it needs one.

GET /api/modules/ID/preview
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules/ID/preview``

Optional module preview (e.g. camera frame); response type varies.

GET /api/packs · POST /api/packs
--------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/packs · POST /api/packs``

GET lists installed JSON packs. POST enable/disable/delete (like ``POST /api/modules``
without ``clear_load_guard``).

GET /api/store/catalog
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/catalog``

Current catalog entries from configured Store URL.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON array with ``type``, ``id``, ``version``, ``url``, and the
     catalog fields, **plus** what the Hub merges in per entry: ``installed``,
     ``installed_version``, ``upgrade_available``.

POST /api/store/refresh
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/refresh``

Re-download ``catalog.json`` from the configured Store URL. The fetch runs off
the HTTP task, so the call returns before it finishes.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"pending"}``, also when a refresh is already
     running (the call is not queued twice)
   * **HTTP 500** — the background task could not be started

The fetch itself has a 5 s timeout; a catalog larger than 64 KiB or invalid JSON
is reported as ``error`` by the status endpoint.

GET /api/store/refresh
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/refresh``

Poll the refresh started by POST.

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``result`` is ``pending``, ``idle``, ``ok``, ``aborted``
     (the Store URL changed mid-fetch), or ``error`` with an ``error`` string

POST /api/store/install
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/install``

Download the catalog ``.mhub`` and unpack it. A JS module is staged at
``/store/modules/ID.staging/`` and renamed to ``/store/modules/ID/``, so a
half-extracted package is never visible. A JSON pack has no package directory:
only its manifest JSON is kept, at ``/data/config/packs/ID.json``.

.. container:: mh-api-block mh-api-parameters

   **Parameters (JSON body)**

   ``type`` (string, required)
       ``module`` or ``pack``.

   ``id`` (string, required)
       Catalog id.

   ``reload`` (bool, optional)
       Default true.

   ``upgrade`` (bool, optional)
       ``true`` routes to the upgrade path, which re-checks the version gate
       (see the ``409`` line below).

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — missing ``type`` / ``id``, or bad body
   * **HTTP 404** — ``id`` not in the catalog, artifact missing, or (with
     ``upgrade: true``) nothing installed under that id yet
   * **HTTP 409** — already installed without ``upgrade``, or the upgrade found a
     catalog version that is not newer
   * **HTTP 500** — download, unpack, size, or ABI failure

.. container:: mh-api-block mh-api-attention

   **Attention**

   Integrity is a byte count, not a digest: the downloaded length must equal the
   catalog ``size``, and ``sha256`` in a catalog is ignored by the device. There
   is no retry — one failed fetch is a 500. Size limits: the package must fit
   ``CONFIG_MINIHUB_HUB_MAX_BYTES`` (default 1 MiB, 64 KiB–4 MiB), at most 64 zip
   entries of 2 MiB each, and the ``id`` must be shorter than 48 characters.
   Version comparison is numeric on up to three dot-parts with a plain string
   tie-break, so ``1.0.0-rc1`` vs ``1.0.0`` is decided lexicographically; an
   installed version longer than 31 characters is truncated and compares wrong.

POST /api/store/upgrade · POST /api/store/uninstall
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/upgrade · POST /api/store/uninstall``

``upgrade`` replaces the installed copy only when the catalog version is newer,
or when ``binary_size`` / ``adapter_version`` differ. A downgrade is not
accepted here — use ``reinstall`` to push the same or an older catalog build.
``uninstall`` removes the package and its adapter slot; asking to uninstall
something that is not installed is not an error, so the only failure status is
**500**. Body for both: ``type`` + ``id`` required, optional ``reload``
(default true).

.. warning::

   The ``adapters.json`` entry that module owned is deleted with it, including your
   edits to that entry. Reinstalling the same package does not bring those edits back.

POST /api/store/reinstall
-------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/reinstall``

Download the catalog entry again and unpack it over the installed copy, keeping
the adapter slot (account and config) and the enabled state. The catalog version
may be unchanged. Body: ``type`` + ``id`` required; optional ``reload``
(default true).

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — module or pack not installed
   * **HTTP 500** — download or unpack failure

GET / POST /api/store/config
----------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/store/config``

GET returns ``custom_url``, ``disable_default``, and ``default_url`` (the
built-in address compiled from Kconfig). The effective base is the custom URL
when set, otherwise the default unless ``disable_default`` is true. POST writes
both values to NVS, aborts any catalog refresh in flight, and rejects a URL that
is not ``http(s)`` or leaves no store configured at all (``disable_default``
with an empty ``custom_url`` is a **400**).

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

