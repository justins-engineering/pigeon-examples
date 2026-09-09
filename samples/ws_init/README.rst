ws_init
=======

Keeps a WebSocket open to the platform alongside the HTTPS connector, so a
config change reaches the device as a push instead of on the next poll, and
telemetry rides a socket that is already connected. Start from this one for a
mains-powered device on WiFi or a cellular device that has to react quickly;
start from ``https_init`` instead if polling is fast enough, or if the device
sleeps between polls, because a persistent socket has to be kept alive.

HTTPS stays the transport for the shadow fetch and the report back. The socket
only decides how soon the device learns there is something to fetch, so
everything still works if it never comes up.

The lesson is in ``src/shadow.c``: ``shadow_ws_event_cb()`` turns a push into a
semaphore give, and ``shadow_loop()`` waits on that semaphore instead of
sleeping. Board bring-up lives in ``../common/net``, behind ``net_connect()``,
``net_disconnect()`` and ``net_install_ca()``, so ``main.c`` reads the same on
every board.

What you need
-------------

- A board: Circuit Dojo nRF9160 Feather or nRF9151 Feather with a SIM that
  has LTE-M data, an ESP32-C6-DevKitC on a WiFi network, or native_sim on a
  build host with internet access.
- A pigeon on the platform. Take its endpoint and token from the pigeon's
  detail page on the dashboard, where a token is shown once: when the pigeon
  is created, and again each time you refresh it.
- A signing key for the Feather builds, which boot through MCUboot. Generate
  one with ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the
  tree and export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build
  shell. Unset, the build warns and signs with a key anyone can forge.

Configure
---------

Write ``samples/ws_init/prj.local.conf``; it is git-ignored and merged on top
of every other configuration file::

  CONFIG_PIGEON_ENDPOINT="https://<platform-host>/device/pigeons/<pigeon-id>"
  CONFIG_PIGEON_TOKEN="<device-bearer-token>"

Copy the endpoint verbatim from the pigeon's detail page rather than
assembling it; mint the token there too, with the refresh action.

The values are compiled in, so a change here needs a rebuild. Refreshing a
token revokes the previous one the moment the new one is issued, so a device
still running the old build starts failing every request until it is rebuilt
and reflashed with the new value.

On the ESP32-C6 the WiFi credentials go in a second git-ignored file, in the
sample's own ``boards/`` directory, creating it if it is not there: write
``samples/ws_init/boards/esp32c6_devkitc_hpcore.local.conf``::

  CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
  CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"

Keeping them out of ``prj.local.conf`` is what stops a WiFi password reaching the
build log of every board that has no WiFi; ``docs/esp32c6.md`` says why.

The socket is authenticated with the same endpoint and token as the HTTPS
connector, reaching ``<endpoint>/ws``. There are no separate WebSocket
credentials to configure.

The sample's own options, in ``prj.conf`` unless noted. The parenthesised
values on the ``PIGEON_WS_*`` tunables are library defaults, not settings this
sample writes:

- ``CONFIG_PIGEON_WS``: the persistent channel. Off leaves plain HTTPS
  polling, which is what ``https_init`` builds.
- ``CONFIG_PIGEON_WS_PING_INTERVAL_SEC`` (60): the server never pings, so the
  device owns keepalive. Two missed replies force a reconnect.
- ``CONFIG_PIGEON_WS_RECONNECT_MAX_DELAY_SEC`` (300): the backoff ceiling
  after a drop.
- ``CONFIG_PIGEON_WS_RX_BUF_SIZE`` (1024): the buffer a pushed shadow has to
  fit into.
- ``CONFIG_PIGEON_SHELL`` and ``CONFIG_PIGEON_SHELL_ALLOWLIST``: diagnostics
  an operator can run from the dashboard over the same socket. This is remote
  execution on the device, so it is off by default in the library and denies
  anything the allowlist does not name. The allowlist here is
  ``kernel version,kernel uptime``; a command the image does not register
  answers ``-ENOEXEC`` however it is spelled.
- ``CONFIG_PIGEON_HTTPS_SEC_TAG`` (43): the TLS security tag the CA certificate
  in ``../common/cert/`` is installed under. Each sample pins its own, because the
  modem's credential store outlives a reflash.
- ESP32-C6 only (``boards/esp32c6_devkitc_hpcore.conf``): reboot on a fatal
  error and a wedge watchdog, because a long-lived socket on that board is
  what exposes a rare assert inside the closed-source WiFi driver.

``shadow.c`` understands these ``target_config`` keys: ``log`` (bool,
silences or restores logging), ``telemetry_interval`` (seconds between polls,
and the ceiling on how long the loop waits for a push) and ``reboot``
(one-shot). A firmware update is ``https_init``'s subject; this sample stages
nothing.

``src/heap_monitor.c`` logs the kernel heap, and the C library's own arena
where the board has one, every 30 seconds. A persistent socket is the case
where a slow leak shows up, so a soak can be read straight off the console.

Build and flash
---------------

Activate the Python environment in every terminal first::

  source .venv/bin/activate
  export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path to your key>

native_sim, from the ``west.yml`` topdir::

  west build -p always -d build -b native_sim/native/64 samples/ws_init
  ./build/zephyr/zephyr.exe

ESP32-C6-DevKitC, from the ``west.yml`` topdir. One image, no bootloader to
build. The USB-JTAG port flashes, the CP210x port is the console::

  west build -p always -d build -b esp32c6_devkitc/esp32c6/hpcore samples/ws_init
  west flash -d build
  pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200

nRF9160 Feather, from the ``west-ncs.yml`` topdir, over a J-Link probe. That
topdir builds MCUboot alongside the application without a flag::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/ws_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 1000000

nRF9151 Feather, from the ``west-ncs.yml`` topdir, over its onboard
CMSIS-DAP probe, whose CDC-ACM port is also the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/ws_init
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

What you should see
-------------------

The console messages to look for, in the order a healthy boot produces them:

- ``Bringing network interface up``, then ``Connecting to the network``, then
  ``Network connected``. A Feather provisions the CA into the modem first and
  logs ``Provisioning CA certificate, sec_tag 43`` before any of it.
- ``Pigeon tracking instance ready``, and the endpoint the transport resolved
  to. A wrong endpoint is visible here rather than three failures later.
- ``WS: worker thread started``. The channel is opened by its own thread, so
  the first shadow poll never waits for it.
- ``Shadow fetched``, with the target and current versions. They differ on the
  first poll of a fresh pigeon and match once it has converged, and a matching
  pair logs ``Shadow already converged`` and applies nothing.
- ``Flushed N telemetry key(s) in one report``. Every key a poll queued rides
  one report: a WebSocket frame while the socket is up, an HTTPS request when
  it is not. The first report of a boot carries an extra key, the reset cause
  the library queues at startup.
- ``Applied shadow`` and ``Reported current_config back to platform``, when a
  poll found something new to apply.
- ``Next shadow poll in <=60 s, sooner on a push``. This is the line that
  separates the sample from ``https_init``. Change the pigeon's
  ``target_config`` on the dashboard and the next poll starts within about a
  second instead of at the end of the interval, because the platform pushed
  rather than waiting to be asked.
- ``heap_stats``, every 30 seconds regardless of what else is happening, with
  free, allocated and peak bytes against the device's uptime. A board with a C
  library arena of its own logs ``libc_heap_stats`` beside it.

Losing the network logs the socket dropping and then reconnecting with a
backoff that widens toward five minutes, while the poll interval carries the
device in the meantime. A shadow carrying ``reboot`` closes the socket and
takes the network down before restarting, so a Feather also logs
``Powering off modem``.

On the dashboard the pigeon shows as online, its telemetry carries
``reset_cause``, ``uptime_s`` and ``poll_count``, and the shadow's current
version equals its target version.

Troubleshooting
---------------

- ``401`` on every request and ``WS: upgrade handshake failed: -113``
  together: the token was refreshed after this build, and both surfaces
  authenticate with it. Rebuild with the current one.
- ``WS: connect failed`` on its own while the shadow still syncs: the socket
  is refused but HTTPS is not, so the loop degrades to polling. The most
  common cause is another client already connected as this pigeon, since the
  platform allows one socket per pigeon and closes the older one.
- A push that never arrives while the socket is up: the dashboard write has
  to change ``target_config``. An identical write leaves the shadow version
  where it is and there is nothing to push.
- A Feather that will not attach for half an hour: the modem refuses to
  attach after repeated ungraceful resets. Let the device power the modem off
  (a shadow ``reboot`` does) instead of resetting it mid-attach.
- native_sim logs one ``Network disconnected`` before ``Network connected``:
  the simulated interface reports its state before it has an address.

Next steps
----------

- ``https_init``: the same connector with firmware updates and log upload,
  and the sample to copy for a device that only polls.
- ``mqtt_init``: one persistent MQTT session, where the push and the shadow
  share a transport rather than sitting beside each other.
- ``coap_dtls_init``: the constrained-device transport, CoAP over DTLS with a
  pre-shared key.
