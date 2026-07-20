# circuitdojo_feather_nrf9151 (out-of-tree board root)

Adopted verbatim (Apache-2.0) from Circuit Dojo's own Zephyr board definition:

  https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers
  branch: v3.4.x
  commit: c248cc5821ab269f162bdb8edd3a23311f2f5364
  path:   boards/circuitdojo/feather_nrf9151

The `v3.4.x` branch was chosen because it matches this workspace's pinned
`nrf` (sdk-nrf) manifest revision (`samples/west.yml`, `v3.4.0`) — the board
definition targets the `nrf9151_laca`/`nrf9151_ns_laca` SoC dtsi and the
`SOC_NRF9151_LACA` Kconfig symbol, both of which exist in this workspace's
vendored `zephyr/` and `nrf/` checkouts at that pin (confirmed present, not
assumed). No files were modified from upstream except this README.

Not vendored by `west update` (no `nrf9151` board target exists in the
pinned `zephyr/boards/circuitdojo/feather` tree, which only has nrf9160
variants) — this is a manually-adopted out-of-tree `BOARD_ROOT`, wired up
per-build via `-DBOARD_ROOT=<path to samples/boards>` rather than by editing
any existing sample's `CMakeLists.txt`. See `~/pidgeiot`'s CLAUDE.md-style
convention of keeping new hardware bring-up additive; task #13.

## Board targets

- `circuitdojo_feather_nrf9151/nrf9151` — secure, single-image, no
  TF-M/MCUboot split. Used for phase-1 no-network smoke tests.
- `circuitdojo_feather_nrf9151/nrf9151/ns` — non-secure application image,
  needs a companion secure/TF-M image (sysbuild) and MCUboot partitioning.
  Not exercised yet — see phase 2 (task #13).

## Known uncertainties (not yet resolved against the physical unit)

- `board.yml` declares hardware revisions 1/2/3, default 3. The physical
  board's actual silicon/PCB revision was not confirmed before adopting this
  definition — revision-specific overlays only add an `sts4x` humidity
  sensor on rev 2/3 non-secure builds, which doesn't affect boot for a
  no-sensor smoke test, but matters before trusting any sensor peripheral.
- `board.cmake` still points the `jlink`/`pyocd` runners at `nRF9160_xxAA`
  (upstream TODO comment: "change to nRF9151_xxAA when such device is
  available in JLink") — irrelevant here since we flash via `probe-rs`,
  which upstream already points at the correct `nRF9151_xxAA` chip id.
- Console/`zephyr,shell-uart` is `uart0` at 115200 baud, routed through the
  onboard RP2040 CMSIS-DAP debug probe's CDC-ACM bridge (same USB device as
  the debug probe, `2e8a:000c`) — not verified against Circuit Dojo's own
  docs site, only against this board definition's devicetree.
