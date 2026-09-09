coap_dtls_init
==============

Polls the device shadow over CoAP-over-DTLS/UDP, applies its ``target_config``,
reports the result back, sends telemetry and uploads the device's own logs.
The whole session is authenticated by a pre-shared key: there is no
certificate to install and no bearer token to carry. Start from this sample
for a battery or cellular device, where one long-lived DTLS session and RFC
7252 confirmable exchanges cost far less than a request-per-poll over TLS.

The lesson is in ``src/shadow.c``. Board bring-up lives in ``../common/net``,
behind ``net_connect()`` and ``net_disconnect()``, so ``main.c`` reads the same
on every board.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that has
  LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a build
  host with internet access.
- A pigeon on the platform whose connector is CoAP. Its endpoint, PSK identity
  and PSK secret come back once, from the create or token-refresh response.
- A signing key for the Feather builds, which boot through MCUboot. Generate
  one with ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree
  and export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Without it MCUboot's public development key signs the image and the build
  warns; fine on a bench, never on a device that leaves it.

Configure
---------

Write ``samples/coap_dtls_init/prj.local.conf``; it is git-ignored and merged
on top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="coaps://coap.pidgeiot.com:5684/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_COAP_TLS_PSK_IDENTITY="<psk-identity>"
  CONFIG_PIGEON_COAP_TLS_PSK_SECRET="<psk-secret>"
  # ESP32-C6 only
  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

The values are compiled in, so a change here needs a rebuild. Refreshing a
pigeon's token mints a new PSK and revokes the previous one; rebuild with the
new values.

The scheme has to be ``coaps://``. The library checks it against the transport
this build was compiled with and refuses the endpoint rather than failing later
in a handshake; ``coaps+tcp://`` belongs to ``coap_tcp_init``.

The sample's own options, all in ``prj.conf`` unless noted:

- ``CONFIG_PIGEON_COAP_TRANSPORT_UDP``: selects DTLS/UDP over the TLS/TCP
  transport, and with it CoAP's own retransmission and duplicate detection.
- ``CONFIG_PIGEON_COAP_DTLS_CID``: on by default in the library. Offers RFC
  9146 Connection ID so a session survives a NAT rebind or a PSM wake without
  a re-handshake. Each handshake logs whether it was negotiated.
- ``CONFIG_PIGEON_COAP_SEC_TAG``: the security tag the PSK is provisioned
  under; the default of 1 needs no change.
- ``CONFIG_PIGEON_LOG_UPLOAD``: batches this device's log output and sends it
  to the platform, dictionary-encoded, as an RFC 7959 Block1 sequence.
  Decoding a chunk needs the ``log_dictionary.json`` of the exact build that
  produced it; one from another build decodes to plausible nonsense rather
  than erroring, so archive it with the image.
- Retransmission timing is Zephyr's own: ``CONFIG_COAP_INIT_ACK_TIMEOUT_MS``,
  ``CONFIG_COAP_ACK_RANDOM_PERCENT``, ``CONFIG_COAP_BACKOFF_PERCENT`` and
  ``CONFIG_COAP_MAX_RETRANSMIT``.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool, silences
or restores logging), ``telemetry_interval`` (seconds between polls) and
``reboot`` (one-shot).

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir::

  west build -p always -d build -b native_sim/native/64 samples/coap_dtls_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. No bootloader is built: this
sample stages no firmware, so the application image boots directly. The
USB-JTAG port flashes, the CP210x port is the console::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/coap_dtls_init
  west flash -d build
  pyserial-miniterm /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe.
``--sysbuild`` is required: it builds the MCUboot the Feathers boot through::

  west build -p always --sysbuild -d build -b circuitdojo_feather/nrf9160/ns samples/coap_dtls_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 115200

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard CMSIS-DAP
probe, whose CDC-ACM port is also the console::

  west build -p always --sysbuild -d build \
    -b circuitdojo_feather_nrf9151/nrf9151/ns samples/coap_dtls_init
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

What you should see
-------------------

A first poll against the platform, here on native_sim::

  *** Booting Zephyr OS build v4.4.1 ***
  <inf> pigeon: Initializing Pigeon tracking instance: pigeon-coap-dtls-sample
  <inf> pigeon: Transport mapped to low-overhead CoAP edge pipeline: coaps://<host>:5684/device/pigeons/<pigeon-id>
  <inf> pigeon: Pigeon tracking instance ready: pigeon-coap-dtls-sample
  <inf> pigeon: Queued telemetry: reset_cause=8
  <inf> net_connect: Bringing network interface up
  <inf> net_connect: Connecting to the network
  <inf> net_connect: Network connected
  <inf> pigeon: DTLS session up, CID status: uplink
  <inf> pigeon: DTLS ciphersuite: 0xc0a8
  <inf> shadow: Shadow fetched: target_version=3 current_version=3 updated_at=1786639870
  <inf> pigeon: Queued telemetry: uptime_s=2
  <inf> pigeon: Queued telemetry: poll_count=1
  <inf> pigeon: Flushed 3 telemetry key(s) in one report (51 bytes)
  <inf> shadow: Shadow already converged at version 3; nothing to apply
  <inf> shadow: Next shadow poll in 60 s

``0xc0a8`` is ``TLS_PSK_WITH_AES_128_CCM_8``, the constrained-device suite this
transport aims at. A shadow whose target is ahead of what the device runs logs
``Applied shadow vN`` and ``Reported current_config back to platform at vN``
instead of the converged line. A Feather adds ``Powering off modem`` before any
reboot.

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- ``CID status`` reported as unsupported: modem firmware older than
  mfw_nrf9160 v1.3.5 has no Connection ID, and the session runs without it.
- A handshake that fails before any request: the PSK was refreshed after this
  build, or the endpoint names a pigeon whose connector is not CoAP.
- A Feather logging ``Not allowed when LTE connection is active``: the modem's
  credential store only accepts writes while it is offline, which is why
  ``main.c`` provisions the PSK before ``net_connect()``.
- A Feather that will not attach for half an hour: the modem refuses to attach
  after repeated ungraceful resets. Let the device power the modem off (a
  shadow ``reboot`` does) instead of resetting it mid-attach.
- A Feather build that overflows its slot: both fill about 90% of the stock
  non-secure partition, which is why they build size-optimized.
- native_sim logs one ``Network disconnected`` before ``Network connected``:
  the simulated interface reports its state before it has an address.

Next steps
----------

- ``coap_tcp_init``: the same connector over TLS/TCP, for a network that will
  not carry UDP.
- ``https_init``: the reference polling device, with firmware updates through
  MCUboot.
- ``ws_init``: a push channel, for a device that should react to a config
  change without polling.
