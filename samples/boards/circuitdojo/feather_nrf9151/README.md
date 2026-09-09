# circuitdojo_feather_nrf9151

Circuit Dojo's own Zephyr board definition for the nRF9151 Feather, adopted verbatim
(Apache-2.0) and unmodified except for this README:

    https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers
    branch: v3.4.x
    commit: c248cc5821ab269f162bdb8edd3a23311f2f5364
    path:   boards/circuitdojo/feather_nrf9151

The `v3.4.x` branch matches the nRF Connect SDK revision `samples/west-ncs.yml` pins, so the
`nrf9151_laca` SoC and the `SOC_NRF9151_LACA` symbol this definition targets both exist in the
tree that builds it. The pinned Zephyr carries only nRF9160 variants of the Feather, which is why
this board lives here rather than arriving with `west update`.

`samples/zephyr/module.yml` registers `samples/boards` as a board root for the application and the
sysbuild stage alike, so a build needs no `BOARD_ROOT` on the command line.

## Board targets

- `circuitdojo_feather_nrf9151/nrf9151/ns`, the non-secure application image beside a TF-M secure
  image. This is what every sample builds.
- `circuitdojo_feather_nrf9151/nrf9151`, a secure single image with no TF-M or MCUboot split.

## Flashing and the console

The board carries an onboard RP2040 CMSIS-DAP debug probe, and the same USB device presents a
CDC-ACM port that is the console at 115200 baud:

```sh
west flash -d build -r probe-rs
pyserial-miniterm /dev/ttyACM0 115200
```

`probe-rs` is the runner to use. The definition's `jlink` and `pyocd` runner arguments still name
`nRF9160_xxAA`, upstream's own placeholder until J-Link carries the nRF9151.

## Hardware revisions

`board.yml` declares revisions 1, 2 and 3 and defaults to 3. The revision-specific overlays only
add an `sts4x` humidity sensor on revisions 2 and 3, so the default is safe for a build that
touches no sensor. Confirm the physical board's revision before trusting that peripheral.
