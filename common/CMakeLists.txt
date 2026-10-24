# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The portable core: plain C11, no ESP-IDF or POSIX calls (libc and libm only).
# As an ESP-IDF component when ESP_PLATFORM is set (add this directory to
# EXTRA_COMPONENT_DIRS), else a static library for a host build.
set(RDOF_COMMON_SRCS
    src/bits.c
    src/fec.c
    src/ambe.c
    src/layer2.c
    src/pdu.c
    src/mmdvm.c
    src/hblink.c
    src/json.c
    src/tracker.c
    src/modem.c
    src/netcalls.c
    src/rawburst.c
    src/jitter.c
    src/txvr.c
    src/duplex.c
    src/repeater.c
    src/split.c
    src/splitops.c
    src/slotgrid.c
    src/jsonp.c
    src/config.c
    src/status.c
    src/oled.c
    src/oled_sdr.c
    src/usbmatch.c
    src/benchguard.c
    src/benchcfg.c
    src/benchtx.c
    src/ipgw.c
    src/ipgwcfg.c
    src/ipgwtx.c
    src/lastheard.c
    src/memplan.c
    src/radiofoundry/dmrFec.c src/radiofoundry/dmrCach.c src/radiofoundry/dmr_lc.c src/radiofoundry/dmrTrellis.c src/radiofoundry/dmrDataCrc.c
)
# split.c's clock arithmetic matches the golden scenarios to the bit: no fused multiply-add.
if(NOT CMAKE_SCRIPT_MODE_FILE)  # ESP-IDF reads this file as a script first, to find the requirements
    set_source_files_properties(src/split.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

if(ESP_PLATFORM)
    # CONFIG_RDOF_RUST: rust's modules (Espressif's Rust toolchain, `espup`) in place of their C files.
    if(CONFIG_RDOF_RUST)
        include(${CMAKE_CURRENT_LIST_DIR}/../rust/rust.cmake)
        foreach(m ${RDOF_RUST_MODULES})
            list(REMOVE_ITEM RDOF_COMMON_SRCS src/${m}.c)
        endforeach()
        list(APPEND RDOF_COMMON_SRCS ${RDOF_RUST_C_SRCS})
    endif()
    idf_component_register(SRCS ${RDOF_COMMON_SRCS} INCLUDE_DIRS include)
    if(CONFIG_RDOF_RUST)
        idf_build_get_property(idf_target IDF_TARGET)
        rdof_rust_add(rdof_rust TARGET xtensa-${idf_target}-none-elf TOOLCHAIN esp BUILD_STD)
        # The Rust calls back into this library (the FEC, RadioFoundry's codes): the two link as a cycle.
        target_link_libraries(${COMPONENT_LIB} PUBLIC rdof_rust)
        set_property(TARGET rdof_rust APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})
    endif()
    # Kconfig (CONFIG_RDOF_FEATURE_*) -> the plain RDOF_FEATURE_* switches in rdof/features.h.
    foreach(feature TX DATA_BLOCKS TRACKER REPEATER)
        if(CONFIG_RDOF_FEATURE_${feature})
            target_compile_definitions(${COMPONENT_LIB} PUBLIC RDOF_FEATURE_${feature}=1)
        else()
            target_compile_definitions(${COMPONENT_LIB} PUBLIC RDOF_FEATURE_${feature}=0)
        endif()
    endforeach()
else()
    cmake_minimum_required(VERSION 3.16)
    project(rdof_common C)
    add_library(rdof_common STATIC ${RDOF_COMMON_SRCS})
    target_include_directories(rdof_common PUBLIC include)
    set_target_properties(rdof_common PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rdof_common PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_library(MATH_LIB m)
    if(MATH_LIB)
        target_link_libraries(rdof_common PUBLIC ${MATH_LIB})
    endif()
endif()
