# Included by every sample before find_package(Zephyr).

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ../pigeon is a symlink to the library checkout rather than a west project,
# so an edit there is picked up without a west update.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../pigeon)

# A sample without a transport sets SAMPLE_NETWORK OFF before including this.
if(NOT DEFINED SAMPLE_NETWORK)
  set(SAMPLE_NETWORK ON)
endif()

# Under sysbuild an image learns its board from the sysbuild cache, which
# find_package(Zephyr) reads after this file has run.
set(board_target "${BOARD}")
if(NOT board_target AND DEFINED SYSBUILD_CACHE)
  file(STRINGS ${SYSBUILD_CACHE} board_entry REGEX "^BOARD:STRING=")
  string(REGEX REPLACE "^BOARD:STRING=" "" board_target "${board_entry}")
endif()
if(NOT board_target)
  set(board_target "$ENV{BOARD}")
endif()

# The two names Zephyr resolves a board conf under: the whole board target, and
# the same without the SoC qualifier.
string(REPLACE "/" "_" board_conf_names "${board_target}")
string(REGEX REPLACE "^([^/]+)/[^/]+" "\\1" board_short "${board_target}")
string(REPLACE "/" "_" board_short "${board_short}")
list(APPEND board_conf_names ${board_short})
list(REMOVE_DUPLICATES board_conf_names)

# Board-family fragments carry the bring-up every networked sample needs. They
# merge after the sample's own boards/<board>.conf, so a sample cannot override
# them there; the local files below merge last and can.
if(SAMPLE_NETWORK)
  if(board_target MATCHES "^circuitdojo_feather")
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/nrf91.conf)
  endif()
  if(board_target MATCHES "^circuitdojo_feather_nrf9151")
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/nrf9151.conf)
  endif()
  if(board_target MATCHES "^esp32c6_devkitc")
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/esp32c6.conf)
  endif()
  if(board_target MATCHES "^native_sim")
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/native_sim.conf)
  endif()
endif()

# The mbedTLS want lists a credential shape needs. A sample names the shapes it
# uses in SAMPLE_TLS (verify, psk) before including this. The modem boards need
# none of it: the modem terminates TLS and holds the credentials itself.
if(SAMPLE_NETWORK AND board_target MATCHES "^(esp32c6_devkitc|native_sim)")
  foreach(shape IN LISTS SAMPLE_TLS)
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/tls-${shape}.conf)
  endforeach()
endif()

# Endpoint, token and PSK credentials live in the git-ignored prj.local.conf of
# each sample.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
endif()

# Credentials only one board has, in a git-ignored file beside that board's own
# conf. Kconfig echoes a value assigned to a symbol the board does not have, so
# a WiFi key left in prj.local.conf reaches the build log of every board that
# has no WiFi.
foreach(name ${board_conf_names})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${name}.local.conf)
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/${name}.local.conf)
  endif()
endforeach()
