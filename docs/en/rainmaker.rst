RainMaker
=========

:link_to_translation:`zh_CN:[中文]`

The optional ESP RainMaker bridge (:doc:`source_tree` —
``components/minihub_rainmaker``) is off by default. Enable it with
``CONFIG_MINIHUB_RAINMAKER``. The hub then registers itself as a RainMaker node
of type ``Gateway`` and bridges its :doc:`data_model` points into RainMaker
devices, so a phone running the ESP RainMaker app can see and control them.

Account binding (self-test only)
--------------------------------

.. warning::
   The on-network binding path below is provided for self-testing and evaluation
   only. If you intend to ship a product that uses RainMaker, contact
   sales@espressif.com to arrange proper node provisioning, certificates, and the
   manufacturing flow. Do not rely on the self-test claim path in production.

1. Enable ``CONFIG_MINIHUB_RAINMAKER`` and
   ``CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE``.
2. Build and flash the Host, then connect the hub to Wi-Fi (see
   :doc:`networking`). Internet access is required for the one-time cloud claim.
3. The hub logs ``RainMaker Node ID: <id>`` at boot. After it gets an IP and the
   node claims itself, it logs ``On-Network user mapping READY`` and advertises
   over the mDNS service ``_esp_rmaker_chal_resp._tcp`` on port ``8081`` (not
   ``80`` — the MiniHub Web console uses ``80``). No proof-of-possession (PoP) is
   required.
4. Open the ESP RainMaker phone app and add a device through the **On-Network**
   (local network) method. The app discovers the hub by mDNS and maps the node to
   your RainMaker account.
5. After mapping, the hub and its bridged sub-devices appear under your RainMaker
   account.
