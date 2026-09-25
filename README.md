# pigeon-examples

Zephyr sample applications for [`pigeon`](https://github.com/justins-engineering/pigeon), the
PidgeIoT device client library. Every sample is a working device: it authenticates to the platform,
syncs a device shadow, applies what the dashboard asked for, reports the result back, and sends
telemetry. What separates them is the transport underneath and the one extra thing each one teaches.

Pick the sample whose transport matches the device you are building, copy it, and change the parts
that are yours. Each has its own README with the exact commands for every board it runs on.

## The samples

| Sample | What it shows | nRF9160 | nRF9151 | ESP32-C6 | native_sim |
|---|---|:-:|:-:|:-:|:-:|
| [shadow_model](samples/shadow_model/README.rst) | The shadow structs with no transport under them | yes | yes | yes | yes |
| [https_init](samples/https_init/README.rst) | HTTPS polling, log upload, firmware updates through MCUboot | yes | yes | yes | yes |
| [ws_init](samples/ws_init/README.rst) | A WebSocket beside HTTPS, so a config change arrives as a push | yes | yes | yes | yes |
| [coap_dtls_init](samples/coap_dtls_init/README.rst) | CoAP over DTLS on one long-lived session, pre-shared key | yes | yes | yes | yes |
| [coap_tcp_init](samples/coap_tcp_init/README.rst) | CoAP over TLS/TCP, for a network that will not carry UDP | yes | yes | yes | yes |
| [mqtt_init](samples/mqtt_init/README.rst) | One persistent MQTT session, shadow pushed rather than polled | yes | yes | yes | yes |
| [asset_tracker](samples/asset_tracker/README.rst) | GNSS position as telemetry | yes | yes | simulated | simulated |
| [wifi_init](samples/wifi_init/README.rst) | WiFi bring-up with the HTTPS connector, firmware updates opt-in | no | no | yes | yes |
| [nidd_init](samples/nidd_init/README.rst) | Reports over the carrier's Non-IP Data Delivery, inside four radio accesses an hour | yes | no | no | no |

The board targets behind those columns:

- `circuitdojo_feather/nrf9160/ns` and `circuitdojo_feather_nrf9151/nrf9151/ns`, the Circuit Dojo
  cellular Feathers. Both need a SIM with LTE-M data. The nRF9151 board definition is out of tree,
  under `samples/boards`, and needs no flag on the command line. `nidd_init` needs a Verizon NB-IoT
  SIM on the NIDD plan instead of LTE-M data, and NIDD is not self-serve: the line has to be on the
  platform's own ThingSpace account, for an organization enabled for NIDD (see its README).
- `esp32c6_devkitc/esp32c6/hpcore`, on a 2.4 GHz WiFi network. See [docs/esp32c6.md](docs/esp32c6.md).
- `native_sim/native/64`, on the build host, using its own sockets. Nothing to flash and no
  hardware to own, which makes it the fastest way to watch a sample talk to the platform.

Only the nRF91 modem has a GNSS receiver, so `asset_tracker` reports a fabricated track elsewhere
and says so in every reading.

One more sample is a bench tool rather than a device: [nidd_probe](samples/nidd_probe/README.rst)
checks a carrier's Non-IP Data Delivery over NB-IoT on either Feather, with no platform involved.

## Prerequisites

- The Zephyr SDK and your host's build dependencies, from Zephyr's
  [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
  Stop after its SDK step; the workspace setup below replaces the rest of that page.
- `probe-rs`, to flash an nRF9151 Feather over its onboard probe.
- `nrfutil`, to flash an nRF9160 Feather over a J-Link probe.

Everything else these commands invoke is a Python package that `west packages pip --install`
brings in below: `imgtool` to sign an image, `pyserial-miniterm` for a board console, `esptool`
to flash the ESP32-C6.

## Setup

Two west manifests live in `samples/`, and each needs a topdir of its own because they vendor
incompatible trees. `west.yml` is upstream Zephyr and is the default; `west-ncs.yml` is the nRF
Connect SDK, which the Feathers need for the modem libraries and the TF-M targets.

A clone is already a west workspace: `.west/config` ships with it, so there is no `west init` step
and `west update` fetches the vendored trees straight away. The default topdir, for the ESP32-C6 and
`native_sim`:

```sh
git clone https://github.com/justins-engineering/pigeon-examples.git
cd pigeon-examples
python3 -m venv .venv && source .venv/bin/activate
pip install west
west update
west packages pip --install
git clone https://github.com/justins-engineering/pigeon.git pigeon
```

A second clone, switched to the other manifest before its first update, for the two Feathers:

```sh
git clone https://github.com/justins-engineering/pigeon-examples.git pigeon-examples-ncs
cd pigeon-examples-ncs
python3 -m venv .venv && source .venv/bin/activate
pip install west
west config manifest.file west-ncs.yml
west update
west packages pip --install
ln -s <path to the pigeon checkout> pigeon
```

That `west config` rewrites the tracked `.west/config`, which is the one file a second topdir is
meant to differ in.

`pigeon` is deliberately not a west project. It is a plain checkout at the top of the workspace,
which `samples/common/app.cmake` adds as an extra Zephyr module, so an edit there is picked up by
the next build with no commit and no `west update`. One checkout serves both topdirs, which is why
the second names the first's rather than cloning again.

Building for the ESP32-C6 needs one more one-time step, `west blobs fetch hal_espressif`, which
fetches the prebuilt WiFi binaries the driver links against. It has to follow the
`west packages pip --install` above, whose packages the fetcher itself needs. See
[docs/esp32c6.md](docs/esp32c6.md).

Activate the environment in every terminal that builds:

```sh
source .venv/bin/activate
```

## Credentials

A sample talks to a pigeon, the platform's record of one device. Create an account at
[pidgeiot.com](https://pidgeiot.com), add a flock to group your devices, then add a pigeon to
that flock. Its detail page carries the endpoint to compile in, and mints the device token,
which is shown once when the pigeon is created and once more each time you refresh it.

A device's endpoint and token are real secrets. They are compiled in, from a git-ignored file in the
sample's own directory, and nothing tracked ever carries a value.

`samples/<sample>/prj.local.conf` holds what identifies the pigeon, whichever board it runs on:

```
CONFIG_PIGEON_ENDPOINT="https://api.pidgeiot.com/device/pigeons/<pigeon-id>"
CONFIG_PIGEON_TOKEN="<device-bearer-token>"
```

The CoAP and MQTT samples take a pre-shared key here instead of, or beside, the token; each
sample's own README names the keys it wants. All of them come back once, from the pigeon's create or
token-refresh response, and refreshing revokes what it replaces.

WiFi credentials go in `samples/<sample>/boards/esp32c6_devkitc_hpcore.local.conf`, merged after
`prj.local.conf`. Create the `boards/` directory if the sample does not ship one:

```
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<ssid>"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<password>"
```

The split is not tidiness. Kconfig prints the value assigned to a symbol whose dependencies are
unmet, so a WiFi password left in `prj.local.conf` is echoed verbatim by the build log of every
board that has no WiFi. Never paste a build log into an issue without reading it first.

The values are compiled in, so changing either file needs a rebuild.

## Signing key

Every sample that builds MCUboot verifies an ECDSA P-256 signature before it boots an image. Generate
a key, keep it outside the tree, and export its path in every build shell:

```sh
imgtool keygen -k <path> -t ecdsa-p256
export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>
```

With the variable unset the build still works and warns: the image is signed with MCUboot's own
published development key, which anyone can forge. Fine on a bench, never on a device that leaves
one. [docs/firmware-updates.md](docs/firmware-updates.md) has the rest, including why the key is set
once at the sysbuild level rather than per image.

## Reference

- [docs/firmware-updates.md](docs/firmware-updates.md): how an update runs, what convergence means,
  the attempt budget, revert behaviour, and the signing key.
- [docs/device-logs.md](docs/device-logs.md): decoding the dictionary-encoded logs a device uploads.
- [docs/esp32c6.md](docs/esp32c6.md): board target, blobs, credentials, flashing and the console.
- [docs/coap-conformance.md](docs/coap-conformance.md): developing the CoAP samples against libcoap.
- [docs/mqtt-e2e.md](docs/mqtt-e2e.md): the whole MQTT path on one workstation, broker included.
- [docs/upstream-issues/](docs/upstream-issues): defects found here that belong upstream.
- [SECURITY.md](SECURITY.md): how to report a vulnerability in PidgeIoT.

## License

AGPL-3.0. See [LICENSE](LICENSE).
