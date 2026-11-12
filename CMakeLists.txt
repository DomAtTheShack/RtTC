cmake_minimum_required(VERSION 4.1)
# Radio to Text Connector. RTC_* options and the rtc_core library are short for its name
project(RadioToTextConnector)

set(CMAKE_CXX_STANDARD 20)
# No C++20 modules here. CMake scans for them anyway with Ninja, and GCC 16's scanner
# fails on some of our files, which broke the Windows CI build
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Windows: programs and the DLLs copied next to them go in build/bin. Hamlib ships old
# libgcc_s_seh-1.dll and libwinpthread-1.dll, and the compiler runs in build/, where
# Windows would load those before its own and crash
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# CLion 2026.3's MinGW compiles for msvcrt, but its libwinpthread was built for UCRT, so
# a -static link fails (undefined __intrinsic_setjmpex, __ms_vsnprintf). Linking against
# UCRT fixes it. Only the link: its libstdc++ is built for msvcrt's headers. Off by
# default since a consistent MinGW (MSYS2, CI) doesn't need it; on when this one is used
set(RTC_MINGW_CRT_FLAG "")
if(MINGW)
    set(RTC_DEFAULT_UCRT_LINK OFF)
    if(CMAKE_CXX_COMPILER MATCHES "JetBrains/CLion")
        set(RTC_DEFAULT_UCRT_LINK ON)
    endif()
    option(RTC_UCRT_LINK "Link against UCRT (for MinGWs with a UCRT libwinpthread)"
           ${RTC_DEFAULT_UCRT_LINK})
    if(RTC_UCRT_LINK)
        set(RTC_MINGW_CRT_FLAG -mcrtdll=ucrt)
        add_link_options(${RTC_MINGW_CRT_FLAG})
    endif()
endif()

# -DRTC_STATIC=ON (Linux and macOS) links the C++ runtime, Hamlib and libusb into the
# programs. It needs a static Hamlib (libhamlib.a) that pkg-config can find. Windows
# (MinGW) builds always link the runtime statically and use the bundled Hamlib DLLs
option(RTC_STATIC "Link libraries statically where possible" OFF)
option(RTC_TESTS "Build RadioTests and register it with ctest" ON)
# Bluetooth audio for UV-Pro-style radios (docs/bluetooth-audio.md). Linux needs BlueZ's
# libbluetooth and libpulse-simple (libbluetooth-dev libpulse-dev on Debian/Ubuntu)
option(RTC_BLUETOOTH "Bluetooth audio for UV-Pro, GA-5WB and Vero radios" ON)

# Shared code

# Everything except the menus and the radio, shared by the programs and the tests
add_library(rtc_core STATIC
        src/backend.cpp src/backend.h
        src/settings.cpp src/settings.h
        src/testcases.cpp src/testcases.h
        "src/Radio/modem.cpp" "src/Radio/modem.h"
        "src/Radio/aioc.cpp" "src/Radio/aioc.h"
        "src/Radio/fsk.cpp" "src/Radio/fsk.h"
        src/Connector/packet.cpp src/Connector/packet.h
        src/Connector/connector.cpp src/Connector/connector.h
        src/Connector/connector_config.cpp src/Connector/connector_config.h
        src/Connector/agw.cpp src/Connector/agw.h
        src/Connector/sessions.cpp src/Connector/sessions.h
        src/Bluetooth/bt_protocol.cpp src/Bluetooth/bt_protocol.h)
# third_party: nlohmann/json, copied in so builds don't need internet access
target_include_directories(rtc_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/third_party)

# Google's libsbc (Apache 2.0), copied in: the SBC codec for Bluetooth audio
add_library(rtc_sbc STATIC src/third_party/libsbc/sbc.c src/third_party/libsbc/bits.c)
set_target_properties(rtc_sbc PROPERTIES C_STANDARD 11)
target_include_directories(rtc_sbc PUBLIC ${CMAKE_SOURCE_DIR}/src/third_party/libsbc)
target_link_libraries(rtc_core PUBLIC rtc_sbc)
# Lets the Test tools menu and RadioTests find tests/audio when run from the build folder
target_compile_definitions(rtc_core PRIVATE RTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")

# Sessions read the keyboard and direwolf's output on their own threads
find_package(Threads REQUIRED)
target_link_libraries(rtc_core PUBLIC Threads::Threads)

# backend.cpp picks the OS code; each OS needs its own audio library
if(WIN32)
    # ws2_32: sockets, for direwolf's KISS and AGW ports and the main program
    target_link_libraries(rtc_core PUBLIC winmm ws2_32)
elseif(APPLE)
    # PulseAudio (Homebrew's): the device list, and playing the tuning tone
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBPULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
    target_link_libraries(rtc_core PUBLIC PkgConfig::LIBPULSE)
    # CoreAudio: sound card names for direwolf's ADEVICE. IOKit: finding a USB serial port
    target_link_libraries(rtc_core PUBLIC "-framework CoreAudio" "-framework CoreFoundation"
            "-framework IOKit")
else()
    find_package(ALSA REQUIRED)
    target_link_libraries(rtc_core PUBLIC ALSA::ALSA)
endif()

# Bluetooth audio: the connection (rfcomm_*) and the virtual sound card (virtual_audio_*)
# differ by OS. RTC_BLUETOOTH is defined when it's built in
if(RTC_BLUETOOTH AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLUEZ IMPORTED_TARGET bluez)
    pkg_check_modules(PULSE_SIMPLE IMPORTED_TARGET libpulse-simple libpulse)
    if(NOT BLUEZ_FOUND OR NOT PULSE_SIMPLE_FOUND)
        message(WARNING "Bluetooth audio left out: it needs libbluetooth and libpulse-simple "
                "(libbluetooth-dev libpulse-dev). -DRTC_BLUETOOTH=OFF hides this warning")
        set(RTC_BLUETOOTH OFF)
    endif()
endif()
if(RTC_BLUETOOTH)
    target_sources(rtc_core PRIVATE src/Bluetooth/bt_audio.cpp src/Bluetooth/bt_audio.h
            src/Bluetooth/rfcomm.h src/Bluetooth/virtual_audio.h)
    target_compile_definitions(rtc_core PUBLIC RTC_BLUETOOTH)
    if(WIN32)
        target_sources(rtc_core PRIVATE src/Bluetooth/rfcomm_windows.cpp src/Bluetooth/virtual_audio_windows.cpp)
    elseif(APPLE)
        enable_language(OBJCXX)
        pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
        target_sources(rtc_core PRIVATE src/Bluetooth/rfcomm_macos.mm src/Bluetooth/virtual_audio_pulse.cpp)
        set_source_files_properties(src/Bluetooth/rfcomm_macos.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
        target_link_libraries(rtc_core PUBLIC PkgConfig::PULSE_SIMPLE
                "-framework IOBluetooth" "-framework Foundation")
    else()
        target_sources(rtc_core PRIVATE src/Bluetooth/rfcomm_linux.cpp src/Bluetooth/virtual_audio_pulse.cpp)
        target_link_libraries(rtc_core PUBLIC PkgConfig::BLUEZ PkgConfig::PULSE_SIMPLE)
    endif()
endif()

# Hamlib:
# - Windows: the bundled build; its DLLs are copied next to the programs
# - RTC_STATIC: a static Hamlib (from `make install`, or built from source as in the CI),
#   with libusb static too, so nothing has to be installed for them
# - Otherwise: the one installed with `make install` on Linux, or Homebrew's on macOS
if(WIN32)
    set(HAMLIB_DIR ${CMAKE_SOURCE_DIR}/hamlib-w64-4.7.2)
    set(HAMLIB_INCLUDE_DIR ${HAMLIB_DIR}/include)
    add_library(hamlib SHARED IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_IMPLIB ${HAMLIB_DIR}/lib/gcc/libhamlib.dll.a
            IMPORTED_LOCATION ${HAMLIB_DIR}/bin/libhamlib-4.dll
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_INCLUDE_DIR})
    # libhamlib-4.dll and the DLLs it needs (libusb, libgcc, libwinpthread)
    file(GLOB HAMLIB_DLLS ${HAMLIB_DIR}/bin/*.dll)
elseif(RTC_STATIC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HAMLIB_PC REQUIRED hamlib)
    find_library(HAMLIB_STATIC_LIBRARY libhamlib.a HINTS ${HAMLIB_PC_STATIC_LIBRARY_DIRS} REQUIRED)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h HINTS ${HAMLIB_PC_STATIC_INCLUDE_DIRS} REQUIRED)
    # What a static Hamlib links against: libusb (for a few USB radios), plus system
    # libraries (libm, libudev, macOS frameworks...), which stay as they are
    set(HAMLIB_DEPS ${HAMLIB_PC_STATIC_LIBRARIES})
    list(REMOVE_ITEM HAMLIB_DEPS hamlib)
    if("usb-1.0" IN_LIST HAMLIB_DEPS)
        find_library(LIBUSB_STATIC_LIBRARY libusb-1.0.a HINTS ${HAMLIB_PC_STATIC_LIBRARY_DIRS} REQUIRED)
        list(TRANSFORM HAMLIB_DEPS REPLACE "^usb-1\\.0$" "${LIBUSB_STATIC_LIBRARY}")
    endif()
    add_library(hamlib STATIC IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_LOCATION ${HAMLIB_STATIC_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_INCLUDE_DIR}
            INTERFACE_LINK_DIRECTORIES "${HAMLIB_PC_STATIC_LIBRARY_DIRS}"
            INTERFACE_LINK_LIBRARIES "${HAMLIB_DEPS}"
            # e.g. -pthread, or libusb's -Wl,-framework,IOKit on macOS
            INTERFACE_LINK_OPTIONS "${HAMLIB_PC_STATIC_LDFLAGS_OTHER}")
else()
    find_library(HAMLIB_LIBRARY hamlib REQUIRED)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h REQUIRED)
    add_library(hamlib UNKNOWN IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_LOCATION ${HAMLIB_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_INCLUDE_DIR})
endif()

# Programs

set(CAT_SOURCES "src/Radio/cat.cpp" "src/Radio/cat.h")

# RadioConnectorCli: the menu program
add_executable(RadioConnectorCli src/main.cpp src/ansi.hpp src/options.h
        src/session.cpp src/session.h ${CAT_SOURCES})

# Headless: one radio to the suite's main program over JSON/TCP (docs/connector-protocol.md)
add_executable(RadioConnector src/Connector/radio_connector.cpp ${CAT_SOURCES})

set(RTC_PROGRAMS RadioConnectorCli RadioConnector)

# Every test in one program: run it with no arguments (RadioTests --help for options)
if(RTC_TESTS)
    enable_testing()
    add_executable(RadioTests src/tests/radio_tests.cpp src/tests/channel.cpp src/tests/channel.h
            ${CAT_SOURCES})
    # The over-the-air tests run RadioConnector from the same folder
    add_dependencies(RadioTests RadioConnector)
    list(APPEND RTC_PROGRAMS RadioTests)
    add_test(NAME RadioTests COMMAND RadioTests)
endif()

foreach(program ${RTC_PROGRAMS})
    target_link_libraries(${program} PRIVATE rtc_core hamlib)
endforeach()

# The test audio goes next to the program, where the Test tools menu looks first
add_custom_command(TARGET RadioConnectorCli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/tests/audio
                $<TARGET_FILE_DIR:RadioConnectorCli>/test-audio)

# direwolf for Windows

# The official Windows download has no Hamlib, so it couldn't key the radio over CAT; this
# builds direwolf (plus atest and gen_packets, for audio files) from source against the
# bundled Hamlib and puts them next to the programs, which look there first. Linux and
# macOS use the installed direwolf, which has Hamlib already.
if(WIN32)
    option(RTC_DIREWOLF "Build direwolf with Hamlib and put it next to the program" ON)
    set(DIREWOLF_SOURCE_DIR ${CMAKE_SOURCE_DIR}/direwolf CACHE PATH "direwolf source to build")
endif()
if(WIN32 AND RTC_DIREWOLF)
    if(NOT EXISTS ${DIREWOLF_SOURCE_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "No direwolf source in ${DIREWOLF_SOURCE_DIR}. Clone it with\n"
                "  git clone --branch 1.8.1 https://github.com/wb2osz/direwolf.git\n"
                "or set -DRTC_DIREWOLF=OFF to build without it.")
    endif()
    include(ExternalProject)
    set(DIREWOLF_BUILD_DIR ${CMAKE_BINARY_DIR}/direwolf-build)
    set(DIREWOLF_PROGRAMS direwolf.exe atest.exe gen_packets.exe)
    list(TRANSFORM DIREWOLF_PROGRAMS PREPEND ${DIREWOLF_BUILD_DIR}/src/ OUTPUT_VARIABLE DIREWOLF_OUTPUTS)
    # windres (for direwolf.rc) starts its preprocessor from its own folder and can't when
    # that has a space in it, e.g. CLion's MinGW under C:\Program Files. So direwolf's build
    # gets its short 8.3 path, when there is one
    set(DIREWOLF_RC_ARGS "")
    get_filename_component(MINGW_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
    if(EXISTS ${MINGW_BIN_DIR}/windres.exe AND MINGW_BIN_DIR MATCHES " ")
        file(WRITE ${CMAKE_BINARY_DIR}/short-path.bat "@echo %~s1\n")
        # Not file(TO_NATIVE_PATH): with MinGW Makefiles it escapes the spaces
        string(REPLACE "/" "\\" WINDRES_NATIVE "${MINGW_BIN_DIR}/windres.exe")
        execute_process(COMMAND ${CMAKE_BINARY_DIR}/short-path.bat ${WINDRES_NATIVE}
                OUTPUT_VARIABLE WINDRES_SHORT OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(TO_CMAKE_PATH "${WINDRES_SHORT}" WINDRES_SHORT)
        if(EXISTS "${WINDRES_SHORT}" AND NOT WINDRES_SHORT MATCHES " ")
            set(DIREWOLF_RC_ARGS -DCMAKE_RC_COMPILER=${WINDRES_SHORT})
        endif()
    endif()
    ExternalProject_Add(direwolf_windows
            SOURCE_DIR ${DIREWOLF_SOURCE_DIR}
            BINARY_DIR ${DIREWOLF_BUILD_DIR}
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                ${DIREWOLF_RC_ARGS}
                -DHAMLIB_INCLUDE_DIR=${HAMLIB_INCLUDE_DIR}
                -DHAMLIB_LIBRARY=${HAMLIB_DIR}/lib/gcc/libhamlib.dll.a
                # The MinGW runtime goes inside the exes, so they only need Hamlib's DLLs
                "-DCMAKE_EXE_LINKER_FLAGS=-static ${RTC_MINGW_CRT_FLAG}"
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${DIREWOLF_BUILD_DIR} --target direwolf atest gen_packets
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${DIREWOLF_OUTPUTS})
endif()

# Linking and the files each program needs next to it

foreach(program ${RTC_PROGRAMS})
    if(WIN32)
        # The exe needs Hamlib's DLLs next to it to start, and direwolf for packets
        set(NEXT_TO_PROGRAM ${HAMLIB_DLLS})
        if(RTC_DIREWOLF)
            add_dependencies(${program} direwolf_windows)
            list(APPEND NEXT_TO_PROGRAM ${DIREWOLF_OUTPUTS})
        endif()
        add_custom_command(TARGET ${program} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NEXT_TO_PROGRAM} $<TARGET_FILE_DIR:${program}>)
    endif()
    if(MINGW)
        # Always link the MinGW runtime into the exe, so an older libwinpthread on PATH
        # (e.g. WSJT-X's) can't be loaded instead. Hamlib's DLLs are still copied next to it
        target_link_options(${program} PRIVATE -static)
    elseif(RTC_STATIC AND NOT APPLE)
        # glibc and ALSA stay shared: a fully static glibc binary breaks ALSA's plugin
        # loading and name lookups. On macOS libc++ is part of the system; only Hamlib and
        # libusb are static
        target_link_options(${program} PRIVATE -static-libgcc -static-libstdc++)
    endif()
endforeach()
