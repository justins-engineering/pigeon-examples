cmake_minimum_required(VERSION 3.20.0)

# This sample verifies the platform's certificate chain.
set(SAMPLE_TLS verify)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(asset_tracker)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cert.cmake)

target_sources(app PRIVATE src/main.c src/shadow.c)

# gnss.c talks to nrf_modem_gnss, which exists only on the nRF91 modem.
if(CONFIG_ASSET_TRACKER_SIM_GPS)
  target_sources(app PRIVATE src/gnss_sim.c)
else()
  target_sources(app PRIVATE src/gnss.c)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/net.cmake)
