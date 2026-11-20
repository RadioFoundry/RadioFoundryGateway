# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# RadioFoundry Gateway on an ESP32-S3: the radios from one box config, an SDR receiver (an RTL-SDR on the
# USB host, sdr), a roamer or repeater (an MMDVM modem on USB or a UART, common), or one of each. common,
# sdr, rtlsdr and components (netclock, wifilist, tinysctp) are components from outside the project.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../common"
    "${CMAKE_CURRENT_LIST_DIR}/../sdr"
    "${CMAKE_CURRENT_LIST_DIR}/../rtlsdr"
    "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(radiofoundry_gateway)

# The image is built for size (-Os); the SDR's per-sample code for speed (CONFIG_RDOF_SDR_FAST: -O2 for sdr and
# esp-dsp, and for the DSP files in main, set in main/CMakeLists.txt).
if(CONFIG_RDOF_SDR AND CONFIG_RDOF_SDR_FAST)
    idf_component_get_property(sdr_lib sdr COMPONENT_LIB)
    target_compile_options(${sdr_lib} PRIVATE -O2)
    if(CONFIG_RDOF_SDR_ESP_DSP)
        idf_component_get_property(dsp_lib espressif__esp-dsp COMPONENT_LIB)
        target_compile_options(${dsp_lib} PRIVATE -O2)
    endif()
endif()

# Internal RAM against its budget after each link (tools/ram_budget.py): a build that takes more fails. The budget's
# name is the build dir's (build/w12: w12) unless RDOF_RAM_BUDGET names it; a name with no budget passes.
if(NOT RDOF_RAM_BUDGET)
    get_filename_component(RDOF_RAM_BUDGET "${CMAKE_BINARY_DIR}" NAME)
endif()
idf_build_get_property(rdof_python PYTHON)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${rdof_python} "${CMAKE_CURRENT_LIST_DIR}/../tools/ram_budget.py" "${CMAKE_BINARY_DIR}"
            --profile "${RDOF_RAM_BUDGET}" --if-budgeted
    VERBATIM)
