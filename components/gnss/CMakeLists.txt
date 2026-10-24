# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# gnss: a GNSS receiver's NMEA on a UART. An ESP-IDF component when ESP_PLATFORM is set; otherwise a static library
# of the portable parts (gnss_portable: the parser, the burst timing, the JSON) for host tests.
set(GNSS_PORTABLE_SRCS src/gnss_nmea.c src/gnss_json.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${GNSS_PORTABLE_SRCS} src/gnss.c
        INCLUDE_DIRS include
        PRIV_REQUIRES esp_driver_uart esp_timer)
else()
    cmake_minimum_required(VERSION 3.16)
    project(gnss C)
    add_library(gnss_portable STATIC ${GNSS_PORTABLE_SRCS})
    target_include_directories(gnss_portable PUBLIC include)
    target_link_libraries(gnss_portable PUBLIC m)
    set_target_properties(gnss_portable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(gnss_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # Standalone: cmake -S <this dir> -B build && cmake --build build && ctest --test-dir build
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
        add_executable(test_gnss test/test_gnss.c)
        target_link_libraries(test_gnss PRIVATE gnss_portable)
        add_test(NAME gnss COMMAND test_gnss)
    endif()
endif()
