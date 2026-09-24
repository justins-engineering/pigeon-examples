nidd_probe
==========

Checks the carrier half of Non-IP Data Delivery (NIDD) from a bench board,
before any platform code is involved. It attaches over NB-IoT, brings up a
Non-IP PDN on the carrier's NIDD APN, prints every downlink it receives, and
sends test frames when told to from the console shell. It is not a pigeon
device: ``CONFIG_PIGEON`` stays off, and the modem is driven through
``nrf_modem`` and ``lte_lc`` directly.

Every modem notification is logged verbatim (``modem:`` lines), and the
responses the checks need are logged after each step: the modem firmware, the
IMEI and the stored PDP contexts at boot; the ICCID, the SIM's home network
and ``AT%XMONITOR`` once registered; then ``AT+CGDCONT?``, ``AT+CGACT?``,
``AT+CGCONTRDP`` (for CID 0 too when the Non-IP PDN has a context of its own),
``AT+CGAPNRC`` and ``AT+CCIOTOPT?`` once the Non-IP PDN is up. The home network
is the first six digits of the IMSI, its country and network codes; the rest
of the IMSI stays off the console. The IMEI and ICCID identify the board and
its SIM, so treat a capture accordingly.

The probe gives up rather than retrying: after ten minutes without
registration, or sixty seconds without the PDN, it logs what the modem last
reported and powers the modem off. The shell stays up for ``at`` commands.

What you need
-------------

- A Circuit Dojo nRF9151 Feather or nRF9160 Feather.
- A SIM with the carrier's NIDD plan, and NB-IoT coverage. Verizon delivers
  NIDD over NB-IoT only, through the APN ``VZWSCEF``.
- A signing key for the build, which includes MCUboot. Generate one with
  ``imgtool keygen -k <path> -t ecdsa-p256``, keep it outside the tree and
  export ``PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>`` in every build shell.
  Unset, the build warns and signs with a key anyone can forge.

Configure
---------

The claim key is optional. Without it the probe runs, but ``nidd hello`` is
refused and downlink tags go unchecked. To set it, write
``samples/nidd_probe/prj.local.conf``; it is git-ignored and merged on top of
every other configuration file::

  CONFIG_NIDD_PROBE_CLAIM_KEY="<32 hex digits>"

The sample's own options, in ``Kconfig``:

- ``CONFIG_NIDD_PROBE_APN`` (``VZWSCEF``): the carrier's Non-IP APN.
- ``CONFIG_NIDD_PROBE_DEDICATED_CID`` (off): off, the default context becomes
  the Non-IP PDN and the board has no IP at all, which is all a NIDD-only plan
  allows. On, the Non-IP PDN gets a context of its own, bound to the socket
  with ``SO_BINDTOPDN``, and the default context is set to IP on the APN the
  subscription names, so the attach itself is a plain IP one; that needs a
  plan that carries IP data too.

Build and flash
---------------

From the ``west-ncs.yml`` topdir, with the Python environment active.

nRF9151 Feather, over its onboard CMSIS-DAP probe, whose CDC-ACM port is also
the console::

  west build -p always -d build -b circuitdojo_feather_nrf9151/nrf9151/ns samples/nidd_probe
  west flash -d build -r probe-rs
  pyserial-miniterm /dev/ttyACM0 115200

nRF9160 Feather, over a J-Link probe::

  west build -p always -d build -b circuitdojo_feather/nrf9160/ns samples/nidd_probe
  west flash -d build -r nrfutil --erase --softreset
  pyserial-miniterm /dev/ttyUSB0 115200

What you should see
-------------------

In order: ``AT+CGMR`` and ``AT+CGSN=1`` lines, ``Attaching over NB-IoT``,
``modem: +CEREG:`` lines ending in a status of 1 (home) or 5 (roaming) with
access technology 9 (NB-IoT), ``LTE mode: NB-IoT``, a ``modem: +CGEV: ME PDN
ACT`` line for the Non-IP context, the PDN lines listed above, ``PDN ID``, and
finally::

  <inf> nidd_probe: Ready: 'nidd raw <bytes>' sends filler, 'nidd hello' sends HELLO

Before the attach, ``Tag check on the API reference example: ok`` shows that
the tag check itself works: it checks the API reference's tagged ``STATUS
STORED 7`` example against its all-zero key.

Each downlink then prints as ``Downlink, <n> bytes`` and a hex dump. A platform
frame, type 0x81 or 0x82, is decoded (``SHADOW`` with its two versions and the
size of its ``target_config``, ``STATUS`` with its code and argument) and its
tag checked against the claim key: ``Downlink tag ok`` or ``Downlink tag bad``.
Anything else prints ``Downlink tag none``.

Shell commands
--------------

- ``nidd raw <bytes>``: sends that many filler bytes, 1 to 2048. Byte 0 is
  0x00, a type no frame uses, and the rest count upward, so truncation shows in
  what arrives. The result prints as ``send(<n> bytes) returned <r>``, with the
  errno on failure.
- ``nidd hello``: sends HELLO, the type byte 0x04 followed by the 16 bytes of
  the claim key.
- ``nidd telemetry``: sends TELEMETRY, the type byte 0x01 followed by a flat
  JSON body with two keys: ``probe_seq``, which counts up from 1 at each send,
  so a burst shows which frames were lost, and ``uptime_s``.
- ``nidd report <version>``: sends SHADOW_REPORT, the type byte 0x02 followed
  by ``{"current_config":{},"current_version":<version>}``. The platform judges
  convergence by version alone, and a received config may not fit a frame.
- ``nidd psm on`` or ``nidd psm off``: requests Power Saving Mode with the
  ``lte_lc`` defaults (a 30-minute periodic TAU and 60 seconds of active time),
  or stops requesting it. The probe boots with PSM off, so a downlink can page
  it at any time until PSM is requested. What the network grants prints as
  ``PSM from the network``, and each modem sleep and wake as a ``modem:
  %XMODEMSLEEP`` line.
- ``at <command>``: any AT command, for example ``at AT%XMONITOR``.

A send succeeding means the modem accepted the bytes, not that anything
received them. Verizon asks that automated devices keep to four radio accesses
an hour.

Troubleshooting
---------------

- ``+CEREG: 2`` for minutes on end, then ``Not registered after 10 minutes``:
  no NB-IoT cell the SIM may use. The ``AT%XMONITOR`` and ``AT+CEER`` lines
  logged just before the modem powers off say what it last saw.
- ``+CEREG: 3``: registration denied; the ``AT+CEREG?`` line carries the
  reject cause.
- ``ESM error`` or ``Non-IP PDN on CID 0 not active``: the network refused the
  Non-IP PDN. The SIM may lack the NIDD plan, or the APN may be wrong.
