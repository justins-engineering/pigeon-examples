cmake_minimum_required(VERSION 3.20.0)

# No transport, so no board network fragment and no bring-up source.
set(SAMPLE_NETWORK OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/app.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(shadow_model)

target_sources(app PRIVATE src/main.c)
