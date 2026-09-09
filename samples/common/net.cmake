# Included by every networked sample after project(): compiles the one
# bring-up source the board needs behind net/net_connect.h.

if(CONFIG_SOC_SERIES_NRF91)
  set(net_source lte.c)
elseif(CONFIG_WIFI)
  set(net_source wifi.c)
elseif(CONFIG_NET_NATIVE_OFFLOADED_SOCKETS_CONNECTIVITY_SIM)
  set(net_source native_sim.c)
else()
  message(FATAL_ERROR "samples/common/net has no bring-up for BOARD=${BOARD}")
endif()

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/net/${net_source})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/net)
