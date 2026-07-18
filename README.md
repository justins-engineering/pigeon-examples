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

## Decoding uploaded device logs (`CONFIG_PIGEON_LOG_UPLOAD`)

`https_init`'s `prj.conf` turns on `pigeon`'s opt-in remote-logging backend
(`CONFIG_PIGEON_LOG_UPLOAD`, see `~/pigeon/zephyr/Kconfig` and
`src/pigeon_log_backend.c`): a background ring buffer captures this device's
own log output via Zephyr's **dictionary-based logging**
(`CONFIG_LOG_DICTIONARY_SUPPORT`, selected automatically) and flushes it in
batches as a raw `application/octet-stream` POST to
`<CONFIG_PIGEON_ENDPOINT>/logs` (`pigeon_transport_upload_logs()` in
`pigeon_https.c`), device-authenticated the same way as the telemetry/shadow
reports. The win is that log **format strings never ship in the firmware
image or over the air** — each record on the wire is just a source id,
level, timestamp, and packed arguments — so decoding it back into readable
text needs a side-channel lookup table, not just the raw bytes.

### The dictionary database is a per-build artifact

Enabling `CONFIG_PIGEON_LOG_UPLOAD` makes every `west build` of `https_init`
emit `build/https_init/zephyr/log_dictionary.json` — the source-id-to-string
mapping for *that exact build*. It never leaves the host (it isn't flashed,
isn't uploaded, isn't part of the firmware image), and it doesn't carry over
between builds: rebuilding regenerates it, and a chunk uploaded by one build
can only be decoded with that same build's `log_dictionary.json`, not a
newer or older one. Keep the database alongside whatever firmware version
you flashed if you'll want to decode its logs later — there's no version tag
tying an uploaded chunk back to a specific `log_dictionary.json` other than
you keeping track yourself.

### Getting a raw chunk to decode

What lands at `<CONFIG_PIGEON_ENDPOINT>/logs` (device-facing `POST
/device/pigeons/:id/logs`) is exactly what `pigeon_log_backend.c` drained
from its ring buffer — a raw binary stream of concatenated dictionary log
records, no JSON envelope, no batching framing of its own beyond that
concatenation. dovecote keeps the last 200 uploaded chunks per pigeon (a
ring buffer server-side too, oldest pruned automatically) and exposes them
to the owning dashboard user at `GET /pigeons/:id/logs` (Kratos-session-
gated, not device-authenticated) as a JSON array of `{id, data, received_at}`
oldest-first, where `data` is the raw chunk **base64-encoded** for JSON
transport — decode that base64 back to bytes before handing it to
`log_parser.py` below, since the bytes it expects are the same raw stream
the device sent, not the base64 text. Save each chunk's decoded bytes to its
own file untouched; there's nothing else to unwrap.

### Running the decoder

Zephyr vendors its own dictionary-log decoder, unmodified — `pigeon` doesn't
ship a custom one:

```sh
source .venv/bin/activate
python3 zephyr/scripts/logging/dictionary/log_parser.py \
    build/https_init/zephyr/log_dictionary.json \
    <captured-chunk-file>
```

`log_parser.py`'s two positional args are the dictionary database, then the
raw log data file — no `--hex`/`--rawhex` needed for this path (those are
for hex-encoded transports, e.g. dumping over a text console; the HTTP POST
above is already raw binary end to end).

**Dependency gap:** `log_parser.py` imports `colorama` for its output
formatting, which this README's `pip install west` setup step does not pull
in — a fresh `.venv` fails at import time (`ModuleNotFoundError: No module
named 'colorama'`) before it even reads its arguments. Run
`python3 -m pip install colorama` (inside the activated venv; plain `pip`
isn't necessarily on `PATH` even when the venv is active) once, ahead of the
first decode.

**No-hardware sanity check:** `log_parser.py <dbfile> /dev/null` exits `0`
with no output (empty input, nothing to decode) — confirms the parser and
database pair are wired up correctly, independent of having a real captured
chunk yet. Verified against this repo's own
`build/https_init/zephyr/log_dictionary.json` while writing this section.

**Verified end-to-end on real hardware (2026-07-17):** flashed `https_init`
to a CircuitDojo nRF9160 Feather against the staging pigeon, confirmed via
`GET /pigeons/:id/logs` that dictionary-log chunks landed on the backend at
the expected `CONFIG_PIGEON_LOG_UPLOAD_MAX_INTERVAL_MS` (60s) cadence, and
decoded one with the exact command above. The decoded text lined up
byte-for-byte with what `pyserial-miniterm` showed live over UART for the
same boot — including the `*** Booting Pigeon ...` banner and first shadow
sync — confirming the dictionary database and the uploaded chunk really do
come from the same build.

## Firmware updates (`CONFIG_PIGEON_FOTA`)

`https_init`'s `prj.conf` turns on `pigeon`'s opt-in FOTA client
(`CONFIG_PIGEON_FOTA`, see `../pigeon/README.md`'s "Firmware updates"
section for the full API/Kconfig writeup) alongside
`CONFIG_PIGEON_FOTA_CURRENT_VERSION="0.1.0"` — this sample's compile-time
"what am I" string, compared against the shadow's `target_config.firmware`
on every poll. `shadow.c` wires the whole loop: `pigeon_fota_confirm_boot()`
runs on every successful `shadow_sync()` (the app's definition of "healthy
boot") *before* the convergence early-return, so a freshly-applied image
gets confirmed on its very first successful poll rather than waiting for a
config change; `pigeon_fota_apply()` fires when the shadow's
`firmware.version` doesn't match the running build; on success the device
reports its updated `current_config` back via `pigeon_shadow_report()`
*before* gracefully disconnecting LTE and rebooting — the shadow must
converge on the platform side before the device goes dark for the swap. A
failed `pigeon_fota_apply()` leaves `current_config.firmware` unchanged, so
the next poll sees the same mismatch and retries from byte 0 rather than
the shadow believing convergence already happened.

### Signing key — do not ship the default

Same caveat as `../pigeon/README.md`: `sysbuild.conf` here only sets
`SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y`, no
`CONFIG_BOOT_SIGNATURE_KEY_FILE` override, so every `https_init` build in
this repo today is signed with MCUboot's upstream dev key
(`bootloader/mcuboot/root-ec-p256.pem`) — a key whose private half is
public in the open-source MCUboot repo. That's fine for bring-up (this is
exactly why MCUboot ships it, and it's what makes `imgtool sign` "just
work" with zero setup below), but it means MCUboot will happily boot an
image signed by *anyone* using that same well-known key. Before pointing a
real fleet at a real backend, generate a project key pair with `imgtool
keygen`, set `CONFIG_BOOT_SIGNATURE_KEY_FILE` to the public half's path in
`samples/https_init/sysbuild/mcuboot/prj.conf`, and make sure only the
firmware-upload path on the `dovecote` side (or whatever signs release
images) ever touches the private half.

### Fallback / revert behavior

`pigeon_fota_apply()` schedules a one-time MCUboot **test-swap**, not a
permanent swap — this is what makes a bad update self-healing:

1. Shadow requests a new `firmware.version` → device downloads, verifies
   sha256, and schedules the test-swap (secondary slot marked
   pending-test, not yet confirmed).
2. Device reports shadow convergence, disconnects LTE, and cold-reboots.
   MCUboot swaps the new image into the primary slot and boots it *once*
   without marking it permanent.
3. If the new image boots and its first `shadow_sync()` succeeds,
   `pigeon_fota_confirm_boot()` calls `boot_write_img_confirmed()` and the
   swap becomes permanent — MCUboot will keep booting this image on future
   resets.
4. If the new image never reaches a successful `shadow_sync()` (crash,
   boot loop, LTE failure, wrong signing key) before the next reset,
   MCUboot reverts: the *un*confirmed image is swapped back out and the
   previous (previously-confirmed) image boots instead, with no server
   involvement needed. This is the same mechanism `west build`/`west
   flash`'s "test image" workflow relies on generally — see MCUboot's own
   docs on swap-type "test" vs "permanent" if you want to force a revert
   manually while bench-testing (flash an unconfirmed image and just power
   cycle without ever calling `pigeon_fota_confirm_boot()`).

**Not yet verified against real hardware or a live backend** as of this
writing: `dovecote`'s `/device/pigeons/:id/firmware` route (task #23) is
still in flight, so the code above has only been build-verified (`west
build` exit 0, MCUboot image signs) — no chunk has actually been
downloaded, flashed, or booted on a CircuitDojo nRF9160 Feather yet. Update
this section with a real hardware e2e result (mirroring the log-upload
section's "Verified end-to-end on real hardware" note above) once that
lands.
