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

# The parrot (rdof/parrot.h).
add_executable(rdof_parrot_tests test_parrot.c)
target_include_directories(rdof_parrot_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rdof_parrot_tests PRIVATE rdof_common)
set_target_properties(rdof_parrot_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_parrot_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_parrot_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_parrot_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_parrot_tests COMMAND rdof_parrot_tests)

# The BrandMeister login (rdof/hbpeer.h with a login), against a fake master in process.
add_executable(rdof_bm_tests test_bm.c)
target_link_libraries(rdof_bm_tests PRIVATE rdof_common)
set_target_properties(rdof_bm_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_bm_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_bm_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_bm_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_bm_tests COMMAND rdof_bm_tests)

# The web API's shared part (rdof/webapi.h), with fake platforms.
add_executable(rdof_webapi_tests test_webapi.c)
target_link_libraries(rdof_webapi_tests PRIVATE rdof_common)
set_target_properties(rdof_webapi_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_webapi_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_webapi_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_webapi_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_webapi_tests COMMAND rdof_webapi_tests)

# The bench's line protocol (rdof/benchline.h, rdof/benchnet.h), with a fake receiver.
add_executable(rdof_benchsig_tests test_benchsig.c)
target_link_libraries(rdof_benchsig_tests PRIVATE rdof_common rdof_sdr m)
set_target_properties(rdof_benchsig_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_benchsig_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_benchsig_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_benchsig_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME benchsig COMMAND rdof_benchsig_tests)

# The carrier envelope (rdof/benchenv.h): synthetic keyings with known edges, ramps and steps.
add_executable(rdof_benchenv_tests test_benchenv.c)
target_link_libraries(rdof_benchenv_tests PRIVATE rdof_common m)
set_target_properties(rdof_benchenv_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_benchenv_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_benchenv_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_benchenv_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME benchenv COMMAND rdof_benchenv_tests)

# A burst's PA ramp (rdof/benchramp.h): synthetic bursts with known overshoot, sag and late key-ups.
add_executable(rdof_benchramp_tests test_benchramp.c)
target_link_libraries(rdof_benchramp_tests PRIVATE rdof_common rdof_sdr m)
target_include_directories(rdof_benchramp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rdof_benchramp_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_benchramp_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_benchramp_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_benchramp_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME benchramp COMMAND rdof_benchramp_tests)

add_executable(rdof_benchline_tests test_benchline.c)
target_link_libraries(rdof_benchline_tests PRIVATE rdof_common)
set_target_properties(rdof_benchline_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_benchline_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_benchline_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_benchline_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_benchline_tests COMMAND rdof_benchline_tests)

# The analog FM generator and receiver (rdof/sdr/fmgen.h, rdof/sdr/fmrx.h), one through the other, and the signal
# modes (rdof/sigmode.h, rdof/sdr/modes.h).
add_executable(rdof_fm_tests test_fm.c)
target_link_libraries(rdof_fm_tests PRIVATE rdof_sdr m)
set_target_properties(rdof_fm_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_fm_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_fm_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_fm_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_fm_tests COMMAND rdof_fm_tests)

# Analog AM as a signal mode (sdr/src/mode_am.c, rdof/sdr/amrx.h): its transmitter through a channel into its
# receiver.
add_executable(rdof_am_tests test_am.c)
target_link_libraries(rdof_am_tests PRIVATE rdof_sdr m)
set_target_properties(rdof_am_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_am_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_am_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_am_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_am_tests COMMAND rdof_am_tests)

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

# The firmware's power key and battery numbers (esp_idf/main/power_core.c).
add_executable(rdof_power_tests test_power.c ../esp_idf/main/power_core.c)
target_include_directories(rdof_power_tests PRIVATE ../esp_idf/main)
set_target_properties(rdof_power_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_power_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_power_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_power_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_power_tests COMMAND rdof_power_tests)

# The shared components' own tests.
add_subdirectory(../components/tinysctp/test tinysctp)

# The same tests with every RDOF_FEATURE_* off: what's gated must build without it.
file(GLOB RDOF_COMMON_MIN_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/radiofoundry/*.c)
list(REMOVE_ITEM RDOF_COMMON_MIN_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../common/src/mssim.c) # a bench tool on data blocks
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
    # The settings schema and the C parser agree (fixtures and key lists).
    add_test(NAME rdof_schema_tests COMMAND ${NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_schema.js)
endif()

# The components' portable parts (no ESP-IDF): netclock and wifilist.
add_subdirectory(../components/netclock netclock)

# sdr: the SDR receive path.
include(sdr.cmake)

# hostweb: the firmware's web page for host tools.
include(web.cmake)

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
if(TARGET netclock_posix)
    add_executable(test_netclock_posix ../components/netclock/test/test_netclock_posix.c)
    target_link_libraries(test_netclock_posix PRIVATE netclock_posix)
endif()
add_executable(test_wifilist ../components/wifilist/test/test_wifilist.c)
target_link_libraries(test_wifilist PRIVATE wifilist_portable)
add_executable(test_gnss ../components/gnss/test/test_gnss.c)
target_link_libraries(test_gnss PRIVATE gnss_portable)
set(component_tests netclock wifilist gnss)
if(TARGET netclock_posix)
    list(APPEND component_tests netclock_posix)
endif()
foreach(t ${component_tests})
    set_target_properties(test_${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(test_${t} PRIVATE -Wall -Wno-unused-parameter)
    if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        if(TARGET ${t}_portable)
            target_compile_options(${t}_portable PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        elseif(TARGET ${t})
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        endif()
        target_compile_options(test_${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
if(TARGET netclock_posix)
    set_tests_properties(netclock_posix PROPERTIES TIMEOUT 30)
endif()

# The timed confirmed-data responder (rdof/mssim.h).
add_executable(rdof_mssim_tests test_mssim.c)
target_link_libraries(rdof_mssim_tests PRIVATE rdof_common)
set_target_properties(rdof_mssim_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_mssim_tests PRIVATE -Wall -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_mssim_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rdof_mssim_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rdof_mssim_tests COMMAND rdof_mssim_tests)
