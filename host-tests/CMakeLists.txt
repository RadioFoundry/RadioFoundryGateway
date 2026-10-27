# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# Host build of common with its golden-vector tests, and of the portable
# parts of the components in components with theirs.
#   cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
cmake_minimum_required(VERSION 3.16)
project(rdof_host_tests C)

add_subdirectory(../common common)

add_executable(rdof_tests test_main.c test_txvr.c test_repeater.c test_split.c test_jitter.c test_guard.c test_benchtx.c test_core.c test_ipgw.c test_ipgw_modes.c fake_radio.c)
target_include_directories(rdof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rdof_tests PRIVATE rdof_common)
set_target_properties(rdof_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_tests PRIVATE -Wall -Wno-unused-parameter)

option(RDOF_SANITIZE "Build with address and undefined behavior sanitizers" ON)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_common PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(rdof_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME rdof_tests COMMAND rdof_tests)

# Last heard: the ring, and the golden tracker scenarios through it.
add_executable(rdof_lastheard_tests test_lastheard.c)
target_include_directories(rdof_lastheard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rdof_lastheard_tests PRIVATE rdof_common)
set_target_properties(rdof_lastheard_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_lastheard_tests PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_lastheard_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_lastheard_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_lastheard_tests COMMAND rdof_lastheard_tests)

# The memory plan (rdof/memplan.h).
add_executable(rdof_memplan_tests test_memplan.c)
target_link_libraries(rdof_memplan_tests PRIVATE rdof_common)
set_target_properties(rdof_memplan_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_memplan_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_memplan_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_memplan_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_memplan_tests COMMAND rdof_memplan_tests)

# The shared components' own tests.
add_subdirectory(../components/tinysctp/test tinysctp)

# The same tests with every RDOF_FEATURE_* off: what's gated must build without it.
file(GLOB RDOF_COMMON_MIN_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/radiofoundry/*.c)
add_library(rdof_common_min STATIC ${RDOF_COMMON_MIN_SRCS})
target_include_directories(rdof_common_min PUBLIC ../common/include)
target_compile_definitions(rdof_common_min PUBLIC RDOF_FEATURE_TX=0 RDOF_FEATURE_DATA_BLOCKS=0 RDOF_FEATURE_TRACKER=0 RDOF_FEATURE_REPEATER=0)
set_target_properties(rdof_common_min PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_common_min PRIVATE -Wall -Wno-unused-parameter)
add_executable(rdof_tests_min test_main.c test_txvr.c test_repeater.c test_split.c test_jitter.c test_guard.c test_benchtx.c test_core.c test_ipgw.c test_ipgw_modes.c fake_radio.c)
target_include_directories(rdof_tests_min PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rdof_tests_min PRIVATE rdof_common_min m)
target_compile_options(rdof_tests_min PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
add_test(NAME rdof_tests_min COMMAND rdof_tests_min)

# The same tests with rust's modules in place of their C files, when cargo is installed.
find_program(RDOF_CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
if(RDOF_CARGO)
    include(../rust/rust.cmake)
    rdof_rust_add(rdof_rust)
    set(RDOF_COMMON_RUST_SRCS ${RDOF_COMMON_MIN_SRCS})
    foreach(m ${RDOF_RUST_MODULES})
        list(REMOVE_ITEM RDOF_COMMON_RUST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/${m}.c)
    endforeach()
    add_library(rdof_common_rust STATIC ${RDOF_COMMON_RUST_SRCS} ${RDOF_RUST_C_SRCS})
    target_include_directories(rdof_common_rust PUBLIC ../common/include)
    set_target_properties(rdof_common_rust PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rdof_common_rust PRIVATE -Wall -Wno-unused-parameter)
    target_link_libraries(rdof_common_rust PUBLIC rdof_rust m)
    set_property(TARGET rdof_rust APPEND PROPERTY INTERFACE_LINK_LIBRARIES rdof_common_rust)
    add_executable(rdof_tests_rust test_main.c test_txvr.c test_repeater.c test_split.c test_jitter.c test_guard.c test_benchtx.c test_core.c test_ipgw.c test_ipgw_modes.c fake_radio.c)
    target_include_directories(rdof_tests_rust PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rdof_tests_rust PRIVATE rdof_common_rust)
    target_compile_definitions(rdof_tests_rust PRIVATE RDOF_RUST=1)
    set_target_properties(rdof_tests_rust PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rdof_tests_rust PRIVATE -Wall -Wno-unused-parameter)
    add_test(NAME rdof_tests_rust COMMAND rdof_tests_rust)
endif()

# The pages' scripts parse (a syntax error leaves a page section empty, and nothing else says so).
find_program(NODE node)
if(NODE)
    file(GLOB RDOF_WEB_JS ${CMAKE_CURRENT_SOURCE_DIR}/../components/*/web/*.js ${CMAKE_CURRENT_SOURCE_DIR}/../esp_idf/web/*.js)
    foreach(js ${RDOF_WEB_JS})
        get_filename_component(name ${js} NAME_WE)
        add_test(NAME js_${name} COMMAND ${NODE} --check ${js})
    endforeach()
endif()

# The components' portable parts (no ESP-IDF): netclock and wifilist.
add_subdirectory(../components/netclock netclock)

# sdr: the SDR receive path.
include(sdr.cmake)

add_subdirectory(../components/wifilist wifilist)
add_subdirectory(../components/gnss gnss)
# lr2021 adds its own tests (LR2021_TESTS), with the sanitizers through LR2021_FLAGS
set(LR2021_TESTS ON)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(LR2021_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
add_subdirectory(../components/lr2021 lr2021)
add_executable(test_netclock ../components/netclock/test/test_netclock.c)
target_link_libraries(test_netclock PRIVATE netclock_portable)
add_executable(test_wifilist ../components/wifilist/test/test_wifilist.c)
target_link_libraries(test_wifilist PRIVATE wifilist_portable)
add_executable(test_gnss ../components/gnss/test/test_gnss.c)
target_link_libraries(test_gnss PRIVATE gnss_portable)
foreach(t netclock wifilist gnss)
    set_target_properties(test_${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(test_${t} PRIVATE -Wall -Wno-unused-parameter)
    if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        if(TARGET ${t}_portable)
            target_compile_options(${t}_portable PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        endif()
        target_compile_options(test_${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
