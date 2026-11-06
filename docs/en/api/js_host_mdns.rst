JS plugin mDNS host API
=======================

:link_to_translation:`zh_CN:[中文]`

This namespace lives in ``components/minihub_js/minihub_js_mdns.c`` over
``espressif/mdns``. The table is read from the C bindings at build time.

.. minihub-JS-api:: mdns

See :doc:`js_host` for the entry point.

Methods — host.mdns
-------------------

One-shot LAN service discovery. Enable ``CONFIG_MINIHUB_JS_MDNS``; when off the
method throws ``host.mdns disabled``. Implementation:
``components/minihub_js/minihub_js_mdns.c`` over ``espressif/mdns``.

host.mdns.resolve(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~

``opts``: ``{ service: "_googlecast", proto: "_tcp", timeout_ms: 2000,
max_results: 8 }``. ``timeout_ms`` is clamped to 100–10000, ``max_results`` to
1–16. The call blocks the JS worker for the query duration, so use it in a
``host.timer`` callback or an explicit sync RPC, not in the 5 s poll.

Returns an array (empty when nothing answers) of::

  { instance: "Living Room", service: "_googlecast", hostname: "chromecast.local",
    port: 8009, ip: "192.168.1.23", txt: { md: "Google Home" } }

mDNS is initialised lazily and idempotently, so a Matter/Thread responder that
already owns the service is left untouched. Results are freed by the binding.
