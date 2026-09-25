cmake_minimum_required(VERSION 3.20.0)

# The sample drives lte_lc itself and brings up no IP, so it takes neither the
# shared IP bring-up nor its board fragments.
set(SAMPLE_NETWORK OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

if(NOT board_target MATCHES "^circuitdojo_feather/nrf9160/ns$")
  message(FATAL_ERROR
    "nidd_init builds for circuitdojo_feather/nrf9160/ns only: Verizon serves NIDD to modules "
    "it supports, and the nRF9151 is not one")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nidd_init)

target_sources(app PRIVATE src/main.c src/shadow.c)
