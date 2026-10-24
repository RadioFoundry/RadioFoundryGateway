# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The Linux (and any host) build: the portable libraries, the rdofsdr tool and the tests.
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
    add_executable(rdofsdr sdr/cli/rdofsdr.c)
    target_link_libraries(rdofsdr PRIVATE rdof_sdr m)
    set_target_properties(rdofsdr PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rdofsdr PRIVATE -Wall -Wno-unused-parameter)

    # rdofsdr's live input (--soapy): SoapySDR when it's installed (Debian / Ubuntu libsoapysdr-dev, Homebrew soapysdr).
    find_path(SOAPYSDR_INCLUDE_DIR SoapySDR/Device.h)
    find_library(SOAPYSDR_LIBRARY SoapySDR)
    if(SOAPYSDR_INCLUDE_DIR AND SOAPYSDR_LIBRARY)
        message(STATUS "SoapySDR found: rdofsdr --soapy")
        target_compile_definitions(rdofsdr PRIVATE RDOF_SOAPY)
        target_include_directories(rdofsdr PRIVATE ${SOAPYSDR_INCLUDE_DIR})
        target_link_libraries(rdofsdr PRIVATE ${SOAPYSDR_LIBRARY})
    else()
        message(STATUS "SoapySDR not found: rdofsdr reads files only")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS rdofsdr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
