# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# RTL2832U + R820T / R828D driver. The register logic (src/) is portable C; esp/ is the
# ESP-IDF USB host glue and sample streaming. As an ESP-IDF component when
# ESP_PLATFORM is set, else a static library rtlsdr of src/ for host tests.
set(RTLSDR_SRCS src/rtl2832u.c src/r820.c src/gaps.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RTLSDR_SRCS} esp/rtl_usb.c
                           INCLUDE_DIRS include
                           REQUIRES usb esp_timer)
else()
    cmake_minimum_required(VERSION 3.16)
    project(rtlsdr C)
    add_library(rtlsdr STATIC ${RTLSDR_SRCS})
    target_include_directories(rtlsdr PUBLIC include)
    set_target_properties(rtlsdr PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rtlsdr PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
