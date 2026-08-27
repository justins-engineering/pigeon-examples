# Shared by every sample that can build MCUboot. Kconfig.sysbuild.signing
# picks the key; this only reports when that key is one anybody can sign with.
#
# The signature type stays in each sample's own sysbuild config, because a
# board can default it away: esp32c6_devkitc's Kconfig.sysbuild defaults the
# choice to BOOT_SIGNATURE_TYPE_NONE, which leaves MCUboot checking an image
# hash and no signature at all.

if(SB_CONFIG_BOOTLOADER_MCUBOOT AND NOT DEFINED ENV{PIGEON_BOOT_SIGNATURE_KEY_FILE})
  message(WARNING
    "pigeon: PIGEON_BOOT_SIGNATURE_KEY_FILE is unset, so this image is signed with MCUboot's "
    "public development key and anyone can forge it. Generate one with "
    "'imgtool keygen -k <path> -t ecdsa-p256', keep it outside the tree, and export that path. "
    "Fine for the bench, never for a device that leaves it."
  )
endif()
