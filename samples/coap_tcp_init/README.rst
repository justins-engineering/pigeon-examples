coap_tcp_init
=============

Polls the device shadow over CoAP on a TLS/TCP stream (RFC 8323,
``coaps+tcp://``), applies its ``target_config``, reports the result back and
sends telemetry. A pre-shared key is the device's whole authentication, so no
bearer token is compiled in. Start from this sample where a stream socket is
the better fit: a device behind a middlebox that drops UDP, or one already
holding a TCP connection open for other reasons.

Every exchange opens its own TLS session, so a poll costs three handshakes.
A battery device that sleeps between reports belongs on ``coap_dtls_init``
instead, which keeps one DTLS session alive across them.

The lesson is in ``src/shadow.c``. Board bring-up lives in ``../common/net``,
behind ``net_connect()`` and ``net_disconnect()``, so ``main.c`` reads the
same on every board.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that
  has LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a
  build host with internet access.
- A pigeon whose connector is CoAP. Its endpoint and PSK come back once, from
  the create or token-refresh response. The platform mints the endpoint in
  its ``coaps://`` form, which is the DTLS/UDP transport; this build wants the
  same authority with the ``coaps+tcp://`` scheme.
- A signing key for the Feather builds, which boot through MCUboot. Generate
  one with ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the
  tree and export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build
  shell. Unset, the build warns and signs with a key anyone can forge.

Configure
---------

Write ``samples/coap_tcp_init/prj.local.conf``; it is git-ignored and merged
on top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="coaps+tcp://<coap-host>/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_COAP_TLS_PSK_IDENTITY="<psk-identity>"
  CONFIG_PIGEON_COAP_TLS_PSK_SECRET="<psk-secret>"

The values are compiled in, so a change here needs a rebuild. Refreshing a
pigeon's token mints a new PSK with it and retires the old one.

On the ESP32-C6 the WiFi credentials go in a second git-ignored file, in the
sample's own ``boards/`` directory, creating it if it is not there: write
``samples/coap_tcp_init/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops a WiFi password reaching the
build log of every board that has no WiFi; ``docs/esp32c6.md`` says why.

The port may be left off the endpoint, in which case 5684 is used.

Only production terminates CoAP; staging runs no terminator at all. To develop
against something local instead, point the build at libcoap's ``coap-server``:
``docs/coap-conformance.md`` has the commands.

The sample's own options:

- ``CONFIG_PIGEON_COAP_TRANSPORT_TCP`` (``prj.conf``): RFC 8323 framing on a
  TLS stream, with TCP owning reliability. The library checks the endpoint's
  scheme against it and refuses to start on a mismatch.
- ``CONFIG_PIGEON_COAP_SEC_TAG`` (46): the security tag the library registers
  the PSK under. Each sample pins its own, because the modem's credential
  store outlives a reflash.
- ``set(SAMPLE_TLS psk)`` in ``CMakeLists.txt``, before it includes
  ``../common/app.cmake``: pulls in ``../common/boards/tls-psk.conf``, the
  mbedTLS key exchange and PSA algorithms a pre-shared key needs, on the boards
  whose TLS runs in mbedTLS. The Feathers need none of it, because their modem
  runs the handshake and keeps the key in its own store.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool,
silences or restores logging), ``telemetry_interval`` (seconds between polls)
and ``reboot`` (one-shot).

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir::

  west build -p always -d build -b native_sim/native/64 samples/coap_tcp_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. The USB-JTAG port flashes,
the CP210x port is the console::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/coap_tcp_init
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/coap_tcp_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 115200

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard
CMSIS-DAP probe, whose CDC-ACM port is also the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/coap_tcp_init
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

What you should see
-------------------

A first poll that fetches a shadow, applies it and reports back, here on
native_sim against a shadow whose target sets ``log`` and a 15 second
interval::

  *** Booting Zephyr OS build v4.4.1 ***
  <inf> pigeon: Initializing Pigeon tracking instance: pigeon-coap-tcp-sample
  <inf> pigeon: Transport mapped to low-overhead CoAP edge pipeline: coaps+tcp://<host>/device/pigeons/<pigeon-id>
  <inf> pigeon: Pigeon tracking instance ready: pigeon-coap-tcp-sample
  <inf> pigeon: Queued telemetry: reset_cause=8
  <inf> net_connect: Bringing network interface up
  <inf> net_connect: Connecting to the network
  <inf> net_connect: Network connected
  <inf> shadow: Shadow fetched: target_version=3 current_version=0 updated_at=1789000000
  <inf> pigeon: Queued telemetry: uptime_s=1
  <inf> pigeon: Queued telemetry: poll_count=1
  <inf> pigeon: Flushed 3 telemetry key(s) in one report (51 bytes)
  <inf> shadow: Shadow v3: log false -> true
  <inf> shadow: Shadow v3: telemetry_interval 60 -> 15
  <inf> shadow: Applied shadow v3: log=true telemetry_interval=15
  <inf> shadow: Reported current_config back to platform at v3
  <inf> shadow: Next shadow poll in 15 s

A Feather writes the key into the modem before the interface comes up, and
logs ``Powering off modem`` before any reboot. Once the shadow has converged,
later polls log ``Shadow already converged at version N; nothing to apply``
instead of applying and reporting; that is expected.

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- ``No TLS credential found with tag 46``: the PSK keys are missing from
  ``prj.local.conf``. An empty value means "no key supplied", so the build
  succeeds and only the handshake fails.
- ``CONFIG_PIGEON_ENDPOINT scheme mismatch``: the endpoint is still in its
  ``coaps://`` form. Keep the authority and change the scheme.
- A handshake that never completes: the terminator and the device must share
  a ciphersuite. The two board fragments ask for AES-128-GCM and AES-128-CCM-8
  without pinning either.
- A Feather build that overflows its slot: the application already fills most
  of the non-secure partition, so another subsystem may not fit. Copy the
  partition overlay from ``../https_init/boards`` to take back the space TF-M
  leaves unused.
- native_sim logs one ``Network disconnected`` before ``Network connected``:
  the simulated interface reports its state before it has an address.

Next steps
----------

- ``coap_dtls_init``: the same connector on one long-lived DTLS session over
  UDP, which is the transport a battery or cellular device wants.
- ``https_init``: the polling reference, plus firmware updates and log upload.
- ``mqtt_init``: one persistent session to the platform broker, with the
  shadow pushed instead of polled.
