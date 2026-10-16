cmake_minimum_required(VERSION 3.13)

# Rinc OS. One project, two builds:
#   RINC_PLATFORM=rp2350   the Adafruit Fruit Jam, with the Pico SDK (the default when PICO_SDK_PATH is set)
#   RINC_PLATFORM=desktop  a native build, for working on the shared code and carts
# RIO_DIR picks the rio checkout: the lib/rio submodule by default, or a working copy while developing both.
set(RIO_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/rio CACHE PATH "rio checkout to build against")
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT RINC_PLATFORM)
  if(PICO_SDK_PATH)
    set(RINC_PLATFORM rp2350)
  else()
    set(RINC_PLATFORM desktop)
  endif()
endif()
set(RINC_PLATFORM ${RINC_PLATFORM} CACHE STRING "rp2350 or desktop")

if(RINC_PLATFORM STREQUAL rp2350)
  set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")
  set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB) # USB host on the USB-A ports
  include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()

# with MSVC, the static C runtime: one self-contained exe (other compilers ignore this; it has to come
# before project(), which is also before MSVC is known)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(rinc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(RINC_PLATFORM STREQUAL rp2350)
  pico_sdk_init()
endif()

include(${RIO_DIR}/library.cmake)
add_library(rinccore STATIC src/core/rinc.c src/core/console.c src/core/font.c src/core/audio.c)
target_include_directories(rinccore PUBLIC src/core)
target_link_libraries(rinccore PUBLIC riocore)
if(NOT MSVC)
  target_compile_options(rinccore PRIVATE -Wall -Wextra)
endif()

if(RINC_PLATFORM STREQUAL rp2350)
  target_compile_definitions(riocore PUBLIC rio_SMALL rio_MAX_FFI=64) # MCU-sized fixed caps, but room for Rinc's host procs
  option(RINC_RIO_IN_RAM "run rio's interpreter loop from SRAM instead of flash" ON)
  if(RINC_RIO_IN_RAM)
    target_compile_definitions(riocore PRIVATE rio_HOT_SECTION=".time_critical.rio")
  endif()
  target_compile_definitions(rinccore PRIVATE rinc_HOT_SECTION=".time_critical.rinc") # the sound mixer, in RAM
  # video.c: HDMI out through the HSTX, on core 1. keyboard.c: TinyUSB's host stack over PIO-USB, its
  # sources compiled here (tinyusb_host would turn the native port into a host too: it stays a device,
  # for the serial port); tusb_config.h in src/rp2350 configures both
  set(TUSB ${PICO_SDK_PATH}/lib/tinyusb/src)
  add_executable(rinc src/rp2350/main.c src/rp2350/video.c src/rp2350/keyboard.c src/rp2350/audio.c src/rp2350/hdmi_packet.c src/rp2350/gamepad.c src/rp2350/sd.c
    lib/fatfs/ff.c lib/fatfs/ffunicode.c
    ${TUSB}/host/usbh.c ${TUSB}/host/hub.c ${TUSB}/class/hid/hid_host.c ${TUSB}/portable/raspberrypi/pio_usb/hcd_pio_usb.c)
  target_include_directories(rinc BEFORE PRIVATE src/rp2350 lib/fatfs) # (sd.c: the SD card, through FatFs)
  # the launcher, a cart built in: where the board starts
  set(launcher ${CMAKE_CURRENT_BINARY_DIR}/launcher_src.c)
  add_custom_command(OUTPUT ${launcher}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_LIST_DIR}/system/launcher.rio -DOUT=${launcher} -DNAME=rinc_launcherSrc
      -P ${CMAKE_CURRENT_LIST_DIR}/tools/embed_text.cmake
    DEPENDS system/launcher.rio tools/embed_text.cmake)
  target_sources(rinc PRIVATE ${launcher})
  target_compile_definitions(rinc PRIVATE PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)
  pico_generate_pio_header(rinc ${CMAKE_CURRENT_LIST_DIR}/src/rp2350/i2s.pio) # audio.c: sound out of the 3.5 mm jack
  target_link_libraries(rinc PRIVATE rinccore pico_stdlib pico_multicore hardware_dma hardware_irq hardware_clocks hardware_pio hardware_i2c hardware_spi
    tinyusb_pico_pio_usb)
  pico_enable_stdio_usb(rinc 1)  # the REPL talks over USB serial
  pico_enable_stdio_uart(rinc 0)
  pico_add_extra_outputs(rinc)   # rinc.uf2, to drop on the board in BOOT mode
  # cmake --build build --target flash: picotool reboots a running rinc (or anything else built with
  # USB stdio) into BOOT mode over USB, writes rinc, and starts it. A board that isn't answering on USB
  # needs BOOT + RESET by hand first.
  if(TARGET picotool)
    add_custom_target(flash COMMAND picotool load -f -x $<TARGET_FILE:rinc> DEPENDS rinc USES_TERMINAL)
  endif()
else()
  add_executable(rinc src/desktop/main.c)
  target_link_libraries(rinc PRIVATE rinccore)
  # the window: SDL3, built from source here and linked in statically, for a single binary
  option(RINC_SDL "a window, through SDL3 (off: headless only)" ON)
  if(RINC_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    foreach(off CAMERA HAPTIC SENSOR POWER DIALOG TRAY GPU)
      set(SDL_${off} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
      URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
    FetchContent_MakeAvailable(SDL3)
    target_link_libraries(rinc PRIVATE SDL3::SDL3-static)
    target_compile_definitions(rinc PRIVATE rinc_SDL=1)
  endif()
  # rinc -web: the web player built in, from a VM-only wasm (python tools/web.py --wasm=rinc.wasm; CI
  # builds one), so making a page needs neither zig nor Python
  set(RINC_WEB_WASM "" CACHE FILEPATH "VM-only rinc.wasm to build in, for rinc -web")
  if(RINC_WEB_WASM)
    get_filename_component(wasm ${RINC_WEB_WASM} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_LIST_DIR}) # (relative: to the source)
    set(player ${CMAKE_CURRENT_BINARY_DIR}/web_player.c)
    add_custom_command(OUTPUT ${player}
      COMMAND ${CMAKE_COMMAND} -DWASM=${wasm} -DPAGE=${CMAKE_CURRENT_LIST_DIR}/src/web/page.html -DOUT=${player}
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/embed.cmake
      DEPENDS ${wasm} src/web/page.html tools/embed.cmake)
    target_sources(rinc PRIVATE ${player})
    target_compile_definitions(rinc PRIVATE rinc_WEB=1)
  endif()

  # tests: every cart runs headless for a few seconds; a cart exported runs the same; with the web
  # player, a page gets made
  enable_testing()
  file(GLOB carts ${CMAKE_CURRENT_LIST_DIR}/carts/*.rio)
  foreach(cart ${carts})
    get_filename_component(name ${cart} NAME_WE)
    add_test(NAME cart_${name} COMMAND rinc ${cart} -frames 180 -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.ppm)
  endforeach()
  set(game ${CMAKE_CURRENT_BINARY_DIR}/breakout-game${CMAKE_EXECUTABLE_SUFFIX})
  add_test(NAME export_native COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/carts/breakout.rio -export ${game})
  add_test(NAME export_native_runs COMMAND ${game} -frames 180 -o ${CMAKE_CURRENT_BINARY_DIR}/breakout-game.ppm)
  set_tests_properties(export_native_runs PROPERTIES DEPENDS export_native)
  # (the exported game draws exactly what rinc does running the cart)
  add_test(NAME export_native_same COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/breakout.ppm ${CMAKE_CURRENT_BINARY_DIR}/breakout-game.ppm)
  set_tests_properties(export_native_same PROPERTIES DEPENDS "export_native_runs;cart_breakout")
  if(RINC_WEB_WASM)
    add_test(NAME export_web COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/carts/breakout.rio -web ${CMAKE_CURRENT_BINARY_DIR}/breakout.html)
  endif()
endif()
