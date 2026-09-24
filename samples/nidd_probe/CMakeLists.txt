cmake_minimum_required(VERSION 3.20.0)

# The probe drives the modem itself, a raw socket on a Non-IP PDN, so it takes
# neither the shared IP bring-up nor its board fragments.
set(SAMPLE_NETWORK OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nidd_probe)

target_sources(app PRIVATE src/main.c)
