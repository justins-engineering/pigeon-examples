asset_tracker
=============

Reports where a device is. It polls the device shadow over HTTPS like
``https_init`` does, and adds the position as telemetry: latitude, longitude,
altitude, speed, heading, satellite count and a fix quality, all in the same
report as ``uptime_s``. Start from it for anything that moves and has nobody
standing next to it.

The lesson is in ``src/shadow.c`` and ``src/gnss.c``. Board bring-up lives in
``../common/net``, behind ``net_connect()``, ``net_disconnect()`` and
``net_install_ca()``, so ``main.c`` reads the same on every board.

Only the nRF91 modem has a GNSS receiver. Every other board reports a
fabricated circuit instead, as ``gps_fix_quality`` 2 rather than 1, and says so
on the console at boot. That code is in ``src/gnss_sim.c``; the two files share
``src/gnss.h`` and the build picks one.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that has
  LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a build
  host with internet access.
- A sky view, for a real fix. GNSS indoors will very likely never acquire one,
  and the receiver reports satellites tracked but no position while it tries.
- A pigeon on the platform. Its endpoint and token come back once, from the
  create or token-refresh response.
- A signing key for the Feather builds, which boot through MCUboot. Generate
  one with ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree
  and export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Without it MCUboot's public development key signs the image and the build
  warns; fine on a bench, never on a device that leaves it.

Configure
---------

Write ``samples/asset_tracker/prj.local.conf``; it is git-ignored and merged on
top of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="https://api.pidgeiot.com/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_TOKEN="<device-bearer-token>"

The values are compiled in, so a change here needs a rebuild. Refreshing a
pigeon's token revokes the previous one, so rebuild with the new value.

On the ESP32-C6 the WiFi credentials go in a second git-ignored file, beside
that board's own conf and merged after ``prj.local.conf``: write
``samples/asset_tracker/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops them reaching the build log
of a board that has no WiFi: Kconfig prints the value assigned to a symbol whose
dependencies are unmet, and that file is merged on every board.

The sample's own options:

- ``CONFIG_ASSET_TRACKER_SIM_GPS``: report the simulated track. Forced on
  wherever there is no receiver, and offered on a Feather for an indoor demo.
- ``CONFIG_ASSET_TRACKER_SIM_BASE_LAT`` / ``_LON`` / ``_RADIUS_M`` /
  ``_PERIOD_SEC``: where the simulated circuit is, how wide, and how long a lap
  takes. The radius and the period together set the reported speed.
- ``CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC``: seconds between fixes.
  Periodic navigation leaves the modem more of the shared radio for LTE.
- ``CONFIG_ASSET_TRACKER_GNSS_FIX_RETRY_SEC``: how long the receiver may search
  before abandoning one fix and waiting for the next interval.
- ``CONFIG_ASSET_TRACKER_CONNECT_MAX_ROUNDS`` and
  ``CONFIG_ASSET_TRACKER_CONNECT_BACKOFF_BASE_SEC`` / ``_MAX_SEC``: how hard the
  device tries to reach the network before rebooting to try again from cold.
- ``CONFIG_PIGEON_HTTPS_SEC_TAG``: the TLS security tag the CA certificate in
  ``cert/`` is installed under; the default of 1 needs no change.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool, silences
or restores logging), ``telemetry_interval`` (seconds between polls) and
``reboot`` (one-shot).

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir. Simulated track, no bootloader::

  west build -p always --sysbuild -d build -b native_sim/native/64 samples/asset_tracker
  ./build/asset_tracker/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. Simulated track. The USB-JTAG
port flashes, the CP210x port is the console::

  west build -p always --sysbuild -d build -b esp32c6_devkitc/esp32c6/hpcore samples/asset_tracker
  west flash -d build
  pyserial-miniterm /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe. Real
GNSS::

  west build -p always --sysbuild -d build -b circuitdojo_feather/nrf9160/ns samples/asset_tracker
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 1000000

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard CMSIS-DAP
probe, whose CDC-ACM port is also the console. Real GNSS::

  west build -p always --sysbuild -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/asset_tracker
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

What you should see
-------------------

A first poll against the platform, here on native_sim::

  *** Booting Zephyr OS build v4.4.1 ***
  <wrn> main: Simulated position: every fix this build reports is fabricated and carries gps_fix_quality=2
  <inf> net_connect: Bringing network interface up
  <inf> net_connect: Network connected
  <inf> pigeon: Initializing Pigeon tracking instance: asset-tracker-sample
  <inf> pigeon: Queued telemetry: reset_cause=8
  <inf> gnss: Simulated track: a 50 m circuit around 45.523064,-122.676483, one lap every 300 s
  <inf> shadow: Shadow fetched: target_version=5 current_version=5 updated_at=1784661124
  <inf> pigeon: Queued telemetry: uptime_s=1
  <inf> pigeon: Queued telemetry: gps_fix_quality=2
  <inf> pigeon: Queued telemetry: gps_sats=9
  <inf> pigeon: Queued telemetry: gps_lat=45.523513
  <inf> pigeon: Queued telemetry: gps_lon=-122.676464
  <inf> pigeon: Queued telemetry: gps_alt_m=50.1
  <inf> pigeon: Queued telemetry: gps_speed_mps=1.05
  <inf> pigeon: Queued telemetry: gps_heading_deg=91.7
  <inf> shadow: Position (simulated): 45.523513,-122.676464 alt=50.1m speed=1.05m/s heading=91.7deg sats=9
  <inf> pigeon: Flushed 9 telemetry key(s) in one report (184 bytes)
  <inf> shadow: Shadow already converged at version 5; nothing to apply
  <inf> shadow: Next shadow poll in 60 s

A Feather adds ``Provisioning CA certificate, sec_tag 1`` before the interface
comes up, and logs ``GNSS started`` instead of the simulated track. Until it
fixes, each poll reports ``gps_fix_quality=0`` with the satellites it can see
and no position, which is what a device that has not found the sky looks like.

On the dashboard the pigeon shows as online and its telemetry carries
``gps_lat``, ``gps_lon``, ``gps_alt_m``, ``gps_speed_mps``, ``gps_heading_deg``,
``gps_sats`` and ``gps_fix_quality`` alongside ``uptime_s``. All of them are
numeric, so all of them graph.

Troubleshooting
---------------

- ``gps_sats=0`` outdoors on a Feather, forever: the GNSS low-noise amplifier
  is gated by an AT command Nordic's antenna library only issues for its own
  boards. ``boards/circuitdojo_feather_nrf9160_ns.conf`` sets it; check it
  survived any edit, and that the GNSS antenna is the one connected.
- ``gps_fix_quality=2``: this build reports the simulated track. Turn
  ``CONFIG_ASSET_TRACKER_SIM_GPS`` off, and note that only an nRF91 can.
- ``Failed to queue telemetry``: the report is larger than
  ``CONFIG_PIGEON_TELEMETRY_MAX_KEYS`` slots, so the last keys were dropped.
- A Feather that will not attach for half an hour: the modem refuses to attach
  after repeated ungraceful resets. Let the device power the modem off, which a
  shadow ``reboot`` does, instead of resetting it mid-attach.
- ``401`` on every request: the token was refreshed after this build; rebuild.
- native_sim logs one ``Network disconnected`` before ``Network connected``: the
  simulated interface reports its state before it has an address.

Next steps
----------

- ``https_init``: the same connector with firmware updates and log upload,
  which this sample leaves out.
- ``ws_init``: a WebSocket push channel, so a config change reaches the device
  without waiting for the next poll.
- ``coap_dtls_init``: the constrained-device transport, for a tracker on a
  tighter power or data budget.
