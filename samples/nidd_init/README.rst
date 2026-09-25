nidd_init
=========

A pigeon over the carrier's Non-IP Data Delivery (NIDD). The device has no IP
path to the platform: its frames leave on a raw socket over a Non-IP PDN on the
APN ``VZWSCEF``, Verizon ThingSpace hands them to the platform, and the
platform's replies come back the same way. There is no TLS and no bearer token.
The SIM authenticates the device to the carrier, and a claim key built into the
firmware binds it to its pigeon and verifies every frame the platform sends.

At boot the device claims its pigeon with ``HELLO``, and the reply carries the
pigeon's target shadow, which the sample applies and reports. From then on it
takes a reading every quarter of its wake interval and sends the four as one
frame at each wake. A shadow the platform pushes, or owes the device, is applied
and reported on the connection it arrived on.

The lesson is in ``src/shadow.c``. ``src/main.c`` shows the order the modem
needs: the modem library, then ``pigeon_init()`` while the modem is still
offline (the PDP contexts, PSM and eDRX can only be written then), then the
NB-IoT attach, then ``pigeon_nidd_start()``.

What you need
-------------

- A Circuit Dojo nRF9160 Feather. Not the nRF9151 Feather: Verizon serves NIDD
  only to modules it supports on its network, and the nRF9151 is not one, so
  the build refuses that board.
- A Verizon SIM on the NIDD price plan, with NB-IoT coverage. NIDD is enabled
  per line in ThingSpace, and the line must not send until ThingSpace reports
  ``ConfigCreated`` for it.
- A pigeon on the platform whose connector is ``Nidd``, created with the
  modem's IMEI. The sample logs the IMEI at boot (``NIDD: modem IMEI``), so a
  first boot without a pigeon is one way to read it.
- The pigeon's claim key: 32 lowercase hex characters, returned once when the
  pigeon is created and again at every token refresh.
- A signing key for the build, which includes MCUboot. Generate one with
  ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree and
  export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Unset, the build warns and signs with a key anyone can forge.

Configure
---------

Write ``samples/nidd_init/prj.local.conf``; it is git-ignored and merged on top
of every other configuration file::

  CONFIG_PIGEON_NIDD_CLAIM_KEY="<32 lowercase hex characters>"

The build fails unless the key is exactly 32 characters. It is compiled in, so
a token refresh on the pigeon, which mints a new claim key and marks the pigeon
unclaimed, needs a rebuild with the new key. The endpoint, ``nidd://VZWSCEF``,
is the same for every pigeon and is set in ``prj.conf``.

The radio budget
----------------

Verizon asks that a device make at most four radio accesses an hour, uplink and
downlink together. Keeping to that is the application's job: the library sends
only what the application asks for, plus ``HELLO`` at boot.

- The device wakes every ``telemetry_interval`` seconds of its shadow, 1200 until
  the shadow sets it, and never more often than every 900 seconds: a shorter
  value is raised to 900 and reported as applied. A 20-minute wake leaves one
  access an hour for a pushed shadow.
- Each wake is one frame carrying four readings, ``uptime_s`` and ``reading``,
  the second counting from 1 at every boot so a gap shows a lost reading.
- A reply rides the connection its uplink opened and costs no access of its
  own. The library holds that connection until the reply owed to a ``HELLO`` or
  a shadow report has arrived, then asks for release; after a wake of readings
  alone the network releases it on its own after a few seconds.
- PSM is requested at every boot with a 190-minute periodic TAU and a 60-second
  active time: the modem keeps PSM settings across images, and Verizon NB-IoT
  refused the 30-minute default. The active time is the window in which a
  pushed shadow can page the device.
- eDRX is written off at every boot. A device in eDRX is paged only on its
  paging occasions, and a long cycle can leave the active time with none.

Build and flash
---------------

From the ``west-ncs.yml`` topdir, with the Python environment active, over a
J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/nidd_init
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 115200

What you should see
-------------------

In order, with ``<...>`` standing for this board's own values::

  <inf> pigeon: Transport mapped to NIDD through the carrier: nidd://VZWSCEF
  <inf> pigeon: NIDD: PSM requested
  <inf> pigeon: NIDD: eDRX off
  <inf> pigeon: NIDD: Non-IP on CID 1, APN VZWSCEF
  <inf> main: Attaching over NB-IoT
  <inf> pigeon: NIDD: network granted PSM, TAU <seconds> s, active time <seconds> s
  <inf> pigeon: NIDD: modem IMEI <imei>, the id this pigeon is registered under
  <inf> pigeon: NIDD: Non-IP PDN up on CID 1
  <inf> pigeon: NIDD: Non-IP socket open on PDN <id>
  <inf> pigeon: NIDD: sent HELLO, 33 bytes
  <inf> pigeon: NIDD: SHADOW <target> <current>, <n> config bytes
  <inf> shadow: Applied shadow v<target>: telemetry_interval=<seconds>

When the platform holds an older version than the target, the report follows
on the same connection, then its confirmation and the release::

  <inf> pigeon: NIDD: sent SHADOW_REPORT, <n> bytes
  <inf> pigeon: NIDD: STATUS STORED <target>
  <inf> pigeon: NIDD: report v<target> confirmed
  <inf> shadow: Reported shadow v<target>
  <inf> pigeon: NIDD: requested radio release

Each wake then prints ``Flushed 4 telemetry reading(s) in one batch`` after
``NIDD: sent TELEMETRY``, and ``NIDD: radio connected`` and ``NIDD: radio
idle`` bracket every connection, which is how the console shows the radio
budget being kept.

Troubleshooting
---------------

- The build fails naming ``CONFIG_PIGEON_NIDD_CLAIM_KEY``: the key is missing
  from ``prj.local.conf`` or is not 32 characters. Naming the network mode: the
  build must stay NB-IoT.
- ``NIDD: claim key refused``: the firmware carries an old key, usually after a
  token refresh. Billable sends stop until the next boot; rebuild with the
  pigeon's current key.
- ``NIDD: account paused``: the account's free allowance is spent. Billable
  sends are held for the time the notice names, and the newest readings stay
  buffered meanwhile.
- ``NIDD: dropped a <n>-byte frame whose tag did not verify``: a frame signed
  with a key other than the one built in, which the device ignores.
- No attach for half an hour after a run of resets: the modem's reset-loop
  protection. Let the device power its modem off before a reset or a reflash,
  as the sample does before every reboot.
- ``NIDD: PSM request failed``: the modem refused the requested timers; see
  ``CONFIG_LTE_PSM_REQ_RPTAU`` and ``CONFIG_LTE_PSM_REQ_RAT`` in ``prj.conf``.

Next steps
----------

``nidd_probe`` checks the carrier's half of NIDD on its own, with no pigeon:
the attach, the Non-IP PDN and every downlink as hex. Start there when this
sample never reaches ``NIDD: SHADOW``.
