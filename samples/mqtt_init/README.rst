mqtt_init
=========

Holds one persistent MQTT session to the platform's broker, which bridges
every publish onto the platform's device routes. Telemetry, shadow reports
and log chunks go out as publishes, and the pigeon's target shadow arrives as
a retained message rather than being polled for, so a config change reaches
the device in about a second. Start from it for a device that should react to
the dashboard immediately and pay for one connection rather than one request
per poll.

The lesson is in ``src/shadow.c``. Board bring-up lives in ``../common/net``,
behind ``net_connect()``, ``net_disconnect()`` and ``net_install_ca()``, so
``main.c`` reads the same on every board.

A session authenticates one of two ways, chosen in the board's own fragment
under ``boards/``:

- TLS-PSK (``CONFIG_PIGEON_MQTT_AUTH_PSK``, the default on the Feathers and
  native_sim): identity is the pigeon id, key is the short secret minted
  alongside the bearer token. Nothing to provision, no certificate chain to
  verify and no clock to keep. The broker resolves this pigeon's token
  server-side, so no token rides the session.
- Server certificate (``CONFIG_PIGEON_MQTT_AUTH_CERT``, the default on the
  ESP32-C6): the broker serves a Let's Encrypt chain, the device verifies it
  against ``cert/isrg-root-x2.pem``, and the CONNECT password is
  ``CONFIG_PIGEON_TOKEN``. The shape every off-the-shelf MQTT client speaks.

Either board can run either mode. See Configure below.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that
  has LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a
  build host with internet access.
- A pigeon on the platform whose connector is MQTT. Its id, endpoint, bearer
  token and PSK secret come back once, from the create or token-refresh
  response.
- A signing key for the Feather builds, which include MCUboot. Generate one
  with ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree
  and export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Unset, the build warns and signs with a key anyone can forge.

Configure
---------

Write ``samples/mqtt_init/prj.local.conf``; it is git-ignored and merged on
top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="mqtts://mqtt.pidgeiot.com:8883"
  CONFIG_MQTT_INIT_PIGEON_ID="<64 hex chars>"
  # PSK mode
  CONFIG_PIGEON_MQTT_TLS_PSK_SECRET="<minted psk secret>"
  # certificate mode
  CONFIG_PIGEON_TOKEN="<device-bearer-token>"

The endpoint names the broker, not this pigeon: no path, unlike the HTTPS and
CoAP connectors' endpoints. The values are compiled in, so a change here needs
a rebuild, and refreshing a pigeon's token revokes both the previous token and
the previous PSK secret.

On the ESP32-C6 the WiFi credentials go in a second git-ignored file, in the
sample's own ``boards/`` directory, creating it if it is not there: write
``samples/mqtt_init/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops a WiFi password reaching the
build log of every board that has no WiFi; ``docs/esp32c6.md`` says why.

The sample's own options:

- ``CONFIG_MQTT_INIT_PIGEON_ID`` (``Kconfig``): the pigeon id. It is the
  CONNECT client id and username, and on a PSK session the handshake identity
  too; the broker refuses a session whose three copies of it disagree.
- ``CONFIG_PIGEON_MQTT_SEC_TAG`` (47): the security tag the session's
  credentials live under, whether the library registered the PSK there or
  ``main.c`` installed the CA. Each sample pins its own, because the modem's
  credential store outlives a reflash.
- ``CONFIG_MQTT_KEEPALIVE`` (``prj.conf``): 60 s here. A device that sleeps
  between reports wants it raised; the broker honours up to 30 minutes and
  closes the session after 1.5x that silence.
- ``CONFIG_PIGEON_MQTT_TELEMETRY_QOS1``: whether a telemetry publish is
  acknowledged. Shadow reports and log chunks are QoS 1 either way.
- ``CONFIG_PIGEON_LOG_UPLOAD`` (``prj.conf``): ships this device's logs to
  the platform as Zephyr dictionary-encoded binary. Decoding a chunk needs
  this build's own ``log_dictionary.json``, and one from another build decodes
  to plausible nonsense rather than erroring; ``docs/device-logs.md`` says
  where the file lands and how to read a chunk back.

``CMakeLists.txt`` declares both shapes, ``set(SAMPLE_TLS verify psk)``, so a
board whose TLS runs in mbedTLS compiles both want lists in and an overlay only
picks which one the session uses. On the ESP32-C6, TLS-PSK::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/mqtt_init \
    -- -DEXTRA_CONF_FILE=overlay-psk-native-tls.conf

On native_sim, a certificate verified against a broker's own development CA::

  west build -p always -d build -b native_sim/native/64 samples/mqtt_init \
    -- -DEXTRA_CONF_FILE=overlay-cert-native-tls.conf -DPIGEON_MQTT_CA_FILE=<ca pem>

A Feather needs neither overlay: the modem verifies the chain, so certificate
mode there is ``CONFIG_PIGEON_MQTT_AUTH_CERT=y`` in ``prj.local.conf`` and
nothing else.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool, silences
or restores logging), ``telemetry_interval`` (seconds between passes) and
``reboot`` (one-shot).

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir::

  west build -p always -d build -b native_sim/native/64 samples/mqtt_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. The USB-JTAG port flashes, the
CP210x port is the console::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/mqtt_init
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/mqtt_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 1000000

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard CMSIS-DAP
probe, whose CDC-ACM port is also the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/mqtt_init
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

``scripts/test/native-sim-e2e.sh`` runs this sample on native_sim against a
broker and a mock platform on the build host, with no account and no hardware:
``docs/mqtt-e2e.md`` has the commands.

What you should see
-------------------

A first session, here on native_sim against a local broker::

  *** Booting Zephyr OS build v4.4.1 ***
  <inf> pigeon: Initializing Pigeon tracking instance: <pigeon-id>
  <inf> pigeon: Transport mapped to MQTT broker session: mqtts://<host>:8883
  <inf> pigeon: Pigeon tracking instance ready: <pigeon-id>
  <inf> pigeon: Queued telemetry: reset_cause=8
  <inf> net_connect: Bringing network interface up
  <inf> net_connect: Connecting to the network
  <inf> net_connect: Network connected
  <inf> pigeon: MQTT TLS ciphersuite: 0xc0a8
  <inf> pigeon: MQTT session up: <host>:8883
  <inf> shadow: Shadow: target_version=1 current_version=0 updated_at=1788975263
  <inf> pigeon: Queued telemetry: uptime_s=1
  <inf> pigeon: Queued telemetry: poll_count=1
  <inf> pigeon: Flushed 3 telemetry key(s) in one report (51 bytes)
  <inf> shadow: Applied shadow v1: log=false telemetry_interval=15
  <inf> pigeon: Target shadow received: target_version 1
  <inf> shadow: Reported current_config back to platform at v1
  <inf> shadow: Next pass in <=15 s (or sooner on a pushed shadow)

``MQTT TLS ciphersuite`` is read off the socket rather than assumed: what a
constrained build offers comes from its PSA wants, and which one is used is
the broker's choice among them; a certificate session lands on an ECDHE-ECDSA
suite instead. A Feather adds ``Provisioning CA certificate, sec_tag 47``
before the interface comes up on a certificate build, and ``Powering off
modem`` before any reboot. A tag's first provision logs two
``modem_key_mgmt: Key not found`` warnings ahead of it, from the installer
clearing a tag that holds nothing yet.

Save a change on the dashboard and the device applies it without waiting out
its interval::

  <inf> pigeon: Target shadow received: target_version 2
  <inf> shadow: Shadow v2: telemetry_interval 15 -> 20
  <inf> shadow: Applied shadow v2: log=true telemetry_interval=20
  <inf> shadow: Reported current_config back to platform at v2

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- The session connects and is closed immediately: the pigeon id, the CONNECT
  username and the PSK identity must all be the same string, and the broker
  refuses a session where they are not.
- ``MQTT connect ... failed on every resolved address``: the endpoint carries
  a path. This one names the broker, not a pigeon.
- A certificate build that fails the handshake against a local broker: pass
  that broker's own CA as ``-DPIGEON_MQTT_CA_FILE``. The default anchors the
  public chain only.
- Nothing arrives on the platform although the session is up: the pigeon's
  connector is not MQTT, so the broker has nothing to bridge onto.
- A Feather that will not attach for half an hour: the modem refuses to
  attach after repeated ungraceful resets. Let the device power the modem off
  (a shadow ``reboot`` does) instead of resetting it mid-attach.

Next steps
----------

- ``https_init``: the polling reference, plus remote log upload and FOTA
  through MCUboot.
- ``ws_init``: the same push behaviour over a WebSocket on the HTTPS
  connector, for a device that already speaks HTTPS.
- ``coap_dtls_init``: the constrained-device transport, CoAP over DTLS with a
  pre-shared key.
