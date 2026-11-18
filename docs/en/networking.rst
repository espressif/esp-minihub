Networking
==========

:link_to_translation:`zh_CN:[中文]`

Most Hub features need IP, so networking comes up early.
``components/minihub_net`` builds the uplinks on top of ESP-IDF's ESP-NETIF, the
Wi-Fi driver, and Ethernet on SoCs that have an EMAC. All three are IDF layers,
documented upstream:

* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_
* `ESP-NETIF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html>`_
* `Wi-Fi Driver <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi-driver/index.html>`_

Matter controller networking is covered in :doc:`matter`. C API details:
:doc:`api/net_api`.

Kconfig
-------

* ``CONFIG_MINIHUB_NET_WIFI_STA`` — Create ``WIFI_STA_DEF``, connect with credentials
* ``CONFIG_MINIHUB_NET_ETH`` — Native EMAC + PHY (S31 Function CoreBoard: YT8531)
* ``CONFIG_MINIHUB_NET_WIFI_SOFTAP`` — SoftAP/STA owned outside ``minihub_net``
* ``CONFIG_MINIHUB_WIFI_PROVISIONING`` — SoftAP provisioning when no NVS creds
* ``CONFIG_MINIHUB_MQTT_BROKER`` — On-device MQTT broker; runtime config decides
  whether it listens

Fragments: ``configs/sdkconfig/feature/wifi_sta.defaults``, ``eth.defaults``,
``mqtt_broker.defaults``.

Boot order
----------

.. image:: ../_static/net_boot/boot_flow.svg
   :align: center
   :alt: MiniHub network bring-up: minihub_net_init → minihub_net_start_wifi → STA connect → default route → console on port 80

`Interactive version <_static/net_boot/index.html>`_ — five steps in boot order;
the cyan return line is the path taken when no credentials are stored yet.

1. ``minihub_net_init()`` — netif/event loop; start Ethernet; **prepare** Wi-Fi handlers only (log: ``wifi STA deferred``).
2. ``minihub_adapter_start_all()`` — radio mutex, then adapters in order
   Wi-Fi → Zigbee or Thread → BLE Mesh → others. The ``wifi`` adapter calls
   ``minihub_net_start_wifi()`` (``esp_wifi_start`` + connect, or provisioning).

Default route vs STA connect
----------------------------

**Having Ethernet IPv4 does not skip Wi-Fi association.**

* **STA credentials present** (Kconfig ``CONFIG_MINIHUB_WIFI_SSID`` or NVS) —
  ``minihub_net_start_wifi()`` runs ``esp_wifi_start`` and ``connect``; Ethernet
  IPv4 does not skip that. No credentials → provisioning (or fail). No
  ``connect`` while provisioning is busy
* **Default route** — ``minihub_net_refresh_default_netif()``: Ethernet if it has
  IPv4, else Wi-Fi if it has IPv4, else Ethernet (if built) or Wi-Fi
* **Ethernet link down** — log ``Ethernet link down``, then refresh the default
  netif; still Ethernet-first if that netif still has IPv4

Credentials and provisioning
----------------------------

Wi-Fi SSID/PSK live in NVS (namespace ``minihub_wifi``), loaded by
``minihub_provision_load_credentials()``. NVS wins over
``CONFIG_MINIHUB_WIFI_SSID`` / ``CONFIG_MINIHUB_WIFI_PASS``, and the literal
placeholder ``YOUR_SSID`` counts as unset.

When **no credentials** exist and ``CONFIG_MINIHUB_WIFI_PROVISIONING`` is on, the
Hub brings up a plain open AP — SSID ``PROV_MiniHub``, no password, channel 1, 4
clients — and Wi-Fi is then configured from the Web console on that AP with
``POST /api/net/wifi``. ``minihub_provision_start()`` is deliberately *not*
called: the ESP-IDF provisioning manager serves its own page on port 80, which is
the console's port. There is no Bluetooth path to Wi-Fi provisioning — a BLE
transport was removed, so ``CONFIG_BT_ENABLED`` is not required to configure a
device, and provisioning needs no pairing tooling beyond the browser.

The manager entry point stays available for a product that moves the console to
another port; ``CONFIG_MINIHUB_WIFI_PROVISIONING=n`` drops the open AP too, and a
device with no credentials then only logs
``no Wi-Fi credentials and provisioning disabled``.

MQTT broker
-----------

``components/minihub_mqtt_broker`` runs an
`mosquitto <https://components.espressif.com/components/espressif/mosquitto>`_
broker on the Hub, so the MQTT devices at a site connect to the Hub instead of a
separate always-on machine. It is a transport service only: it registers no
points. Turning it on also starts the Hub's own MQTT client on ``127.0.0.1`` —
built in the same ``minihub_mqtt_broker`` component (``mqtt_adapter.c``),
registered under the same ``mqtt_broker`` name — because that client is what
carries ``host.mqtt`` for JS plugins. There is no external-broker mode: with
the broker off, no client is created.

The firmware carries the broker whenever ``CONFIG_MINIHUB_MQTT_BROKER`` is on,
which is the default. Whether it listens is decided at run time by
``adapters.mqtt_broker`` (:ref:`adapters-JSON`), and the factory seed has it off:

.. code-block:: JSON

   "mqtt_broker": {
     "enabled": false,
     "port": 1883,
     "allow_anonymous": true,
     "users": [],
     "connect_rate_limit": 16
   }

* ``enabled`` — ``false`` in the factory seed: no listener task, no open port,
  and no loopback client
* ``port`` — TCP listener, 1–65535, default 1883
* ``allow_anonymous`` — default ``true``, so a client that sends no username is
  accepted. A client that does send a username is always checked against
  ``users``, and an unknown one is refused. Once it is ``false`` the Hub's own
  client logs in as ``users[0]``, so an empty ``users[]`` leaves ``host.mqtt``
  disconnected and logs a warning
* ``users`` — at most 4 ``{"name": …, "pass": …}`` entries, each field at most 31
  bytes; extras are dropped with a warning
* ``connect_rate_limit`` — CONNECT packets accepted per 10 s window, default 16.
  There is no cap on simultaneous clients

The listener binds ``0.0.0.0``, so it answers on Ethernet, on the Wi-Fi STA
address, and on the provisioning AP (``192.168.4.1``). With the default posture —
anonymous allowed, no per-topic **access control list (ACL)** — any device that can reach the Hub can
publish and subscribe to every topic. Set ``allow_anonymous`` to ``false`` and
list users before a unit ships; the network assumptions are in :doc:`security`.

A reload restarts the broker only when the ``mqtt_broker`` subtree changed, so
editing another adapter leaves MQTT clients connected. A restart does disconnect
them. Retained messages and offline sessions live in RAM only and are gone after
a reboot or a restart.

Not in this version: TLS, WebSocket, bridging, persistence, per-topic ACL, and
more than one listener. ``CONFIG_MOSQ_ENABLE_SYS`` (off by default) is the debug
switch that publishes the broker's own ``$SYS`` counters to any MQTT client.

REST: network configuration
---------------------------

``POST /api/net/config`` applies static or DHCP settings per interface.

**Request body**

.. code-block:: JSON

   {
     "iface": "eth",
     "dhcp": false,
     "ip": "192.168.1.50",
     "netmask": "255.255.255.0",
     "gw": "192.168.1.1",
     "dns": ["8.8.8.8", "1.1.1.1"]
   }

Or use ``dns_main`` / ``dns_backup`` instead of the ``dns`` array.

* ``iface`` — ``"eth"`` or ``"wifi"`` (required)
* ``dhcp`` — boolean (required)
* ``ip``, ``netmask``, ``gw`` — required when ``dhcp`` is false

**Response**

.. code-block:: JSON

   { "result": "ok" }

On failure: ``{ "result": "fail", "error": "ESP_ERR_..." }``.

Example (curl):

.. code-block:: bash

   curl -s -X POST http://HUB/api/net/config \
     -H 'Content-Type: application/json' \
     -d '{"iface":"wifi","dhcp":true}'

Expected logs
-------------

* ``Ethernet started`` — EMAC started (link may still be down)
* ``Ethernet link up, MAC …`` — physical link is up
* ``eth got IP`` / ``wifi got IP`` — that uplink has IPv4
* ``MiniHub ready (net=… prov=… web_ui=… lcd_ui=… eth=… wifi=…)`` — features
  prepared, not proof of IPv4
* ``listening on 0.0.0.0:1883, anonymous allowed, 0 user(s)`` — the MQTT broker
  task is up (tag ``mqtt_broker``)

Troubleshooting
---------------

**Ethernet is the default route but Wi-Fi has no IP**

Check that NVS has STA credentials, ``adapters.wifi.enabled`` is true, and
the ``wifi`` adapter ran ``minihub_net_start_wifi()``. Default-route selection
does not skip STA connect.

**STA never connects**

* Confirm provisioning finished (Web UI or SoftAP flow)
* Look for ``wifi got IP``; if missing, check SSID/PSK in NVS
* Ensure Matter/CHIP is not owning a second STA (see :doc:`matter`)

**Both interfaces up — which route?**

Traffic uses Ethernet when it has IPv4; otherwise Wi-Fi. Unplugging Ethernet
should fall back once Wi-Fi has IP (``minihub_net_refresh_default_netif``).

**REST net config returns fail**

Static IP fields must be valid when ``dhcp`` is false. Body max ~2 KB.

**MQTT device cannot reach the broker**

* ``adapters.mqtt_broker.enabled`` must be ``true``, then reload from the Web
  console or ``POST /api/hub/reload``
* ``listening on …`` confirms the bind. ``mosq_broker_run returned …`` means the
  broker exited and is retrying every 5 s — check whether another service holds
  the port
* A client that sends a username is refused unless that username is in
  ``users``, even while ``allow_anonymous`` is ``true``
* A device in a reconnect loop can exhaust ``connect_rate_limit`` and then be
  refused for the rest of that 10 s window

C API
-----

Declared in ``components/minihub_net/include/minihub_net.h``:

* ``minihub_net_init`` / ``minihub_net_start_wifi``
* ``minihub_net_has_ip`` / ``minihub_net_get_ip`` / ``minihub_net_get_netif``
* ``minihub_net_refresh_default_netif`` — pick default netif from IPv4 uplinks
* ``minihub_net_dns_probe`` (host-exported for Store modules)

Interface DHCP / static IP lives in ``minihub_net_cfg.h``:

* ``minihub_net_set_iface_config`` — used by ``POST /api/net/config``
* ``minihub_net_get_iface_status`` · ``minihub_net_apply_saved_configs``

See :doc:`api/net_api` for struct fields and return codes.
