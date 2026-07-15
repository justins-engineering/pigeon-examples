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
