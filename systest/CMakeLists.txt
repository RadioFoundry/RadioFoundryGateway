# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The system tests (docs/spec/system-tests.md). radiosim is built from a RadioFoundry checkout:
#   cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry   (or RADIOFOUNDRY_DIR in the environment),
#   a checkout of https://github.com/RadioFoundry/RadioFoundry at the commit in radiofoundry.ref
# Without one, these tests are left out.

set(RF ${RADIOFOUNDRY_DIR})
set(RADIOSIM_SRCS
    radiosim/radiosim.c
    ${RF}/core/hotspot/mmdvm.c
    ${RF}/core/hotspot/hotspot.c
    ${RF}/core/hotspot/hotspotUplink.c
    ${RF}/core/hotspot/hotspotData.c
    ${RF}/core/hotspot/dmrDataFrame.c
    ${RF}/core/dmr/fec/dmrFec.c
    ${RF}/core/dmr/call/dmr_lc.c
    ${RF}/core/hostlink/hostlink_policy.c
    ${RF}/core/radio/morse.c
    ${RF}/tests/host/sim/dmr_sim_modem.c)
foreach(f ${RADIOSIM_SRCS})
    if(NOT IS_ABSOLUTE ${f})
        set(f ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    endif()
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "RADIOFOUNDRY_DIR=${RF}: ${f} is missing (a RadioFoundry checkout this doesn't match)")
    endif()
endforeach()

add_executable(radiosim ${RADIOSIM_SRCS})
target_include_directories(radiosim PRIVATE ${RF}/core ${RF}/drivers ${RF}/hal ${RF}/tests/host/sim)
set_target_properties(radiosim PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
# RadioFoundry's sources are its own: built without our warnings. (Its host tests define __attribute__ away for
# the firmware's linker sections; none of these files has one, and on Linux that would break glibc's headers.)
target_compile_options(radiosim PRIVATE -w)

add_executable(test_radiosim test_radiosim.c radiolink.c)
target_link_libraries(test_radiosim PRIVATE rdof_common m)
set_target_properties(test_radiosim PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(test_radiosim PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_radiosim PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_radiosim PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME radiosim COMMAND test_radiosim $<TARGET_FILE:radiosim>)
