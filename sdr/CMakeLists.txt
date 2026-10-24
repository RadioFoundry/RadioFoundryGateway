# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The SDR receive path: plain C11, libc and libm only, on top of common.
# As an ESP-IDF component when ESP_PLATFORM is set (add this directory and
# ../common to EXTRA_COMPONENT_DIRS), else a static library rdof_sdr for a host
# build (the caller adds ../common first, for the rdof_common target).
set(RDOF_SDR_SRCS
    src/fft.c
    src/iq.c
    src/channelizer.c
    src/fm.c
    src/demod.c
    src/agc.c
    src/receiver.c
    src/status.c
    src/bench.c
    src/config.c
    src/spectrum.c
    src/ppm.c
    src/rate.c
    src/frontend.c
    src/resample.c
    src/mod.c
    src/sdrmodem.c
    src/loopback.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RDOF_SDR_SRCS} INCLUDE_DIRS include REQUIRES common LDFRAGMENTS linker.lf)
    # Kconfig (CONFIG_RDOF_FEATURE_SDR*) -> the plain RDOF_FEATURE_SDR* switches in rdof/sdr/features.h.
    foreach(feature SDR SDR_AGC SDR_BUILTIN_FFT SDR_RECEIVER)
        if(CONFIG_RDOF_FEATURE_${feature})
            target_compile_definitions(${COMPONENT_LIB} PUBLIC RDOF_FEATURE_${feature}=1)
        else()
            target_compile_definitions(${COMPONENT_LIB} PUBLIC RDOF_FEATURE_${feature}=0)
        endif()
    endforeach()
else()
    cmake_minimum_required(VERSION 3.16)
    project(rdof_sdr C)
    add_library(rdof_sdr STATIC ${RDOF_SDR_SRCS})
    target_include_directories(rdof_sdr PUBLIC include)
    target_link_libraries(rdof_sdr PUBLIC rdof_common)
    set_target_properties(rdof_sdr PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(rdof_sdr PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
