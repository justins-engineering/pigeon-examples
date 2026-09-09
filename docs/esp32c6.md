# ESP32-C6-DevKitC notes

Every sample builds for this board. All of them except `shadow_model`, which has no transport at
all, join WiFi through `samples/common/net/wifi.c`, so all of them need WiFi credentials here.

## Board target

`esp32c6_devkitc/esp32c6/hpcore`. The SoC has a second, low-power RISC-V core; `hpcore` is the
application core WiFi runs on, and the board's own devicetree already enables the WiFi node, so no
overlay is needed for it.

The radio is 2.4 GHz only. A join that retries forever usually means the network is 5 GHz, or the
SSID or password is wrong.

## One-time setup beyond `west update`

```sh
west packages pip --install
west blobs fetch hal_espressif
```

The first installs `esptool`, which the SoC's own CMake hard-requires and which `pip install west`
does not pull in, along with the `requests` and `jsonschema` the blob fetcher itself needs. Run it
first or the fetch fails on a fresh virtualenv. The second fetches the prebuilt WiFi binaries the
driver links against, which are not git-tracked source.

## Only the default manifest builds this board

`west.yml` is upstream Zephyr and is what the ESP32-C6 builds under. Under `west-ncs.yml` the `nrf`
project is pulled into every sysbuild image, and `nrfxlib/common.cmake` asserts trying to resolve a
Nordic crypto library path for an SoC it has never heard of. That is the nRF Connect SDK's own build
machinery assuming Nordic SoCs workspace-wide, not something a sample can work around, and it is
part of why the two manifests need separate topdirs.

## Credentials

WiFi credentials belong in `samples/<sample>/boards/esp32c6_devkitc_hpcore.local.conf`, git-ignored
and merged after `prj.local.conf`:

```
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"
```

Keeping them out of `prj.local.conf` is what stops them reaching the build log of a board that has
no WiFi. Kconfig prints the value assigned to a symbol whose dependencies are unmet, so a WiFi
password left in the shared file is echoed verbatim by every Feather and `native_sim` build.

`samples/common/boards/esp32c6.conf` ships a `"changeme"` SSID because Zephyr's WiFi credentials
subsystem needs a non-empty string at build time. A build that never got a local file fails to
associate rather than failing quietly.

## Flashing and the console

The DevKitC exposes two USB ports. The USB-JTAG port flashes; the CP210x port is the console.

```sh
west flash -d build
pyserial-miniterm --rts 0 --dtr 0 /dev/ttyUSB0 115200
```

Pass `--esp-device /dev/ttyUSBn` to `west flash` if more than one serial adapter is attached. The
`--rts 0 --dtr 0` matters: the console port drives EN and IO9, so a terminal opened with those
lines asserted parks the chip in download mode instead of letting it boot.

A single-image build prints `SHA-256 comparison failed ... Attempting to boot anyway` at power-on.
The first-stage loader has no digest to check for an image that MCUboot did not sign; a build with
a bootloader under it does not print it.

## Firmware updates

Espressif's MCUboot port is overwrite-only, so a staged image replaces the running one and there is
no previous slot to revert to. See [firmware-updates.md](firmware-updates.md) for what that changes
and for the signing key every MCUboot build on this board verifies.
