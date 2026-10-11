cmake_minimum_required(VERSION 3.13)

# Rim OS. One project, two builds:
#   RIM_PLATFORM=rp2350   the Adafruit Fruit Jam, with the Pico SDK (the default when PICO_SDK_PATH is set)
#   RIM_PLATFORM=desktop  a native build, for working on the shared code and carts
# RIO_DIR picks the rio checkout: the lib/rio submodule by default, or a working copy while developing both.
set(RIO_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/rio CACHE PATH "rio checkout to build against")
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT RIM_PLATFORM)
  if(PICO_SDK_PATH)
    set(RIM_PLATFORM rp2350)
  else()
    set(RIM_PLATFORM desktop)
  endif()
endif()
set(RIM_PLATFORM ${RIM_PLATFORM} CACHE STRING "rp2350 or desktop")

if(RIM_PLATFORM STREQUAL rp2350)
  set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")
  include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()

project(rim C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(RIM_PLATFORM STREQUAL rp2350)
  pico_sdk_init()
endif()

include(${RIO_DIR}/library.cmake)
add_library(rimcore STATIC src/core/rim.c)
target_include_directories(rimcore PUBLIC src/core)
target_link_libraries(rimcore PUBLIC riocore)
if(NOT MSVC)
  target_compile_options(rimcore PRIVATE -Wall -Wextra)
endif()

if(RIM_PLATFORM STREQUAL rp2350)
  target_compile_definitions(riocore PUBLIC RIO_SMALL) # MCU-sized fixed caps
  add_executable(rim src/rp2350/main.c)
  target_link_libraries(rim PRIVATE rimcore pico_stdlib)
  pico_enable_stdio_usb(rim 1)  # the REPL talks over USB serial
  pico_enable_stdio_uart(rim 0)
  pico_add_extra_outputs(rim)   # rim.uf2, to drop on the board in BOOT mode
else()
  add_executable(rim src/desktop/main.c)
  target_link_libraries(rim PRIVATE rimcore)
endif()
