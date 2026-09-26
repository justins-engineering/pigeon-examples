shadow_model
============

Builds the shadow structs a connector exchanges with the platform, with no
transport underneath, and logs what a sync would send and receive. Start
here to see the shape of a shadow document and a shadow update before wiring
up a network; every other sample adds a transport on top of these structs.

What you need
-------------

Any of the four boards: Circuit Dojo nRF9160 Feather, Circuit Dojo nRF9151
Feather, ESP32-C6-DevKitC or native_sim on the build host. No SIM, no WiFi
credentials and no pigeon on the platform. The Feather builds include
MCUboot and warn when ``PIGEON_BOOT_SIGNATURE_KEY_FILE`` is unset; the
development key they fall back to is fine for a bench board.

Configure
---------

Nothing. ``CONFIG_PIGEON`` stays off because it selects a connector, so there
is no endpoint or token to supply and no ``prj.local.conf`` to write.

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate

native_sim, from the ``west.yml`` topdir::

  west build -p always -d build -b native_sim/native/64 samples/shadow_model
  ./build/zephyr/zephyr.exe -stop_at=2

ESP32-C6-DevKitC, from the ``west.yml`` topdir. The board's USB-JTAG port
flashes, the CP210x port is the console::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/shadow_model
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/shadow_model
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 115200

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard
CMSIS-DAP probe, whose CDC-ACM port is also the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/shadow_model
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

What you should see
-------------------

The same log lines on every board, after that board's own boot output; this is
native_sim::

  *** Booting Zephyr OS build v4.4.1 ***
  <inf> pigeon: Initializing Pigeon tracking instance: demo-pigeon-0003
  <inf> pigeon: Transport mapped to secure HTTPS edge pipeline:
  <inf> pigeon: Pigeon tracking instance ready: demo-pigeon-0003
  <inf> shadow_model: shadow target=v2 current=v1 target_config={"report_interval_s":60} current_config={"report_interval_s":300}
  <inf> shadow_model: shadow update request target_config={"report_interval_s":60}

Nothing reaches the platform, so the dashboard shows no change.

Troubleshooting
---------------

- The endpoint in the transport line is empty because no connector is
  configured; that is expected here.

Next steps
----------

- ``https_init``: the same structs synced over HTTPS, with telemetry, log
  upload and firmware updates.
