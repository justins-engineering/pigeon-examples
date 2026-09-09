cmake_minimum_required(VERSION 3.20.0)

# Either authentication mode is selectable at build time.
set(SAMPLE_TLS verify psk)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt_init)

# The broker's trust anchor, included by main.c as a string literal on a
# certificate build. A broker with its own development CA is pointed at that
# file instead: -DPIGEON_MQTT_CA_FILE=<pem>.
set(PIGEON_MQTT_CA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cert/isrg-root-x2.pem
    CACHE FILEPATH "PEM trust anchor for the MQTT broker's certificate")

generate_inc_file_for_target(
  app ${PIGEON_MQTT_CA_FILE}
  ${ZEPHYR_BINARY_DIR}/include/generated/broker-ca.pem.hex
)

target_sources(app PRIVATE src/main.c src/shadow.c)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/net.cmake)
