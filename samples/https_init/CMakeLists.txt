cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pigeon_module.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(https_init)

# Generate hex files from root certificates
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
  src/net/connection_manager.c
)
