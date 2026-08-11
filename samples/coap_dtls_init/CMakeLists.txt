cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pigeon_module.cmake)

# Git-ignored local overrides (real device secrets like CONFIG_PIGEON_TOKEN);
# auto-merged on top of prj.conf if present, no -D flags needed. Must be set
# before find_package(Zephyr) so configuration_files.cmake's EXTRA_CONF_FILE
# merge picks it up from this directory's scope.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coap_dtls_init)

target_sources(app PRIVATE
  src/main.c
  src/shadow.c
)

# Three board-conditional network bring-ups, all exporting the same
# lte_connect()/lte_disconnect() names so main.c/shadow.c need no
# board-specific code of their own:
#  - native_sim has no modem (and, in this workspace's default vanilla-
#    Zephyr manifest, no NCS modem/lte_lc.h to even compile against) --
#    swap in a generic conn_mgr-over-NSOS connection manager instead (see
#    src/net/native_sim_connection_manager.c's own header comment).
#  - ESP32-C6 has no modem either, but a real WiFi radio -- reuses ws_init/
#    wifi_init's join/retry logic (see src/net/wifi_connection_manager.c).
#  - everything else (the nRF9160 family) keeps the original LTE bring-up.
if(CONFIG_BOARD_NATIVE_SIM)
  target_sources(app PRIVATE src/net/native_sim_connection_manager.c)
elseif(CONFIG_SOC_SERIES_ESP32C6)
  target_sources(app PRIVATE src/net/wifi_connection_manager.c)
else()
  target_sources(app PRIVATE src/net/connection_manager.c)
endif()
