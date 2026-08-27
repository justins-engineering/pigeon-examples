# Shared by every sample that can build MCUboot. It decides which key signs
# the image, and says so when that key is one anybody can sign with.
#
# The signature type itself stays in each sample's own sysbuild config,
# because a board can default it away: esp32c6_devkitc's Kconfig.sysbuild
# defaults the choice to BOOT_SIGNATURE_TYPE_NONE, which leaves MCUboot
# checking an image hash and no signature at all.
#
# The key is a build input, never a commit. Point
# PIGEON_BOOT_SIGNATURE_KEY_FILE at a PEM, or drop one at the untracked path
# below that .gitignore already excludes. With neither, MCUboot falls back to
# its own published development key, whose private half ships in the MCUboot
# repository for anyone to sign with, so the build says so out loud rather
# than letting a bench default follow a board into the field.
#
# Guarded on the bootloader being in the build at all: samples that boot
# without MCUboot have no image for the setting to land on, and setting it
# anyway is a configure-time error rather than a no-op.

if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  if(DEFINED ENV{PIGEON_BOOT_SIGNATURE_KEY_FILE})
    set(pigeon_boot_key "$ENV{PIGEON_BOOT_SIGNATURE_KEY_FILE}")
  else()
    set(pigeon_boot_key "${CMAKE_CURRENT_LIST_DIR}/keys/private/boot-ecdsa-p256.pem")
  endif()

  if(EXISTS "${pigeon_boot_key}")
    set_config_string(mcuboot CONFIG_BOOT_SIGNATURE_KEY_FILE "${pigeon_boot_key}")
  else()
    message(WARNING
      "pigeon: no signing key at ${pigeon_boot_key} and PIGEON_BOOT_SIGNATURE_KEY_FILE is unset, "
      "so this image is signed with MCUboot's public development key and anyone can forge it. "
      "Fine for the bench, never for a device that leaves it."
    )
  endif()
endif()
