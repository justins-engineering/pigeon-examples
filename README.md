# pigeon-examples

Zephyr/nRF Connect SDK sample applications for [`pigeon`](https://github.com/justins-engineering/pigeon) (the
PidgeIoT device client library, checked out as a sibling directory at `../pigeon`
and pulled in as a west module via `samples/pigeon_module.cmake`).

## Layout

This directory is a west workspace: `samples/` is the manifest ("self") repo, and
`nrf/`, `zephyr/`, `bootloader/`, `modules/`, `nrfxlib/` are vendored checkouts
fetched by `west update` (gitignored, not part of this repo).

```
samples/
  west.yml              # west manifest (self path: samples)
  pigeon_module.cmake    # shared: wires ../../pigeon in via ZEPHYR_EXTRA_MODULES
  https_init/             # pigeon_init() with an HTTPS connector; shadow sync,
                           # MCUmgr DFU, MCUboot/sysbuild, graceful modem shutdown
  coap_tcp_init/          # pigeon_init() with a CoAP-over-TLS/TCP connector
                           # (TLS PSK fields; no on-device UDP support yet);
                           # same shadow-sync loop as https_init, no bootloader
  shadow_model/           # builds pigeon_shadow_doc / pigeon_shadow_update_request
                           # structs and logs them (no transport yet)
```

Each sample is independently buildable. `https_init` and `coap_tcp_init` both
enable `CONFIG_PIGEON` now that `pigeon`'s HTTPS and CoAP-over-TLS/TCP
transports (`pigeon_https.c`/`pigeon_coap.c`) are implemented, and both
exercise the shadow-sync loop (fetch, apply, and report back via
`pigeon_set_shadow_param()`/`pigeon_shadow_flush()`). `shadow_model` still
leaves `CONFIG_PIGEON` disabled (see note in its `prj.conf`) since it only
needs `pigeon_init()` and the data structures in `pigeon.h`, which work
regardless — `pigeon`'s `CMakeLists.txt` compiles `pigeon_core.c`
unconditionally.

## Setup

```sh
python3 -m venv .venv && source .venv/bin/activate
pip install west
west init -l samples
west update
```

## Building a sample

`coap_tcp_init` and `shadow_model` have no bootloader and build for
`native_sim` for quick local iteration (`coap_tcp_init`'s LTE bring-up skips
the graceful-modem-shutdown path there — no real modem, and
`CONFIG_LTE_LINK_CONTROL` isn't available on that SoC):

```sh
source .venv/bin/activate
west build -d build samples/shadow_model -b native_sim/native/64
./build/shadow_model/zephyr/zephyr.exe
```

`https_init` boots under MCUboot via sysbuild (see `sysbuild.conf`), and
MCUboot doesn't support the native/`native_sim` SoC — so unlike the other two
samples, it only builds for real hardware:

```sh
source .venv/bin/activate
west build -d build samples/https_init -b circuitdojo_feather/nrf9160/ns
```

`coap_tcp_init` also builds for that same board target (no sysbuild/MCUboot
involved, so it isn't subject to the `native_sim` restriction above):

```sh
source .venv/bin/activate
west build -d build samples/coap_tcp_init -b circuitdojo_feather/nrf9160/ns
```

The `../pigeon` repo's `.vscode/settings.json` points clangd at
`build/https_init/compile_commands.json` here (via a `pigeon/build` symlink to
this repo's `build/`), so keep `https_init` building under plain `build/` for
IDE tooling to resolve `pigeon`'s includes.

## Flashing `https_init` to real hardware

This is the only sample that boots on and has been verified against a real
device: a CircuitDojo nRF9160 Feather, over J-Link/`nrfutil`.

### 1. Provision device credentials

`pigeon_init()` needs a pigeon's endpoint + bearer token, which are real
device secrets and must never be committed. They're supplied as
`CONFIG_PIGEON_ENDPOINT`/`CONFIG_PIGEON_TOKEN`, baked in at compile time via
`samples/https_init/prj.local.conf` (gitignored; auto-merged by
`CMakeLists.txt` if present — see the `EXTRA_CONF_FILE` logic there). Create
the pigeon first (dashboard or API), then write:

```sh
cat > samples/https_init/prj.local.conf <<'EOF'
CONFIG_PIGEON_ENDPOINT="https://<backend-host>/device/pigeons/<pigeon-id>"
CONFIG_PIGEON_TOKEN="<device-bearer-token>"
EOF
```

Both values come back from the pigeon's `create`/`token/refresh` response —
tokens are stripped from every other route, so this is the only chance to
capture one (refreshing mints a new keypair and revokes the old token).

`samples/https_init/src/main.c` also has a `config.device_id` field, used for
logging only (`pigeon_init()` never uses it to build a request — see the
comment above it in `main.c`). It's left as a neutral placeholder
(`"pigeon-sample"`) in tracked source and doesn't need to match
`CONFIG_PIGEON_ENDPOINT` for the sample to work — don't replace it with a
real pigeon ID, since unlike `prj.local.conf` this file is committed.

### 2. Build and flash

```sh
source .venv/bin/activate
west build -d build samples/https_init -b circuitdojo_feather/nrf9160/ns
west flash --no-rebuild -r nrfutil --erase --softreset
```

(`.vscode/tasks.json` has the same flash command as the "West Flash" task,
plus a "West Flash and Monitor" task that chains it with the serial monitor
below.) Changing `prj.local.conf` requires a rebuild — the values are baked
into the binary, not read at runtime.

### 3. Watch the serial console

The board enumerates a USB CDC serial port (`/dev/ttyUSB0` at 1000000 baud
here; may differ per host):

```sh
source .venv/bin/activate
pyserial-miniterm -f colorize /dev/ttyUSB0 1000000
```

Expected boot sequence on a working device + live backend:

```
*** Booting Pigeon v0.1.0-... ***
*** Using nRF Connect SDK v3.4.0-... ***
*** Using Zephyr OS v4.4.0-... ***
<inf> connection_manager: Bringing network interface up
<inf> connection_manager: Provisioning certificate
<inf> connection_manager: Connecting to the network
<inf> connection_manager: Network connectivity established and IP address assigned
<inf> pigeon: Transport mapped to secure HTTPS edge pipeline: https://<backend-host>/device/pigeons/<pigeon-id>
<inf> pigeon: Pigeon tracking instance ready: <device-id>
<inf> shadow: Shadow fetched: target_version=N current_version=M updated_at=...
<inf> pigeon: Flushed shadow param: uptime_s=...
<inf> shadow: Next shadow poll in 60 s
```

"Flushed shadow param: uptime_s=..." is telemetry, not a shadow report,
despite the log line's name — see the comment above `pigeon_shadow_flush()`
in `shadow.c`; it's a `pigeon_set_shadow_param()` + `pigeon_shadow_flush()`
pair that POSTs to `<endpoint>/telemetry`. A device only POSTs a shadow
*report* (`pigeon_shadow_report()`) when `target_version` fetched from the
shadow differs from `current_version` — with nothing new targeted, you'll
instead see `shadow: Shadow already converged at version N; nothing to
apply`, which is expected, not a failure.
