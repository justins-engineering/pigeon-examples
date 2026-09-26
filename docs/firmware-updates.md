# Firmware updates

`https_init` carries the reference wiring on every board that boots through MCUboot, and
`wifi_init` has an opt-in firmware-update build. `native_sim` has no bootloader and no second slot,
so firmware updates are off there. `nidd_init` boots through MCUboot but leaves them off: over NIDD
the image still comes by HTTPS, on an IP connection beside the Non-IP one, which needs a SIM plan
with IP data to spare and the pigeon's device token. `pigeon`'s README describes that build.

## How an update runs

The target lives in the shadow's `target_config.firmware`: a `version`, a `size` and a `sha256`,
written by the dashboard's firmware assignment. Every poll compares that version against
`CONFIG_PIGEON_FOTA_CURRENT_VERSION`, the string this build was compiled with. The library never
reads MCUboot's image header, so that string has to name the version the image was uploaded under.

A release therefore bumps two strings, not one. `CONFIG_PIGEON_FOTA_CURRENT_VERSION` is what the
device reports and compares; a sample's `VERSION` file, where it has one, is what `imgtool` stamps
into the image header. Nothing checks that the two agree, so a build whose reported version is
stale chases a target it is already running until the attempt budget stops it.

On a difference the device downloads the image in `CONFIG_PIGEON_FOTA_CHUNK_SIZE` Range requests,
verifies the sha256 of the whole image, writes it into MCUboot's second slot, schedules a one-time
test swap and reboots.

Upload the signed artifact, `zephyr.signed.bin`, never the bare `zephyr.bin`. An unsigned image
downloads and verifies its sha256, is staged, and is then refused at the next boot.

## Convergence means booted, not staged

Staging an image is not the bootloader having accepted it, so the device reports at the platform's
existing `current_version` and only then reboots. The image that comes up reports convergence for
itself on its first successful shadow sync, when its own baked version finally matches what the
shadow asked for.

That costs one poll cycle before a dashboard reads converged, and the cycle is the point: a
bootloader that refuses the image leaves the pigeon visibly unconverged, instead of converged
against firmware it never ran.

A failed download, or a refusal by the attempt budget, reports the same way. The report still goes
out, carrying whatever else in the target the device did apply and the version it is really
running. What the platform does not learn is that the target was met, so the device keeps trying.

## The attempt budget

`CONFIG_PIGEON_FOTA_ATTEMPT_BUDGET` bounds how many times a device chases one target, so an image
that boot-loops cannot re-download itself forever. The budget is keyed on the shadow's
`target_version` rather than on the firmware version string, so writing the shadow anew reopens it
without republishing unchanged bytes under a new label. The dashboard's re-push firmware action
does exactly that.

## Fallback and revert

The swap MCUboot schedules is a test swap, not a permanent one, which is what makes a bad update
self-healing:

1. The device stages the image, reports at the version it is still running, and reboots.
2. MCUboot swaps the new image into the primary slot and boots it once without marking it
   permanent.
3. If that image reaches a successful shadow sync it reports convergence for itself and confirms
   the swap, which is what makes MCUboot keep booting it.
4. If it never reaches one, whether from a crash, a boot loop or a network it cannot join, the
   next reset reverts to the previously confirmed image with no server involvement.

Step 4 needs swap-with-revert, and Espressif's MCUboot port is overwrite-only
(`CONFIG_BOOT_UPGRADE_ONLY=y` in `bootloader/mcuboot/boot/zephyr/socs/esp32c6_hpcore.conf`), so on
the ESP32-C6 a staged image replaces the running one and no previous slot survives to revert to.
Confirming still runs; what it cannot do is make a bad image recoverable without another update. An
image signed with the wrong key is refused before any of this, by the bootloader, on both boards.

## Signing key

Every sample that builds MCUboot verifies an ECDSA P-256 signature before it boots an image, the
ESP32-C6 included. That is not the board's own default: `esp32c6_devkitc`'s `Kconfig.sysbuild`
defaults the choice to `BOOT_SIGNATURE_TYPE_NONE`, which leaves MCUboot checking an image hash and
no signature, so a corrupted download is caught and a substituted one is not. Each sample's
`Kconfig.sysbuild` assigns the type rather than relying on that default.

A key is a build input, never a commit:

```sh
imgtool keygen -k <path> -t ecdsa-p256
export PIGEON_BOOT_SIGNATURE_KEY_FILE=<path>
```

`samples/Kconfig.sysbuild.signing` reads that variable, and it deliberately sets one symbol at the
sysbuild level rather than either image's own key setting. Sysbuild feeds that symbol to two
places: the public half compiled into the bootloader, and the private half `imgtool` signs the
application with. Override only one and the bootloader trusts a key nothing signs with, which no
build catches. Both images compile, both report ECDSA P-256, and every update is then refused at
the reboot after a download that verified perfectly. The symbol wants the private PEM; the build
extracts the public half itself.

With the variable unset the build still works and says so, from `samples/mcuboot-signing.cmake`:

```
CMake Warning: pigeon: PIGEON_BOOT_SIGNATURE_KEY_FILE is unset, so this image is signed with
MCUboot's public development key and anyone can forge it.
```

MCUboot's development key ships in the open-source MCUboot repository, private half and all, which
is exactly why it makes bring-up work with no setup and exactly why an image signed with it proves
nothing. Before pointing a fleet at a real backend, generate a project key and keep the private
half off any machine that does not sign a release image.

## The trap worth knowing first

An image carries `CONFIG_PIGEON_TOKEN` from build time, so an image built before a token rotation
answers 401 on every request once it is booted into, however cleanly the update itself ran. Rebuild
after rotating a token; re-uploading the same artifact changes nothing.
