cmake_minimum_required(VERSION 3.20.0)

# This sample verifies the platform's certificate chain.
set(SAMPLE_TLS verify)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ws_init)

# The platform's root CA, included by main.c as a string literal.
generate_inc_file_for_target(
  app ${CMAKE_CURRENT_SOURCE_DIR}/cert/GTS_Root_R4.crt
  ${ZEPHYR_BINARY_DIR}/include/generated/GTS_Root_R4.crt.hex
)

target_sources(app PRIVATE src/main.c src/shadow.c src/heap_monitor.c)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/net.cmake)
