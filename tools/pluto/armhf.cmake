# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The PlutoSDR's ARM (Zynq 7010 / 7020, Cortex-A9, hard float) for tools/pluto/build.sh: Debian's
# arm-linux-gnueabihf cross compiler, the firmware's libiio and libad9361 in RDOF_PLUTO_LIBS.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
# libgcc in the binary (the firmware has its own); the firmware's libiio needs libs this link doesn't see
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -Wl,--allow-shlib-undefined")
set(CMAKE_FIND_ROOT_PATH $ENV{RDOF_PLUTO_LIBS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
