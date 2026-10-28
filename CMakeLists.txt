# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The Linux (and any host) build: the portable libraries, the host tools (sdr/cli/tools.cmake) and the tests.
#   cmake -S . -B build/linux && cmake --build build/linux && ctest --test-dir build/linux
# The ESP32-S3 firmware builds from esp_idf/ with idf.py instead (docs/reference.md).
cmake_minimum_required(VERSION 3.16)
project(radiofoundry_gateway C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RDOF_BUILD_TESTS "Build the host tests (host-tests/) with the libraries" ON)

if(RDOF_BUILD_TESTS)
    # host-tests builds the libraries itself (common, sdr, rtlsdr, the Rust when cargo is there), each in its
    # sanitized and feature-off variants, and rdofsdr.
    enable_testing()
    add_subdirectory(host-tests)
    # The system tests need a RadioFoundry checkout (systest/CMakeLists.txt)
    set(RADIOFOUNDRY_DIR "$ENV{RADIOFOUNDRY_DIR}" CACHE PATH "A RadioFoundry checkout, for the system tests")
    if(RADIOFOUNDRY_DIR)
        add_subdirectory(systest)
    else()
        message(STATUS "RADIOFOUNDRY_DIR not set: the system tests (systest/) are left out")
    endif()
else()
    add_subdirectory(common common)
    add_subdirectory(sdr sdr)
    add_subdirectory(rtlsdr rtlsdr)
    if(NOT WIN32)
        add_subdirectory(hostweb hostweb)
    endif()
    include(sdr/cli/tools.cmake)
endif()

include(GNUInstallDirs)
install(TARGETS rdofsdr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET rdofradio)
    install(TARGETS rdofradio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
