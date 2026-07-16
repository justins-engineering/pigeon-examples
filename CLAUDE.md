# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`pigeon-examples` holds Zephyr/nRF Connect SDK **sample applications** that exercise `pigeon`, the
PidgeIoT on-device client library. It's a standalone repo (own git history) whose job is to prove
`pigeon` actually works on real firmware — the counterpart to `pigeon`'s own unit-level scaffolding.
`pigeon` itself lives in a sibling checkout at **`~/pigeon`** and is linked in locally (see "The
`pigeon` module" below), not fetched as a normal west project.

**When working on anything shadow/auth/connector-shaped here, read `~/pigeon/CLAUDE.md` first** —
it documents the wire-compat contract with the `~/pidgeiot` backend (dovecote/capsules) that this
repo's samples must stay consistent with. This file only covers workspace mechanics and sample state.

## West workspace structure

This directory *is* the west workspace (`west topdir`), and `samples/` is the manifest ("self")
repo — the one piece of this tree with its own independent git history in the usual sense of "the
repo you'd clone." Everything else here is either vendored (gitignored, fetched by `west update`)
or a small set of local-only support directories:

```
pigeon-examples/            <- west topdir; this repo's git history covers samples/ + these dotfiles
  .west/config               # west topdir marker; manifest = { path: samples, file: west.yml }
  .venv/                      # Python venv (west, pyserial, etc.) — gitignored, `source .venv/bin/activate`
  .envrc                      # direnv: auto-activates .venv, wires up west shell completion
  .clang-format, .clangd       # 2-space Google-based style; clangd config for the ARM Zephyr toolchain
  build/                      # default west build dir — gitignored, see "Build directory conventions"
  pigeon -> ~/pigeon/          # symlink, NOT a west project (see below)
  nidd-sample/                 # gitignored (.gitignore) — pristine reference copy of the Nordic NIDD
                                # sample this workspace was originally bootstrapped from; kept on disk
                                # locally for comparison, not part of this repo or its build
  samples/                    # <-- the actual git repo; west manifest self-path
    west.yml                   # west manifest: pulls sdk-nrf (nrf/) at v3.4.0, which imports
                                # zephyr/, nrfxlib/, mcuboot/, mbedtls/, etc. via name-allowlist
    pigeon_module.cmake         # shared by every sample: ZEPHYR_EXTRA_MODULES += ../pigeon
    https_init/                 # pigeon_init() over HTTPS — the reference/most-developed sample
    coap_tcp_init/               # pigeon_init() over CoAP-over-TLS/TCP, mirrors https_init's shadow
                                  # sync; builds for native_sim + real hardware, not yet wire-compatible
                                  # (see pigeon's CLAUDE.md: backend only speaks coaps:// UDP/DTLS so far)
    shadow_model/                # builds pigeon_shadow_doc/pigeon_shadow_update_request and logs them;
                                  # no transport, exists to sanity-check the data structures compile/link
  nrf/, zephyr/, bootloader/, nrfxlib/, modules/   # vendored by `west update`, gitignored, not this repo
```

`.gitignore` is the authoritative list of what's vendored vs. tracked — when in doubt about whether
something here belongs to this repo, check it before assuming.

### The `pigeon` module

`pigeon` is **intentionally not a west project**. `samples/west.yml` has a comment explaining why:
linking it as a plain filesystem symlink (`pigeon -> ~/pigeon/`) and adding it to
`ZEPHYR_EXTRA_MODULES` by physical path (in `samples/pigeon_module.cmake`, included from every
sample's `CMakeLists.txt`) means local edits in `~/pigeon` are picked up by the very next
`west build` — no commit/push/`west update` round trip needed. This is the mechanism that makes
"stay in sync as the Pigeon Agent changes the module" actually work: a plain rebuild here already
sees their latest working tree, not just their last push.

Consequence: `west update` never touches `pigeon`, and there's no lockfile-style pin recorded
anywhere in this repo for which `pigeon` commit a given build used. If a build here breaks after a
`~/pigeon` change, `git -C ~/pigeon log`/`diff` — not anything in this repo — is where to look.

### MCUboot / sysbuild (`https_init`)

`https_init` boots under MCUboot via sysbuild rather than as a plain standalone app:

- `samples/https_init/sysbuild.conf` — turns on sysbuild's MCUboot image (`SB_CONFIG_BOOTLOADER_MCUBOOT=y`,
  ECDSA P256 signing).
- `samples/https_init/sysbuild/mcuboot/` — the MCUboot child image's own config, scoped to this
  sample: `prj.conf` is a checked-in copy of upstream MCUboot's default `prj.conf` (a comment at the
  top explains why it's a copy and not a symlink — git doesn't like symlinks to paths that don't
  exist until after `west update`), plus a `boards/circuitdojo_feather_nrf9160.conf`/`.overlay` pair
  for board-specific MCUboot settings (RTT console instead of UART so serial recovery/newtmgr can
  still own uart0, `zephyr,code-partition = &boot_partition` so MCUboot links against 0x0 rather
  than the app's default code partition).
- Devicetree-based partitioning (migrated off Partition Manager — see git history): the app-image
  overlay `samples/https_init/boards/circuitdojo_feather_nrf9160_ns.overlay` and the MCUboot-image
  overlay under `sysbuild/mcuboot/boards/` must independently agree on flash layout, since Zephyr's
  devicetree-overlay lookup is a cascading fallback (first `boards/<board>.overlay` found wins), not
  a merge — the two overlays' comments cross-reference each other for exactly this reason. If you
  touch one, check whether the other needs a matching change.
- Sysbuild produces per-image build directories under `build/` (`build/https_init/`,
  `build/mcuboot/`), plus a top-level `build/_sysbuild/` and `build/dfu_application.zip` (the
  combined DFU-able image bundle) — see "Build directory conventions" below.

### Build directory conventions

- Default build dir is `build/` at the workspace root (`west build -d build ...`), not
  `samples/<sample>/build`. This matters beyond convention: `~/pigeon/.vscode/settings.json` points
  clangd at `build/https_init/compile_commands.json` here (via a `pigeon/build` symlink into this
  repo), so `pigeon`'s own IDE tooling depends on `https_init` continuing to build under plain
  `build/`. Don't rename or relocate it without checking that symlink.
- `-p`/`--pristine` forces a clean reconfigure; plain `west build -d build <sample>` reuses the
  existing `build/` if the sample/board match what's already configured there — check
  `build/CMakeCache.txt`'s `CACHED_BOARD` if unsure what a stale `build/` was last configured for.
- Only one sample/board combination is resident in `build/` at a time. Switching sample or board
  needs a fresh `-d` dir or `--pristine`; there's no per-sample build dir convention here (unlike
  some Zephyr workspaces).
- `prj.local.conf` (gitignored, per-sample) holds real device secrets (`CONFIG_PIGEON_ENDPOINT`/
  `CONFIG_PIGEON_TOKEN`) and is auto-merged by each sample's `CMakeLists.txt` if present — never
  commit one or paste its contents into a transcript.

### Related CLAUDE.md files, for style/context

- `~/pigeon/CLAUDE.md` — the pigeon module itself: wire-compat contract with `~/pidgeiot`
  (dovecote/capsules), current transport implementation state, feature-parity gap analysis. Read
  before touching anything shadow/auth/connector-shaped in the samples here. As of 2026-07-15 both
  `pigeon_https.c` and `pigeon_coap.c` are implemented (the latter still uncommitted in `~/pigeon` as
  of this writing) — if that file still says `pigeon_coap.c` doesn't exist, it's lagging the Pigeon
  Agent's actual working tree; check `git -C ~/pigeon log`/`status`, not just that file, for ground
  truth on transport state.
- `~/pidgeiot/dovecote`, `~/pidgeiot/capsules`, `~/pidgeiot/fancier` don't currently have their own
  CLAUDE.md files (checked 2026-07-15) — `~/pigeon/CLAUDE.md`'s citations into their source files
  are the closest thing to that context today.

## `https_init` sample: current state

The most developed sample; treat it as the reference consumer of `pigeon`.

- **`src/main.c`**: brings up LTE (`lte_connect()`), calls `pigeon_init()` with a
  `PIGEON_CONNECTOR_HTTPS` config, then runs `shadow_loop()` forever (does not return under normal
  operation).
- **`src/shadow.c` / `shadow.h`** — device shadow sync: `shadow_sync()` calls `pigeon_shadow_get()`,
  compares `target_version`/`current_version`, and if they differ, JSON-decodes `target_config` into
  an app-defined `{log, telemetry_interval, reboot}` struct (the pigeon library treats
  `target_config` as an opaque string — the app owns its meaning). Applying it: `log` toggles
  runtime log filtering via `log_filter_set()`, `telemetry_interval` changes the shadow poll period,
  and `reboot` is a one-shot command (deliberately excluded from the persisted `current_config` so
  it doesn't refire every poll). `shadow_loop()` re-polls on the shadow's own `telemetry_interval`.
  Each sync also exercises both device→platform report paths, which dovecote now serves (as of
  2026-07-15 — see `~/pigeon/CLAUDE.md` for the wire contract): `pigeon_set_shadow_param()`/
  `pigeon_shadow_flush()` (shared `pigeon_core.c` plumbing over the per-transport
  `pigeon_transport_report_shadow` hook) POSTs an `uptime_s` metric to `/telemetry`, and — only when
  a new target was actually applied — `pigeon_shadow_report()` POSTs the applied `current_config` +
  version back to `/shadow` to ack the config change. Distinct endpoints, distinct purposes:
  telemetry is a latest-value-per-key metric store; the shadow report closes the config-convergence
  loop (`current_version` is reported by the device, never re-derived server-side).
- **`src/net/connection_manager.c`** — `lte_connect()`/`lte_disconnect()`, plus CA cert provisioning
  (`provision_cert()`, modem key storage via `modem_key_mgmt_*` on nRF91 targets since TLS there is
  socket-offloaded to the modem, or `tls_credential_add()` elsewhere). `lte_disconnect()` explicitly
  sends `CFUN=0` (`lte_lc_power_off()`, retried up to 3 times) after bringing the interface down,
  because `conn_mgr_all_if_down()` alone only sends `CFUN=20`, which the modem does *not* count as a
  graceful shutdown for its reset-loop protection — see "Modem reset safety" below.
- **MCUmgr DFU**: `prj.conf` enables `CONFIG_MCUMGR`/`CONFIG_UART_MCUMGR`/image-management groups;
  the `.vscode/tasks.json` "Load image via bootloader" task drives it with `newtmgr -c serial image
  upload build/https_init/zephyr/zephyr.signed.bin` over `/dev/ttyUSB0`.
- **`CONFIG_PIGEON`/`CONFIG_PIGEON_CONNECTOR_HTTPS`** are enabled in `prj.conf`; endpoint/token are
  Kconfig strings supplied via the gitignored `prj.local.conf`, not hardcoded.

### Modem reset safety

Never reset, reflash, or power-cycle hardware while it's mid-connection — always confirm a graceful
`CFUN=0` power-off happened first (or let `lte_disconnect()` complete). An ungraceful reset trips the
nRF91 modem's reset-loop protection and refuses LTE attach for **30 minutes**. This is exactly what
`lte_disconnect()`'s explicit power-off exists to avoid (see `connection_manager.c` above) — don't
work around or skip it when scripting flashes/tests.

### Other samples

- **`coap_tcp_init`** — mirrors `https_init`'s structure (`src/shadow.c` is a copy, now back in
  sync as of `pigeon`'s 82f5233 "Add pigeon_shadow_report() config-ack over HTTPS + CoAP":
  `pigeon_coap.c` implements `pigeon_shadow_report()` too, so this sample's `shadow_sync()` calls
  it after applying `target_config`, same as `https_init`'s. `src/net/connection_manager.c` is the same LTE
  bring-up/graceful-shutdown pattern, minus CA-cert provisioning since PSK credentials are registered
  by `pigeon_coap.c` itself from `pigeon_init()`'s config). `PIGEON_CONNECTOR_COAP`, speaking
  CoAP-over-TLS/TCP (`coaps+tcp://`). No sysbuild/MCUboot, so unlike `https_init` it builds for both
  `native_sim` and `circuitdojo_feather/nrf9160/ns` (verified 2026-07-15; hardware build needed
  `CONFIG_SIZE_OPTIMIZATIONS` instead of `CONFIG_DEBUG_OPTIMIZATIONS` to fit the default,
  non-rebalanced 192 KB nonsecure flash partition — see `prj.conf`'s comment).
  Isn't wire-compatible with the current `~/pidgeiot` backend yet (UDP/DTLS `coaps://` only, and no
  CoAP listener at all) — see `~/pigeon/CLAUDE.md`'s "Known wire-compat gap" note; `shadow_sync()`
  failing against a real backend is expected, not a bug in this sample. **Known gap surfaced while
  building this sample:** `pigeon_coap.c`'s PSK registration always calls `tls_credential_add()`, with
  no `CONFIG_MODEM_KEY_MGMT` branch — on real nRF91 hardware, TLS sockets are offloaded to the modem
  regardless of that setting (see `connection_manager.c`'s `CONFIG_MODEM_KEY_MGMT` comment), so PSK
  credentials never reach the modem's actual credential store there. Documented in this sample's
  `boards/circuitdojo_feather_nrf9160_ns.conf`; a `~/pigeon`-side fix (a `modem_key_mgmt_write()` path
  for PSK) is needed before real-hardware CoAP auth will work, not something to patch from this repo.
- **`shadow_model`** — smallest sample; just builds/logs `pigeon_shadow_doc`/
  `pigeon_shadow_update_request`, no network transport. Good smoke test that the shared data
  structures still compile after a `pigeon` header change, independent of any connector work.

### Native-sim gotcha: no real modem

`connection_manager.c` (both `https_init` and `coap_tcp_init`) guards its graceful-shutdown
`lte_lc_power_off()` call behind `!IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)`. Without that guard the link
fails outright on `native_sim` — `CONFIG_LTE_LINK_CONTROL` isn't available on that SoC (no real modem
to control), so the symbol doesn't exist to link against. This was caught by actually linking (not
just reading Kconfig), since the failure is a linker error, not a compile error — Kconfig alone won't
show it. If you add another sample with LTE bring-up, carry this guard over too.

## Build instructions

```sh
# One-time setup
python3 -m venv .venv && source .venv/bin/activate
pip install west
west init -l samples
west update

# Every session
source .venv/bin/activate   # or let direnv's .envrc do it
```

`coap_tcp_init` and `shadow_model` have no bootloader and build for `native_sim` for fast local
compile checks:

```sh
west build -d build samples/shadow_model -b native_sim/native/64
./build/shadow_model/zephyr/zephyr.exe
```

`https_init` boots under MCUboot via sysbuild (`sysbuild.conf`), and MCUboot doesn't support the
native/`native_sim` SoC (nrfxlib's crypto build asserts on a missing `GCC_M_CPU` for that SoC) — so
unlike the other two samples, **`https_init` only builds for real hardware**, matching how NCS's own
MCUboot+sysbuild cellular samples (e.g. `nrf/samples/cellular/lwm2m_client`) restrict themselves to
real boards via `platform_allow` rather than trying to make MCUboot board-conditional:

```sh
west build -d build samples/https_init -b circuitdojo_feather/nrf9160/ns
```

(Verified 2026-07-15: this board target builds clean — 80% flash, 51% RAM — while
`-b native_sim/native/64` on `https_init` fails at CMake configure time for the reason above. This
was introduced by the same-day "Migrate https_init to devicetree-based MCUboot partitioning"
commit, not by anything in `~/pigeon`; if `https_init` ever needs to build for `native_sim` again,
that commit's `sysbuild.conf` is where to start.)

Flashing/monitoring a real board uses the `.vscode/tasks.json` tasks (`West Flash`, driven by
`nrfutil`; `Serial Monitor`, `pyserial-miniterm` at 1_000_000 baud on `/dev/ttyUSB0`) rather than
plain `west flash` — check that file before improvising flash commands, and see
"Modem reset safety" above before power-cycling attached hardware.

When verifying a change actually builds (not just reviewing it), run the real `west build` command
above and confirm it exits 0 — don't just read the CMake/Kconfig and infer success.

## Conventions

- 2-space indentation, Google base style, 100-column limit (`.clang-format`); `clangd` is configured
  for the `arm-zephyr-eabi` toolchain (`.clangd`).
- Real device secrets (`CONFIG_PIGEON_ENDPOINT`/`CONFIG_PIGEON_TOKEN`, any token/key material) belong
  in the gitignored `prj.local.conf`, never in a tracked `prj.conf`/overlay, and never printed to a
  transcript or committed.
