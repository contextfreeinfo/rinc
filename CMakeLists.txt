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
  set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB) # USB host on the USB-A ports
  include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()

# with MSVC, the static C runtime: one self-contained exe (other compilers ignore this; it has to come
# before project(), which is also before MSVC is known)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
add_library(rimcore STATIC src/core/rim.c src/core/console.c src/core/font.c src/core/audio.c)
target_include_directories(rimcore PUBLIC src/core)
target_link_libraries(rimcore PUBLIC riocore)
if(NOT MSVC)
  target_compile_options(rimcore PRIVATE -Wall -Wextra)
endif()

if(RIM_PLATFORM STREQUAL rp2350)
  target_compile_definitions(riocore PUBLIC RIO_SMALL RIO_MAX_FFI=64) # MCU-sized fixed caps, but room for Rim's host procs
  option(RIM_RIO_IN_RAM "run rio's interpreter loop from SRAM instead of flash" ON)
  if(RIM_RIO_IN_RAM)
    target_compile_definitions(riocore PRIVATE RIO_HOT_SECTION=".time_critical.rio")
  endif()
  # video.c: HDMI out through the HSTX, on core 1. keyboard.c: TinyUSB's host stack over PIO-USB, its
  # sources compiled here (tinyusb_host would turn the native port into a host too: it stays a device,
  # for the serial port); tusb_config.h in src/rp2350 configures both
  set(TUSB ${PICO_SDK_PATH}/lib/tinyusb/src)
  add_executable(rim src/rp2350/main.c src/rp2350/video.c src/rp2350/keyboard.c src/rp2350/audio.c
    ${TUSB}/host/usbh.c ${TUSB}/host/hub.c ${TUSB}/class/hid/hid_host.c ${TUSB}/portable/raspberrypi/pio_usb/hcd_pio_usb.c)
  target_include_directories(rim BEFORE PRIVATE src/rp2350)
  target_compile_definitions(rim PRIVATE PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)
  pico_generate_pio_header(rim ${CMAKE_CURRENT_LIST_DIR}/src/rp2350/i2s.pio) # audio.c: sound out of the 3.5 mm jack
  target_link_libraries(rim PRIVATE rimcore pico_stdlib pico_multicore hardware_dma hardware_irq hardware_clocks hardware_pio hardware_i2c
    tinyusb_pico_pio_usb)
  pico_enable_stdio_usb(rim 1)  # the REPL talks over USB serial
  pico_enable_stdio_uart(rim 0)
  pico_add_extra_outputs(rim)   # rim.uf2, to drop on the board in BOOT mode
  # cmake --build build --target flash: picotool reboots a running rim (or anything else built with
  # USB stdio) into BOOT mode over USB, writes rim, and starts it. A board that isn't answering on USB
  # needs BOOT + RESET by hand first.
  if(TARGET picotool)
    add_custom_target(flash COMMAND picotool load -f -x $<TARGET_FILE:rim> DEPENDS rim USES_TERMINAL)
  endif()
else()
  add_executable(rim src/desktop/main.c)
  target_link_libraries(rim PRIVATE rimcore)
  # the window: SDL3, built from source here and linked in statically, for a single binary
  option(RIM_SDL "a window, through SDL3 (off: headless only)" ON)
  if(RIM_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    foreach(off CAMERA HAPTIC SENSOR POWER DIALOG TRAY GPU HIDAPI)
      set(SDL_${off} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
      URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
    FetchContent_MakeAvailable(SDL3)
    target_link_libraries(rim PRIVATE SDL3::SDL3-static)
    target_compile_definitions(rim PRIVATE RIM_SDL=1)
  endif()
endif()
