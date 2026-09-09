https_init
==========

Polls the device shadow over HTTPS, applies its ``target_config``, reports
the result back, sends telemetry, uploads the device's own logs and stages a
firmware update through MCUboot. This is the reference for a device that is
normally connected and polls the platform; start from it for a cellular or
WiFi device, and copy its ``CMakeLists.txt`` shape for any new sample.

The lesson is in ``src/shadow.c``. Board bring-up lives in
``../common/net``, behind ``net_connect()``, ``net_disconnect()`` and
``net_install_ca()``, so ``main.c`` reads the same on every board.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that
  has LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a
  build host with internet access.
- A pigeon on the platform. Its endpoint and token come back once, from the
  create or token-refresh response.
- A signing key for the MCUboot builds. Generate one with
  ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree and
  export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Unset, the build warns and signs with a key anyone can forge.

Configure
---------

Write ``samples/https_init/prj.local.conf``; it is git-ignored and merged on
top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="https://api.pidgeiot.com/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_TOKEN="<device-bearer-token>"

The values are compiled in, so a change here needs a rebuild. Refreshing a
pigeon's token revokes the previous one; rebuild with the new value, and
rebuild any firmware image that was uploaded to the platform before the
refresh, since it carries the old token too.

On the ESP32-C6 the WiFi credentials go in a second git-ignored file, in the
sample's own ``boards/`` directory, creating it if it is not there: write
``samples/https_init/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops a WiFi password reaching the
build log of every board that has no WiFi; ``docs/esp32c6.md`` says why.

The sample's own options, all in ``prj.conf`` unless noted:

- ``CONFIG_PIGEON_LOG_UPLOAD``: batches this device's log output and posts
  it to the platform, dictionary-encoded.
- ``CONFIG_PIGEON_FOTA``: downloads a firmware image named by the shadow
  into MCUboot's second slot and reboots into it.
- ``CONFIG_PIGEON_FOTA_CURRENT_VERSION`` (``boards/nrf91.conf`` on the
  Feathers, ``boards/esp32c6_devkitc_hpcore.conf`` on the ESP32-C6): what
  this build reports as its running version, and what the shadow's firmware
  target is compared against. Bump it and this sample's ``VERSION`` file
  together with each release; ``docs/firmware-updates.md`` says why.
- ``CONFIG_PIGEON_HTTPS_SEC_TAG`` (42): the TLS security tag the CA
  certificate in ``../common/cert/`` is installed under. Each sample pins its own,
  because the modem's credential store outlives a reflash.
- ESP32-C6 only (``boards/esp32c6_devkitc_hpcore.conf``): 32 KiB download
  chunks, download resume, a bounded attempt budget per firmware target and
  reboot-on-fatal, the set that completes a download on that board.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool,
silences or restores logging), ``telemetry_interval`` (seconds between
polls), ``reboot`` (one-shot) and ``firmware`` (``version``, ``size``,
``sha256``, as the dashboard's firmware assignment writes them).

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir. No bootloader, so no FOTA::

  west build -p always -d build -b native_sim/native/64 samples/https_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. ``--sysbuild`` is required:
it builds MCUboot alongside the application, and an image signed for a
bootloader that is not flashed does not boot. The USB-JTAG port flashes, the
CP210x port is the console::

  west build -p always --sysbuild -d build -b esp32c6_devkitc/esp32c6/hpcore samples/https_init
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/https_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 1000000

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard
CMSIS-DAP probe, whose CDC-ACM port is also the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/https_init
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

The image to upload to the platform for a firmware update is
``build/https_init/zephyr/zephyr.signed.bin``, under the version string the
build carries in ``CONFIG_PIGEON_FOTA_CURRENT_VERSION``.

What you should see
-------------------

A first poll against the platform, here on native_sim::

  *** Booting Zephyr OS build v4.4.1 ***
  <inf> net_connect: Bringing network interface up
  <inf> net_connect: Connecting to the network
  <inf> net_connect: Network connected
  <inf> pigeon: Initializing Pigeon tracking instance: pigeon-sample
  <inf> pigeon: Transport mapped to secure HTTPS edge pipeline: https://<host>/device/pigeons/<pigeon-id>
  <inf> pigeon: Pigeon tracking instance ready: pigeon-sample
  <inf> pigeon: Queued telemetry: reset_cause=8
  <inf> shadow: Shadow fetched: target_version=1 current_version=0 updated_at=1786204292
  <inf> pigeon: Queued telemetry: uptime_s=1
  <inf> pigeon: Queued telemetry: poll_count=1
  <inf> pigeon: Flushed 3 telemetry key(s) in one report (51 bytes)
  <inf> shadow: Applied shadow v1: log=false telemetry_interval=60
  <inf> shadow: Reported current_config back to platform at v1
  <inf> shadow: Next shadow poll in 60 s

A Feather adds ``Provisioning CA certificate, sec_tag 42`` before the
interface comes up, and ``Powering off modem`` before any reboot. Once the
shadow has converged, later polls log ``Shadow already converged at version
N; nothing to apply`` instead of applying and reporting; that is expected.

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- A Feather that will not attach for half an hour: the modem refuses to
  attach after repeated ungraceful resets. Let the device power the modem
  off (a shadow ``reboot`` does) instead of resetting it mid-attach.
- ``401`` on every request: the token was refreshed after this build; rebuild.
- An ESP32-C6 that never boots: the build was made without ``--sysbuild``.
- A firmware update that downloads and then reboots back into the old image:
  the image was signed with a key MCUboot does not trust; both images must be
  built with the same ``PIGEON_BOOT_SIGNATURE_KEY_FILE``.
- native_sim logs one ``Network disconnected`` before ``Network connected``:
  the simulated interface reports its state before it has an address.

Next steps
----------

- ``ws_init``: the same connector plus a WebSocket push channel for devices
  that should react to a config change without polling.
- ``coap_dtls_init``: the constrained-device transport, CoAP over DTLS with
  a pre-shared key.
- ``mqtt_init``: one persistent MQTT session to the platform broker.
- ``asset_tracker``: GNSS position as telemetry on the Feathers.
