wifi_init
=========

Brings an ESP32-C6 onto a WiFi network and runs the platform's HTTPS
connector over it: poll the device shadow, apply its ``target_config``,
report the result back and send telemetry. Start from this sample for a
mains-powered WiFi device, and for the WiFi half of any board bring-up.

The lesson is in ``src/shadow.c``. Joining the network lives in
``../common/net``, behind ``net_connect()``, ``net_disconnect()`` and
``net_install_ca()``, so ``main.c`` reads the same here as in every other
sample.

Firmware updates are off by default: this sample's plain build is a single
image with no bootloader, which flashes without a signing key or a slot
layout to match. ``fota.conf`` and ``sysbuild-mcuboot.conf`` turn MCUboot and
``CONFIG_PIGEON_FOTA`` on together.

What you need
-------------

- A board: an ESP32-C6-DevKitC on a 2.4 GHz WiFi network, or native_sim on a
  build host with internet access.
- A pigeon on the platform. Its endpoint and token come back once, from the
  create or token-refresh response.
- A signing key, for the firmware-update build only. Generate one with
  ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree and
  export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in the build shell.
  Without it MCUboot's public development key signs the image and the build
  warns; fine on a bench, never on a device that leaves it.

Configure
---------

Write ``samples/wifi_init/prj.local.conf``; it is git-ignored and merged on
top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="https://api.pidgeiot.com/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_TOKEN="<device-bearer-token>"

The values are compiled in, so a change here needs a rebuild. Refreshing a
pigeon's token revokes the previous one; rebuild with the new value, and
rebuild any firmware image that was uploaded to the platform before the
refresh, since it carries the old token too.

The WiFi credentials go in a second git-ignored file, beside the board's own
conf and merged after ``prj.local.conf``: write
``samples/wifi_init/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops them reaching the build log
of a board that has no WiFi: Kconfig prints the value assigned to a symbol whose
dependencies are unmet, and that file is merged on every board. This sample
builds for ``native_sim`` too, which is such a board.

The SSID has to be a non-empty string before the merge, so
``../common/boards/esp32c6.conf`` ships ``"changeme"``. A build that never got
that board file fails to associate rather than failing quietly.

The sample's own options:

- ``CONFIG_PIGEON_HTTPS_SEC_TAG``: the TLS security
  tag the CA certificate in ``cert/`` is installed under; the default of 1
  needs no change.
- ``CONFIG_PIGEON_FOTA`` (``fota.conf``): downloads a firmware image named by
  the shadow into MCUboot's second slot and reboots into it.
- ``CONFIG_PIGEON_FOTA_CURRENT_VERSION`` (``fota.conf``): what this build
  reports as its running version. It ships as a placeholder and has to name
  the version the image is uploaded under, since the library compares that
  string and never reads MCUboot's image header.
- ``CONFIG_PIGEON_FOTA_CHUNK_SIZE``, ``CONFIG_PIGEON_FOTA_RESUME``,
  ``CONFIG_PIGEON_FOTA_ATTEMPT_BUDGET`` and ``CONFIG_PIGEON_REBOOT_ON_FATAL``
  (``fota.conf``): the settings that carry a download to the end on this
  board.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool,
silences or restores logging), ``telemetry_interval`` (seconds between
polls), ``reboot`` (one-shot) and, on a firmware-update build, ``firmware``
(``version``, ``size``, ``sha256``, as the dashboard's firmware assignment
writes them).

Build and flash
---------------

Activate the Python environment in every terminal first, from the
``west.yml`` topdir::

  source .venv/bin/activate

native_sim. No bootloader, so no firmware updates::

  west build -p always -d build -b native_sim/native/64 samples/wifi_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, the single-image build. The USB-JTAG port flashes, the
CP210x port is the console. That port drives EN and IO9, so a console opened
with DTR and RTS asserted parks the chip in download mode::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/wifi_init
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

ESP32-C6-DevKitC with firmware updates. ``--sysbuild`` builds MCUboot
alongside the application, and both fragment paths resolve against the
sample directory rather than the one west runs in::

  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>
  west build -p always --sysbuild -d build -b esp32c6_devkitc/esp32c6/hpcore \
    samples/wifi_init -- \
    -DEXTRA_CONF_FILE=fota.conf -DSB_EXTRA_CONF_FILE=sysbuild-mcuboot.conf
  west flash -d build

The image to upload to the platform for a firmware update is
``build/wifi_init/zephyr/zephyr.signed.bin``, under the version string the
build carries in ``CONFIG_PIGEON_FOTA_CURRENT_VERSION``.

What you should see
-------------------

A first poll against the platform, here on an ESP32-C6::

  *** Pigeon v4.4.1 ***
  <inf> net_connect: Bringing WiFi interface up
  <inf> net_connect: Joining stored WiFi network (attempt 1)
  <inf> net_wifi_mgmt: Connection requested
  <inf> net_dhcpv4: Received: 192.168.3.18
  <inf> net_connect: Network connected, address assigned
  <inf> pigeon: Initializing Pigeon tracking instance: pigeon-wifi-sample
  <inf> pigeon: Transport mapped to secure HTTPS edge pipeline: https://<host>/device/pigeons/<pigeon-id>
  <inf> pigeon: Pigeon tracking instance ready: pigeon-wifi-sample
  <inf> pigeon: Queued telemetry: reset_cause=0
  <inf> shadow: Shadow fetched: target_version=39 current_version=39 updated_at=1788196053
  <inf> pigeon: Queued telemetry: uptime_s=7
  <inf> pigeon: Queued telemetry: poll_count=1
  <inf> pigeon: Flushed 3 telemetry key(s) in one report (51 bytes)
  <inf> shadow: Shadow already converged at version 39; nothing to apply
  <inf> shadow: Next shadow poll in 60 s

native_sim reaches the same point without the join and the DHCP lease. A
shadow that has not converged logs what it applied and the report back to the
platform instead of the converged line.

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- The board retries a failed join forever, so repeated
  ``timed out waiting for the network`` means the SSID, the password or the
  band is wrong. This radio is 2.4 GHz only.
- ``401`` on every request: the token was refreshed after this build; rebuild.
- A firmware update that downloads and then reboots back into the old image:
  the image was signed with a key MCUboot does not trust; both images must be
  built with the same ``PIGEON_BOOT_SIGNATURE_KEY_FILE``.
- A firmware-update build with no firmware updates in it: without
  ``-DSB_EXTRA_CONF_FILE=sysbuild-mcuboot.conf`` sysbuild builds no
  bootloader, ``CONFIG_PIGEON_FOTA`` loses the dependency it needs and drops
  out, and the build still succeeds. The two fragments go together.
- ``FOTA: attempt budget spent``: the device has stopped chasing a target it
  cannot reach. The dashboard's re-push firmware action writes the shadow
  anew, which reopens the budget.
- ``SHA-256 comparison failed ... Attempting to boot anyway`` at power-on:
  the single-image build has no digest for the first-stage loader to check.
  The firmware-update build, which MCUboot verifies, does not print it.
- native_sim logs one ``Network disconnected`` before ``Network connected``:
  the simulated interface reports its state before it has an address.

Next steps
----------

- ``https_init``: the same connector with remote log upload, on the cellular
  Feathers as well as this board.
- ``ws_init``: a WebSocket push channel for a device that should react to a
  config change without polling.
- ``coap_dtls_init``: the constrained-device transport, CoAP over DTLS with a
  pre-shared key.
