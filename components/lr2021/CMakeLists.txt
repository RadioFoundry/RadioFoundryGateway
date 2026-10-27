# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# lr2021: a Semtech LR2021 over SPI, its command encodings and its raw I/Q session. An ESP-IDF component when
# ESP_PLATFORM is set (with the ESP-IDF HAL and port); otherwise static libraries for Linux / macOS and the tests.
set(LR2021_PORTABLE_SRCS src/lr2021_cmd.c src/lr2021_tx.c src/lr2021_radio.c src/lr2021_lora.c src/lr2021_fsk.c
    src/lr2021_proto.c src/lr2021_chips.c src/lr2021_board.c src/lr2021_iq_plan.c
    src/lr2021_lora_extra.c src/lr2021_patch.c src/lr2021_patch_image.c src/lr2021_rdof_pram_image.c)
set(LR2021_DRIVER_SRCS src/lr2021.c src/lr2021_tx_stream.c src/lr2021_packet.c src/lr2021_iq.c src/lr2021_patch_load.c)

# linker.lf: the receive and transmit paths in IRAM (6.7 KB). On the W12 the reader took 30% of core 0 with it, 36%
# without, and the heap had no more free either way (2026-10-27).
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${LR2021_PORTABLE_SRCS} ${LR2021_DRIVER_SRCS} port/esp_idf.c hal/esp_idf.c
        INCLUDE_DIRS include
        LDFRAGMENTS linker.lf
        PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(lr2021 C)

# LR2021_FLAGS: extra compile and link flags for every target here (host-tests passes its sanitizers).
function(lr2021_options t)
    set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter ${LR2021_FLAGS})
    get_target_property(type ${t} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_options(${t} PRIVATE ${LR2021_FLAGS})
    endif()
endfunction()

# The encodings alone: no I/O, no port.
add_library(lr2021_portable STATIC ${LR2021_PORTABLE_SRCS})
target_include_directories(lr2021_portable PUBLIC include)
target_link_libraries(lr2021_portable PUBLIC m)
lr2021_options(lr2021_portable)

# The driver without a port or HAL: the tests link the mock, programs a port and a HAL.
add_library(lr2021_driver STATIC ${LR2021_DRIVER_SRCS})
target_link_libraries(lr2021_driver PUBLIC lr2021_portable)
lr2021_options(lr2021_driver)

# The library for Linux / macOS: the driver, the POSIX port, and on Linux the spidev / GPIO HAL. The driver's sources
# go in it, not lr2021_driver: the driver calls the port, and a single-pass linker (GNU ld) won't go back for it.
add_library(lr2021 STATIC port/posix.c ${LR2021_DRIVER_SRCS})
target_link_libraries(lr2021 PUBLIC lr2021_portable)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(lr2021 PRIVATE hal/linux.c)
    target_compile_definitions(lr2021 PUBLIC LR2021_HAL_LINUX=1)
endif()
lr2021_options(lr2021)

# The tests: host-tests sets LR2021_TESTS; standalone, cmake -S <this dir> -B build && ctest --test-dir build.
if(LR2021_TESTS OR CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_library(lr2021_mock STATIC test/mock_hal.c)
    target_include_directories(lr2021_mock PUBLIC test)
    target_link_libraries(lr2021_mock PUBLIC lr2021_driver)
    lr2021_options(lr2021_mock)
    foreach(t lr2021 lr2021_tx lr2021_radio lr2021_lora lr2021_lora_extra lr2021_fsk lr2021_proto lr2021_chips)
        add_executable(test_${t} test/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE lr2021_portable)
        lr2021_options(test_${t})
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    foreach(t lr2021_driver lr2021_tx_stream lr2021_packet lr2021_iq lr2021_patch)
        add_executable(test_${t} test/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE lr2021_mock)
        lr2021_options(test_${t})
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_lr2021_hal_linux test/test_lr2021_hal_linux.c)
        target_link_libraries(test_lr2021_hal_linux PRIVATE lr2021)
        lr2021_options(test_lr2021_hal_linux)
        add_test(NAME lr2021_hal_linux COMMAND test_lr2021_hal_linux)
    endif()
endif()
