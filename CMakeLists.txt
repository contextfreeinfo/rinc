cmake_minimum_required(VERSION 3.13)

# Rinc OS. One project, two builds:
#   RINC_PLATFORM=rp2350   the Adafruit Fruit Jam, with the Pico SDK (the default when PICO_SDK_PATH is set)
#   RINC_PLATFORM=desktop  a native build, for working on the shared code and apps
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
  # the firmware's bundle (docs/spec/apps.md#bundles): the apps it carries, home and the console by
  # default (the SD card's apps of the same names take their place); RINC_HOME is the one home means
  set(RINC_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/system/home.rio|${CMAKE_CURRENT_LIST_DIR}/system/console.rio" CACHE STRING
    "the firmware's apps, their .rio files, separated by |")
  set(RINC_HOME "" CACHE STRING "the firmware's home app, by name (by default the only app, else one named home)")
  string(REPLACE "|" ";" bundleApps "${RINC_BUNDLE}")
  set(bundle ${CMAKE_CURRENT_BINARY_DIR}/bundle.c)
  add_custom_command(OUTPUT ${bundle}
    COMMAND ${CMAKE_COMMAND} "-DAPPS=${RINC_BUNDLE}" -DHOME=${RINC_HOME} -DOUT=${bundle} -P ${CMAKE_CURRENT_LIST_DIR}/tools/bundle.cmake
    DEPENDS ${bundleApps} tools/bundle.cmake VERBATIM)
  add_executable(rinc ${bundle} src/rp2350/main.c src/rp2350/video.c src/rp2350/keyboard.c src/rp2350/audio.c src/rp2350/hdmi_packet.c src/rp2350/gamepad.c src/rp2350/sd.c
    lib/fatfs/ff.c lib/fatfs/ffunicode.c
    ${TUSB}/host/usbh.c ${TUSB}/host/hub.c ${TUSB}/class/hid/hid_host.c ${TUSB}/portable/raspberrypi/pio_usb/hcd_pio_usb.c)
  target_include_directories(rinc BEFORE PRIVATE src/rp2350 lib/fatfs) # (sd.c: the SD card, through FatFs)
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
  add_executable(rinc src/desktop/main.c src/desktop/files.c)
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

  # tests: every app runs headless for a few seconds; an app exported runs the same; with the web
  # player, a page gets made
  enable_testing()
  file(GLOB apps ${CMAKE_CURRENT_LIST_DIR}/apps/*.rio)
  foreach(app ${apps})
    get_filename_component(name ${app} NAME_WE)
    add_test(NAME app_${name} COMMAND rinc ${app} -frames 180 -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.ppm)
  endforeach()
  # the built-in apps: home runs, and the console, typed at, runs Rio and its commands
  # storage in a folder of the tests' own (never the user's): apps installed there, then run by name
  set(data ${CMAKE_CURRENT_BINARY_DIR}/test-data)
  add_test(NAME install COMMAND rinc -data ${data} -install ${CMAKE_CURRENT_LIST_DIR}/system/home.rio
    ${CMAKE_CURRENT_LIST_DIR}/system/console.rio ${CMAKE_CURRENT_LIST_DIR}/apps/hello.rio)
  add_test(NAME app_by_name COMMAND rinc -data ${data} -app hello -frames 2)
  set_tests_properties(app_by_name PROPERTIES DEPENDS install PASS_REGULAR_EXPRESSION "drawn at")
  add_test(NAME app_home COMMAND rinc -data ${data} -frames 30)
  set_tests_properties(app_home PROPERTIES DEPENDS install)
  add_test(NAME app_console COMMAND rinc -data ${data} -app console -frames 60
    -type "x := 6 * 7\\nsq :: proc(n: Int) -> Int\\nreturn n * n\\nend\\nsq(x)\\nnope\\nls\\n")
  set_tests_properties(app_console PROPERTIES DEPENDS install PASS_REGULAR_EXPRESSION "1764[\r\n]+> nope[\r\n]+1:1: undefined name")
  # an app's memory: the -m report
  add_test(NAME memory_report COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/apps/chord.rio -m)
  set_tests_properties(memory_report PROPERTIES PASS_REGULAR_EXPRESSION "fits in the 256 KB app space")
  set(game ${CMAKE_CURRENT_BINARY_DIR}/breakout-game${CMAKE_EXECUTABLE_SUFFIX})
  add_test(NAME export_native COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/apps/breakout.rio -export ${game})
  add_test(NAME export_native_runs COMMAND ${game} -frames 180 -o ${CMAKE_CURRENT_BINARY_DIR}/breakout-game.ppm)
  set_tests_properties(export_native_runs PROPERTIES DEPENDS export_native)
  # (the exported game draws exactly what rinc does running the app)
  add_test(NAME export_native_same COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/breakout.ppm ${CMAKE_CURRENT_BINARY_DIR}/breakout-game.ppm)
  set_tests_properties(export_native_same PROPERTIES DEPENDS "export_native_runs;app_breakout")
  # every rio example in the docs, run by a headless rinc (with Rio's runner, once lib/rio has it)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS ${RIO_DIR}/tests/docs.py)
    add_test(NAME docs COMMAND Python3::Interpreter ${RIO_DIR}/tests/docs.py $<TARGET_FILE:rinc>
      ${CMAKE_CURRENT_LIST_DIR}/docs ${CMAKE_CURRENT_LIST_DIR}/README.md -- -frames 2 -data ${CMAKE_CURRENT_BINARY_DIR}/docs-data)
    set_tests_properties(docs PROPERTIES PASS_REGULAR_EXPRESSION "all [0-9]+ doc examples passed")
  endif()
  # pc.rio, the host procs written out, matches what rinc registers
  add_executable(rinc_pc_test tests/pc.c)
  target_link_libraries(rinc_pc_test PRIVATE rinccore)
  add_test(NAME pc_declared COMMAND rinc_pc_test ${CMAKE_CURRENT_LIST_DIR}/src/core/pc.rio)
  set_tests_properties(pc_declared PROPERTIES PASS_REGULAR_EXPRESSION "pc.rio declares all [0-9]+ host procs")
  # a bundle of two apps, exported: it starts at the one -home names
  set(pair ${CMAKE_CURRENT_BINARY_DIR}/pair-game${CMAKE_EXECUTABLE_SUFFIX})
  add_test(NAME export_bundle COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/apps/bounce.rio ${CMAKE_CURRENT_LIST_DIR}/apps/hello.rio -export ${pair} -home hello)
  add_test(NAME export_bundle_runs COMMAND ${pair} -frames 2 -data ${CMAKE_CURRENT_BINARY_DIR}/pair-data)
  set_tests_properties(export_bundle_runs PROPERTIES DEPENDS export_bundle PASS_REGULAR_EXPRESSION "drawn at")
  if(RINC_WEB_WASM)
    add_test(NAME export_web COMMAND rinc ${CMAKE_CURRENT_LIST_DIR}/apps/breakout.rio -web ${CMAKE_CURRENT_BINARY_DIR}/breakout.html)
  endif()
endif()
