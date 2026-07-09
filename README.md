# pigeon-examples

Zephyr/nRF Connect SDK sample applications for [`pigeon`](https://github.com/) (the
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
  https_init/             # pigeon_init() with an HTTPS connector
  coap_init/              # pigeon_init() with a CoAP connector (DTLS PSK fields)
  shadow_model/           # builds pigeon_shadow_doc / pigeon_shadow_update_request
                           # structs and logs them (no transport yet)
```

Each sample is independently buildable; none of them currently enable
`CONFIG_PIGEON` (see note in each `prj.conf`) since `pigeon`'s CoAP/HTTPS
transport source files aren't implemented yet — `pigeon_init()` and the data
structures in `pigeon.h` work regardless, since `pigeon`'s `CMakeLists.txt`
compiles `pigeon_core.c` unconditionally.

## Setup

```sh
python3 -m venv .venv && source .venv/bin/activate
pip install west
west init -l samples
west update
```

## Building a sample

```sh
source .venv/bin/activate
west build -d build samples/https_init -b native_sim/native/64
./build/https_init/zephyr/zephyr.exe
```

Swap `samples/https_init` for `samples/coap_init` or `samples/shadow_model`, and
`-b native_sim/native/64` for a real board (e.g. `circuitdojo_feather/nrf9160/ns`)
when flashing hardware.

The `../pigeon` repo's `.vscode/settings.json` points clangd at
`build/https_init/compile_commands.json` here (via a `pigeon/build` symlink to
this repo's `build/`), so keep `https_init` building under plain `build/` for
IDE tooling to resolve `pigeon`'s includes.
