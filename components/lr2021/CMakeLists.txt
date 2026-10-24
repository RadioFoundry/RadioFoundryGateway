# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# lr2021: a Semtech LR2021 on an ESP32's SPI bus. An ESP-IDF component when
# ESP_PLATFORM is set; otherwise a static library of the portable command
# encodings (lr2021_portable) for host tests.
set(LR2021_PORTABLE_SRCS src/lr2021_cmd.c src/lr2021_tx.c src/lr2021_radio.c src/lr2021_lora.c src/lr2021_fsk.c
    src/lr2021_proto.c src/lr2021_patch.c src/lr2021_patch_image.c src/lr2021_rdof_pram_image.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${LR2021_PORTABLE_SRCS} src/lr2021.c src/lr2021_tx_stream.c src/lr2021_packet.c src/lr2021_patch_load.c
        INCLUDE_DIRS include
        PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer)
else()
    cmake_minimum_required(VERSION 3.16)
    project(lr2021 C)
    add_library(lr2021_portable STATIC ${LR2021_PORTABLE_SRCS})
    target_include_directories(lr2021_portable PUBLIC include)
    target_link_libraries(lr2021_portable PUBLIC m)
    set_target_properties(lr2021_portable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(lr2021_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # The driver itself, on the host: src/lr2021.c over a shim of the few ESP-IDF calls it makes.
    add_library(lr2021_driver_host STATIC src/lr2021.c src/lr2021_tx_stream.c src/lr2021_packet.c src/lr2021_patch_load.c
        test/esp_shim/esp_shim.c)
    target_include_directories(lr2021_driver_host PUBLIC include test/esp_shim)
    target_link_libraries(lr2021_driver_host PUBLIC lr2021_portable)
    set_target_properties(lr2021_driver_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(lr2021_driver_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # Standalone: cmake -S <this dir> -B build && cmake --build build && ctest --test-dir build
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
        add_executable(test_lr2021 test/test_lr2021.c)
        target_link_libraries(test_lr2021 PRIVATE lr2021_portable)
        add_test(NAME lr2021 COMMAND test_lr2021)
        add_executable(test_lr2021_driver test/test_lr2021_driver.c)
        target_link_libraries(test_lr2021_driver PRIVATE lr2021_driver_host)
        add_test(NAME lr2021_driver COMMAND test_lr2021_driver)
        add_executable(test_lr2021_tx test/test_lr2021_tx.c)
        target_link_libraries(test_lr2021_tx PRIVATE lr2021_portable)
        add_test(NAME lr2021_tx COMMAND test_lr2021_tx)
        add_executable(test_lr2021_tx_stream test/test_lr2021_tx_stream.c)
        target_link_libraries(test_lr2021_tx_stream PRIVATE lr2021_driver_host)
        add_test(NAME lr2021_tx_stream COMMAND test_lr2021_tx_stream)
        add_executable(test_lr2021_packet test/test_lr2021_packet.c)
        target_link_libraries(test_lr2021_packet PRIVATE lr2021_driver_host)
        add_test(NAME lr2021_packet COMMAND test_lr2021_packet)
        add_executable(test_lr2021_patch test/test_lr2021_patch.c)
        target_link_libraries(test_lr2021_patch PRIVATE lr2021_driver_host)
        add_test(NAME lr2021_patch COMMAND test_lr2021_patch)
        add_executable(test_lr2021_radio test/test_lr2021_radio.c)
        target_link_libraries(test_lr2021_radio PRIVATE lr2021_portable)
        add_test(NAME lr2021_radio COMMAND test_lr2021_radio)
        add_executable(test_lr2021_lora test/test_lr2021_lora.c)
        target_link_libraries(test_lr2021_lora PRIVATE lr2021_portable)
        add_test(NAME lr2021_lora COMMAND test_lr2021_lora)
        add_executable(test_lr2021_fsk test/test_lr2021_fsk.c)
        target_link_libraries(test_lr2021_fsk PRIVATE lr2021_portable)
        add_test(NAME lr2021_fsk COMMAND test_lr2021_fsk)
        add_executable(test_lr2021_proto test/test_lr2021_proto.c)
        target_link_libraries(test_lr2021_proto PRIVATE lr2021_portable)
        add_test(NAME lr2021_proto COMMAND test_lr2021_proto)
    endif()
endif()
