# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# tinysctp: an ESP-IDF component when ESP_PLATFORM is set, else a static library
# (the core, and the UDP glue unless TINYSCTP_UDP is OFF).
if(ESP_PLATFORM)
    idf_component_register(
        SRCS src/sctp.c src/crc32c.c src/udp.c src/esp.c
        INCLUDE_DIRS include
        PRIV_REQUIRES lwip freertos esp_hw_support log)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TSCTP_MAX_STREAMS=${CONFIG_TINYSCTP_MAX_STREAMS}
        TSCTP_RX_WINDOW=${CONFIG_TINYSCTP_RX_WINDOW})
else()
    cmake_minimum_required(VERSION 3.16)
    project(tinysctp C)
    option(TINYSCTP_UDP "Build the UDP encapsulation glue (BSD sockets)" ON)
    set(TINYSCTP_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/sctp.c ${CMAKE_CURRENT_LIST_DIR}/src/crc32c.c)
    if(TINYSCTP_UDP)
        list(APPEND TINYSCTP_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/udp.c)
    endif()
    add_library(tinysctp STATIC ${TINYSCTP_SRCS})
    target_include_directories(tinysctp PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    set_target_properties(tinysctp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(tinysctp PRIVATE -Wall -Wextra)
    if(NOT APPLE AND NOT WIN32)
        target_compile_definitions(tinysctp PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    endif()
endif()
