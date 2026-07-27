cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pigeon_module.cmake)

# Git-ignored local overrides (real device secrets like CONFIG_PIGEON_TOKEN,
# CONFIG_WIFI_CREDENTIALS_STATIC_SSID/_PASSWORD); auto-merged on top of
# prj.conf if present, no -D flags needed. Must be set before
# find_package(Zephyr) so configuration_files.cmake's EXTRA_CONF_FILE merge
# picks it up from this directory's scope.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi_init)

# Generate hex files from root certificates (own copy of https_init's
# cert/GTS_Root_R4.crt -- this sample has no MCUboot/sysbuild coupling to
# https_init, so it's kept self-contained rather than reaching into a
# sibling sample's directory).
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
zephyr_include_directories(${gen_dir})

foreach(inc_file
  GTS_Root_R4.crt
)
  generate_inc_file_for_target(
    app
    ${CMAKE_CURRENT_SOURCE_DIR}/cert/${inc_file}
    ${gen_dir}/${inc_file}.hex
  )
endforeach()

target_sources(app PRIVATE
  src/main.c
  src/shadow.c
)

# native_sim has no ESP32 WiFi radio to join a network with -- swap in a
# generic conn_mgr-over-NSOS connection manager instead (see
# src/net/native_sim_connection_manager.c's own header comment). Both files
# export the same wifi_connect()/wifi_disconnect() names, so main.c/shadow.c
# need no board-specific code of their own. CONFIG_BOARD_NATIVE_SIM is a
# real CMake variable here, not just a Kconfig symbol: find_package(Zephyr)
# above has already run Kconfig for the board passed to `west build -b`.
if(CONFIG_BOARD_NATIVE_SIM)
  target_sources(app PRIVATE src/net/native_sim_connection_manager.c)
else()
  target_sources(app PRIVATE src/net/wifi_connection_manager.c)
endif()
