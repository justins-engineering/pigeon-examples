# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this
repository.

## What this repo is

`pigeon-examples` holds the Zephyr sample applications for `pigeon`, the PidgeIoT device client
library. Its job is to prove `pigeon` works on real firmware. The library is a sibling checkout at
`~/pigeon`, linked in rather than fetched.

Read `~/pigeon/CLAUDE.md` before touching anything shadow-, auth- or connector-shaped: it carries
the wire contract with the `~/pidgeiot` backend that these samples have to stay consistent with.

Everything under `samples/`, plus `README.md` and `docs/`, is customer-facing. A Zephyr developer
evaluating the platform reads it. Nothing there names this file, an agent, a task or a date.

## Workspace

The git repository IS the west topdir and `.west/config` is tracked, so a clone needs no
`west init`. `samples/` is west's manifest self path but is not a git repo of its own.

Two manifests, one topdir each, because they vendor incompatible Zephyr trees:

| topdir | manifest | boards |
|---|---|---|
| `~/pigeon-examples` | `samples/west.yml`, upstream Zephyr | `esp32c6_devkitc/esp32c6/hpcore`, `native_sim/native/64` |
| `~/pigeon-examples-ncs` | `samples/west-ncs.yml`, nRF Connect SDK | `circuitdojo_feather/nrf9160/ns`, `circuitdojo_feather_nrf9151/nrf9151/ns` |

The NCS topdir is not a checkout: its `samples/` is a hardlink copy (`cp -al`) of this one. After
editing a sample here, refresh only that sample's directory there:

```sh
rm -rf ~/pigeon-examples-ncs/samples/<sample>
cp -al ~/pigeon-examples/samples/<sample> ~/pigeon-examples-ncs/samples/<sample>
```

Editing a file breaks its hardlink, since Edit and Write replace and rename, while every untouched
file stays in sync. The symptom is an edit CMake correctly lists in `CONF_FILE` that `.config`
never reflects.

Never build the NCS flavor in a scratch topdir that symlinks the vendored trees back here. Kconfig
resolves the symlink to the nearest `.west`, finds this topdir's config, and silently builds
against the wrong manifest with `ZEPHYR_NRF_MODULE_DIR` empty.

`pigeon` is deliberately not a west project: `common/app.cmake` adds `<topdir>/pigeon` to
`ZEPHYR_EXTRA_MODULES` by path, so an edit in `~/pigeon` is picked up by the next build. Nothing
here records which `pigeon` commit built what, so `git -C ~/pigeon log` is where a build that broke
without a change here is explained.

## samples/ layout

- `common/app.cmake`, included by every sample **before** `find_package(Zephyr)`: adds `../pigeon`
  as an extra module, appends the board-family fragment matching `${BOARD}`, appends
  `common/boards/tls-<shape>.conf` for each shape a sample names in `SAMPLE_TLS` (`verify`, `psk`),
  and merges `prj.local.conf` then `boards/<board>.local.conf`. A sample with no transport sets
  `SAMPLE_NETWORK OFF` before including it.
- `common/net.cmake`, included **after** `project()`: compiles one bring-up source, `net/lte.c` on
  the nRF91, `net/wifi.c` under `CONFIG_WIFI`, `net/native_sim.c` under NSOS.
- `common/net/net_connect.h`: `net_prepare()`, `net_connect()`, `net_disconnect()`,
  `net_install_ca()`. `net_prepare()` exists because a modem reaches its credential store over AT
  commands that need the modem library running and the modem offline, so the order in every
  sample's `main()` is prepare, `pigeon_init()`, connect.
- `common/boards/`: `nrf91.conf`, `nrf9151.conf`, `esp32c6.conf`, `native_sim.conf`,
  `tls-verify.conf`, `tls-psk.conf`.
- `zephyr/module.yml` registers `samples/boards` as a board root for the application and sysbuild
  stages, so the out-of-tree nRF9151 needs no `-DBOARD_ROOT`.
- `Kconfig.sysbuild.signing` and `mcuboot-signing.cmake`: one signing-key symbol fed to both
  images, and the warning when `PIGEON_BOOT_SIGNATURE_KEY_FILE` is unset.
- A sample is `src/main.c` plus `src/shadow.c` and whatever else is its lesson, `prj.conf`,
  `boards/` entries only for what it needs beyond the family fragment, `Kconfig.sysbuild`,
  `sysbuild.cmake` and `README.rst`.

## The samples

Every sample builds for all four boards except `wifi_init`, which is ESP32-C6 and `native_sim`
only.

- `shadow_model`: the shadow structs with no transport. `CONFIG_PIGEON` stays off, so it is the
  smoke test that the shared data structures still compile after a `pigeon` header change.
- `https_init`: the reference port. HTTPS polling, log upload, FOTA through MCUboot. Copy its
  `CMakeLists.txt` shape for a new sample.
- `ws_init`: HTTPS plus `CONFIG_PIGEON_WS`, the persistent push channel, plus the WS-riding
  diagnostic shell and `src/heap_monitor.c`.
- `coap_dtls_init`: CoAP over DTLS/UDP with a pre-shared key and RFC 9146 Connection ID. The
  primary constrained-device transport.
- `coap_tcp_init`: the RFC 8323 TLS/TCP sibling. Every exchange opens its own session.
- `mqtt_init`: one persistent session to the `pigeonhole` broker (`~/pigeonhole`, whose
  `docs/design.md` is the authority for the topic map and auth model). The target shadow arrives
  as a retained publish, so the periodic pass is the telemetry tick.
- `asset_tracker`: GNSS position as telemetry. `src/gnss.c` talks to `nrf_modem_gnss`;
  `src/gnss_sim.c` fabricates a track everywhere else and reports `gps_fix_quality=2`, never 1.
- `wifi_init`: WiFi bring-up with the HTTPS connector. Firmware updates are opt-in through
  `fota.conf` plus `sysbuild-mcuboot.conf`, which go together.

## Credentials

- `samples/<sample>/prj.local.conf`: endpoint, token, PSK. Git-ignored, merged by `app.cmake`.
- `samples/<sample>/boards/<board>.local.conf`: credentials only one board has, merged after the
  above. WiFi keys live here and nowhere else, because Kconfig prints the value assigned to a
  symbol whose dependencies are unmet, so a WiFi password in `prj.local.conf` reaches the build log
  of every board without WiFi.

Never print, echo, commit or paste the contents of either, or of anything under `samples/keys/`.
Reference them by path.

**A sample's existing `prj.local.conf` may belong to live hardware.** One of them once pointed at a
production pigeon that was actively reporting, and a `native_sim` run stole its WebSocket. Provision
a dedicated pigeon and pass it with `-DEXTRA_CONF_FILE=<scratch path>`, or override single symbols
with `-DCONFIG_...`, rather than reusing a file that "just works".

## Building

```sh
source ~/pigeon-examples/.venv/bin/activate   # both topdirs use this venv
export PIGEON_BOOT_SIGNATURE_KEY_FILE=$PWD/samples/keys/private/boot-ecdsa-p256.pem
west build -p always -d build_<what> -b <board target> samples/<sample>
```

- Always `-p always`. Only one sample and board can live in a build directory at a time.
- The NCS topdir defaults sysbuild on: its west enables sysbuild when nothing sets it, and
  neither `.west/config` does. So a Feather builds MCUboot, TF-M and the application with no
  flag, and it is the vanilla topdir, which defaults it off, where a C6 build that needs
  MCUboot passes `--sysbuild`. Name the flag only where it changes what gets built; each
  sample's README carries the exact command per board.
- Build directories are gitignored under `/build/` and `/build_*/`. `~/pigeon`'s clangd config
  points at `build/https_init/compile_commands.json` here through a `pigeon/build` symlink, so
  keep `https_init` building under plain `build/` for that tooling to resolve includes.
- The ESP32-C6 needs `west blobs fetch hal_espressif` and `west packages pip --install` once.
- Confirm a build by running it and checking it exits 0, never by reading the Kconfig and
  inferring.

## Traps that have cost real time

- **Modem reset safety.** Never reset, reflash or power-cycle an nRF91 mid-connection. An
  ungraceful reset trips the modem's reset-loop protection and refuses LTE attach for 30 minutes.
  `net_disconnect()`'s explicit `CFUN=0` is what avoids it; do not skip it when scripting.
- **The ESP32-C6 console port drives EN and IO9.** A terminal opened with DTR and RTS asserted
  parks the chip in download mode. Open it with `--rts 0 --dtr 0`.
- **`native_sim` needs a dummy address.** NSOS has no L2 and never runs DHCP, and the connection
  manager only reports L4 connected once the interface has an address, so `net/native_sim.c`
  assigns `192.0.2.1`, which nothing routes through.
- **Both CoAP samples fill about 90% of the stock non-secure slot on a Feather.** Adding a
  subsystem there needs `https_init`'s partition-rebalance overlay.
- **Espressif's MCUboot port is overwrite-only**, so a bad image on the C6 cannot be reverted to a
  previous slot. See `docs/firmware-updates.md`.

## Conventions

- 2-space indentation, Google base style, 100-column limit (`.clang-format`); clangd is configured
  for `arm-zephyr-eabi` (`.clangd`).
- A comment states a constraint the code cannot show, tersely, for a human. No task numbers, no
  dates, no change-log narration, no verification stories, no notes to a reviewer.
- KISS: the plain construct first. No new dependencies and no new Kconfig symbols unless a board
  genuinely needs one.
- A sample stays a teaching unit: `main.c` and `shadow.c` are the lesson and stay in the sample;
  board bring-up is plumbing and lives in `common/`.
- `README.md` and `docs/` are the user-facing documentation. Every command in them exists in the
  tree exactly as written; keep it that way when the tree moves.
