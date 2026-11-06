Error Reference
===============

:link_to_translation:`zh_CN:[中文]`

What the Hub can answer when something goes wrong, read out of the firmware at build
time. The lists live in ``docs/_extensions/minihub_errors.py``: it scans every file that
reports an HTTP error and both MCP sources, so a new error joins these tables by itself.
When the code starts returning something the guide cannot name — or keeps a row for
something it no longer returns — the docs build stops instead of publishing a stale list.

Open this page from the other end: you have a status, a body, or a ``code`` and want to
know who can send it. To follow a symptom through the stack instead, use
:doc:`../troubleshooting`.

HTTP statuses
-------------

A failed REST call carries no JSON. ``httpd_resp_send_err()`` answers ``text/plain``, and
the body is the shortest thing the handler had to say about the failure.

.. minihub-errors:: http

Three of those codes carry behavior the status line alone does not:

* A **401** also clears the ``mh_sess`` cookie in the response, so a console that has
  been left open falls back to the login screen instead of retrying with a dead session.
  Its body is always ``unauthorized``; changing a password with the wrong old one is the
  one case that answers **401** with ``wrong old password`` instead.
* A **500** body is either ``oom`` / ``no mem`` (an allocation failed) or the
  ``esp_err`` name the callee returned, so ``ESP_ERR_TIMEOUT`` on the wire means a real
  timeout inside the Hub, not a generic failure.
* **409** is reserved for a slot that is already taken: one OTA at a time, one initial
  password.

The HTTP server itself answers some requests before any MiniHub handler sees them, and
those codes are not in the table because no MiniHub source produces them: **404** for a
URI that was never registered, **405** for a URI registered under another method,
**414** for a URI longer than the server's buffer, **408** for a connection that opened
and never sent a request, **505** for an HTTP version other than 1.0/1.1, and **501** for
a body it cannot size — the server does not accept ``Transfer-Encoding``, so a client that
streams a body (``curl -T -``, some HTTP libraries' default for a file handle) is refused
before the payload is read. Send bodies with a known length.

Error bodies
------------

Every string the firmware writes literally into an error response, with the endpoints
that can return it. This is the table to grep when the only thing you have is what curl
printed.

.. minihub-errors:: messages

* ``invalid JSON`` and ``empty body`` are the two most common, and both
  mean the request never got as far as the operation. ``empty body`` is literal: the
  handler read zero bytes. ``invalid JSON`` has two causes — the body is not JSON, or it
  is longer than the buffer that handler happened to reserve. Endpoints that read into a
  fixed array (``/api/store/install`` takes 255 bytes) cut the body and then fail to parse
  it; endpoints that go through ``read_http_body()`` reject an oversized body up front with
  ``invalid body`` or ``body too large``. The header is never the problem: no handler
  looks at ``Content-Type``.
* ``missing <field>`` names the key the handler looked for and did not find. The field
  list per endpoint is in :doc:`host_rest` and :doc:`plugin_rest`.
* Where the last column gives a function name instead of a URI, the reply is sent from a
  worker or a shared helper (``preview_async_worker``, ``rules_import_apply_root``), so no
  single endpoint owns it.
* Bodies built at runtime from ``esp_err_to_name()`` cannot be listed here — they are any
  ``ESP_*`` name, and the status already tells you which table row to read.

MCP codes
---------

The on-device MCP server reports failure **inside a normal tool result**: the JSON-RPC
call succeeds and the returned string is ``{"ok":false,"error":"CODE"}``. A client that
only checks the protocol level therefore sees a green call and a failed operation.

.. minihub-errors:: mcp

The ``code`` comes from one of two helpers, and that decides how much the result says.
``hub.api`` builds its reply with ``err_obj()``, which adds a ``detail`` — the gateway's own
text, or a stage name like ``save_adapters`` — except where it passes ``NULL`` and you get
the bare code. Every other tool uses ``mcp_error()``, which returns the code alone, so the
code is the whole message:

* ``HUB_NOT_READY`` — a reload is in flight. Wait and retry; ``hub.reload`` is rate
  limited to once per 5 s, while REST ``POST /api/hub/reload`` is not.
* ``DENIED`` — the method or path is outside the ``hub.api`` allowlist, listed per rule in
  :doc:`mcp`.

``err_obj()`` also takes ``esp_err_to_name(err)`` as the code, so a gateway failure can
answer ``ESP_ERR_NOT_FOUND`` or ``ESP_FAIL`` where the table above names a class. Those are
not a second system: the row you want is the one whose gloss says the detail names it.

Related
-------

* :doc:`host_rest`, :doc:`plugin_rest` — the endpoints behind the status codes
* :doc:`mcp` — the tool list and the JSON-RPC envelope
* :doc:`../troubleshooting` — symptoms to codes, in order
* :doc:`../limits` — the capacity rows that ``oom`` and ``worker busy`` come from
