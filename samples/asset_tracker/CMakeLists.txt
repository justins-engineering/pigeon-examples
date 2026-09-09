cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(asset_tracker)

# The platform's root CA, included by main.c as a string literal.
generate_inc_file_for_target(
  app ${CMAKE_CURRENT_SOURCE_DIR}/cert/GTS_Root_R4.crt
  ${ZEPHYR_BINARY_DIR}/include/generated/GTS_Root_R4.crt.hex
)

target_sources(app PRIVATE src/main.c src/shadow.c)

# gnss.c talks to nrf_modem_gnss, which exists only on the nRF91 modem.
if(CONFIG_ASSET_TRACKER_SIM_GPS)
  target_sources(app PRIVATE src/gnss_sim.c)
else()
  target_sources(app PRIVATE src/gnss.c)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/net.cmake)
