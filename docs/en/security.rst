Security Model
==============

:link_to_translation:`zh_CN:[中文]`

This chapter says what the Host protects, what it assumes about your network, and
what an OEM has to add before a unit ships. It is a description of shipped
behavior, not a promise: the public build is written for a trusted LAN.

What is on by default
---------------------

* **One administrator.** ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on, so every mutating
  API call and every console page except the login screen needs a session. The
  username is fixed (``admin``, ``MINIHUB_WEB_USERNAME``) and the password is set on
  the device: 6–64 characters, stored in the NVS namespace ``minihub``
  (:file:`components/minihub_auth/minihub_auth.c:260,302`).
* **Sessions are short.** An idle session dies after 300 s
  (``MINIHUB_AUTH_SESS_SEC``) and never outlives 8 h
  (``MINIHUB_AUTH_SESS_ABS_SEC``). There is one admin, no roles, and no per-user
  history: build accountability into your cloud or plugin, not into the Hub.
* **An optional Bearer token** exists for automation, and it is a shipped default:
  ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` comes up as ``espressif`` and wins over the
  NVS ``api_token`` whenever it is non-empty. Leaving it as-is means every unit of
  that product answers to the same token. Set it per product, or clear it and rely on
  the cookie.

What is public by design
------------------------

These answer without a session, and a reader auditing a device should expect them to
be open: ``/``, ``/login``, ``/web/*``, ``/images/*``, ``/modules/*``,
``/favicon.ico``, ``GET /api/``, and ``/api/auth/status · setup · login · logout``.
The console HTML, device icons and any module's own web UI are therefore readable by
anyone who can reach the device; authentication protects *actions* and configuration,
not the static shell. Do not put secrets in a module's web assets or in a device
icon file.

No transport encryption
-----------------------

The Host serves plain HTTP and has no TLS listener, so credentials, the Bearer token
and configuration cross the LAN in clear text. Treat the network as the control plane:
its own VLAN or SSID, no guest access, and reachability from your services through
something you terminate elsewhere (a VPN or a reverse proxy). A Hub is not
safe on a network where strangers can sit.

The same applies to what the Hub pulls. Firmware and Store packages are fetched from
the URL you configure and are validated **structurally** — size and image checksum —
not authenticated: nothing verifies who signed the image. Whoever controls that URL
controls what gets written to the flash. Use an HTTPS URL that is not merely
convenient, pin the deploy host in your DNS, or enable secure boot and flash
encryption (see below) so a written image still has to be signed to run.

Where secrets live
------------------

* NVS namespace ``minihub`` — the console password and the optional API token.
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — empty by default, which is the state a
  deployment should stay in: REST then accepts a session cookie or the NVS ``api_token``
  (namespace ``minihub``), so the token is one secret per device. Setting a value here is a
  deliberate decision, because a compiled-in string beats NVS and is then shared by every
  unit built from that recipe — the reason this default was left empty rather than filled
  with a placeholder. Login stays required either way; the token is an alternative way to
  satisfy it, not a way around it.
* ``CONFIG_MINIHUB_WIFI_SSID`` / ``CONFIG_MINIHUB_WIFI_PASS`` — Wi-Fi credentials set here
  are compiled into the app binary, so they are readable from the flash image of any unit
  unless flash encryption is on. Leave the default ``YOUR_SSID`` and provision through NVS
  or the SoftAP flow instead.
* ``/store/modules/<id>/`` — a module's own account material. Vendor cloud tokens
  belong here or in module-private NVS keys, never in Host Core ``configs/``:
  anything under ``configs/`` is compiled and flashed into every unit of the build
  (:doc:`host_customization`).
* ``/data/config/*.json`` — adapters, rules, scenes, as plain text on LittleFS.
  Anything in these files is readable by a session, and by anyone with the device in
  hand and a serial port. ``GET /api/config/adapters`` returns the whole document.

Physical access and a factory reset
-----------------------------------

Two windows are open by design:

* **Factory reset clears the password.** Holding the configured GPIO (or
  ``POST /api/system/reset`` with ``{"confirm": true}``) erases NVS, and
  ``POST /api/auth/setup`` is unauthenticated so a fresh device can be configured —
  the first person to open the console after a reset becomes the owner. The reset
  also drops rules / scenes / adapters and every Store-installed module
  (:doc:`production`).
* **Provisioning runs an open access point.** With no stored Wi-Fi credentials the
  Host brings up ``PROV_MiniHub`` (channel 1, no password) and serves the console on
  it, so anyone in radio range can set the credentials and the password. Configure
  devices where the radio is yours.

Serial is full control: logs, and — while the console is enabled — the routes that
replace the console and the catalog. ``PUT /api/dev/web/``,
``PUT/DELETE /api/dev/assets/`` and ``PUT/DELETE /api/dev/catalog/`` are built
whenever ``CONFIG_MINIHUB_WEB_UI`` is on, and ``PUT /api/dev/fs/*`` whenever
``CONFIG_MINIHUB_DEV_FS_UPLOAD`` is on (the default). They need a session, but they
let that session write files on the device, so a leaked token is also a file write.
Turn these off in a production build and do not rely on them in the field.

Not provided by the public build
--------------------------------

Decide these yourself; none of them is a switch you can turn on later:

* **Secure boot, flash encryption and NVS encryption are off**, and the public
  ``minihub`` sdkconfig sets no eFuse, serial-number or signing policy
  (:doc:`production`). Enabling them changes the provisioning flow for signing keys
  and, with the wrong key, permanently bricks a unit — validate on your own
  build first, following the ESP-IDF security guide for this chip.
* **No debug lock policy.** Whether USB serial and JTAG stay open is yours to decide
  with eFuses; the guide sets nothing.
* **No hardware key storage, attestation, or rotation** — credentials are on the
  device in software-readable form.
* **No audit log.** A Hub records what it is doing, not who told it to.
* **No vulnerability-reporting channel** is defined for the open-source Host. An OEM
  shipping product should publish one before it ships, and record where the reports
  go.
* **No radio certification, and no RF design input.** This repository ships firmware:
  there are no test reports, no antenna or matching guidance, and no CE / FCC / SRRC /
  TELEC module certificates, so nothing here is evidence that a device built from it
  complies with anything. Country and channel plan, transmit power and coexistence
  behavior are set through ESP-IDF's Wi-Fi driver — MiniHub never overrides them — and
  the resulting certification work belongs to your hardware team and the target market.

Related: :doc:`web_console` (login and sessions), :doc:`production` (flash, eFuse
territory, SBOM and CVE scanning), :doc:`host_customization` (product configs),
:doc:`api/host_rest` (per-endpoint authentication), :doc:`versions`.
