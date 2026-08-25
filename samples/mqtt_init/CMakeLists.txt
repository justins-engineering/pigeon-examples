cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pigeon_module.cmake)

# Git-ignored local overrides (this pigeon's id, endpoint, token or PSK
# secret, WiFi credentials); auto-merged on top of prj.conf if present, no -D
# flags needed. Must be set before find_package(Zephyr) so
# configuration_files.cmake's EXTRA_CONF_FILE merge picks it up from this
# directory's scope.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt_init)

# The trust anchor a certificate build verifies the broker against. The
# default is the production chain's root, ISRG Root X2, which is what
# certbot's `--key-type ecdsa --preferred-chain "ISRG Root X2"` anchors the
# platform broker's chain at (see src/net/wifi_connection_manager.c). A
# broker running locally with its own development CA is pointed at that
# instead:
#
#   west build ... -- -DPIGEON_MQTT_CA_FILE=~/pigeonhole/scripts/dev-cert/ca.pem
#
# which is exactly what scripts/test/native-sim-e2e.sh does. A PSK build
# never reads the generated file (there is no certificate to verify), but it
# is generated regardless rather than made conditional -- one less way for a
# mode switch to fail at link time instead of at configure time.
set(PIGEON_MQTT_CA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cert/isrg-root-x2.pem
    CACHE FILEPATH "PEM trust anchor for the MQTT broker's certificate")

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
zephyr_include_directories(${gen_dir})

generate_inc_file_for_target(
  app
  ${PIGEON_MQTT_CA_FILE}
  ${gen_dir}/broker-ca.pem.hex
)

target_sources(app PRIVATE
  src/main.c
  src/shadow.c
)

# Two board-conditional network bring-ups exporting the same
# wifi_connect()/wifi_disconnect() names, so main.c and shadow.c need no
# board-specific code of their own -- the same split wifi_init/ws_init use.
# CONFIG_BOARD_NATIVE_SIM is a real CMake variable here, not just a Kconfig
# symbol: find_package(Zephyr) above has already run Kconfig for the board
# passed to `west build -b`.
if(CONFIG_BOARD_NATIVE_SIM)
  target_sources(app PRIVATE src/net/native_sim_connection_manager.c)
else()
  target_sources(app PRIVATE src/net/wifi_connection_manager.c)
endif()
