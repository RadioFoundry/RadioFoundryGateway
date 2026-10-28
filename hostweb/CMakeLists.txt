# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rdof_hostweb: the firmware's web page and status API for a host tool (POSIX sockets and pthreads). A static
# library for the host build only; the firmware has its own server. The page and the scripts it loads are the
# firmware's files, embedded at build time (embed.cmake), not copies.
find_package(Threads REQUIRED)
set(RDOF_HOSTWEB_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(RDOF_HOSTWEB_PAGES
    "/=${RDOF_HOSTWEB_ROOT}/esp_idf/main/web/index.html"
    "/netclock.js=${RDOF_HOSTWEB_ROOT}/components/netclock/web/netclock.js"
    "/wifilist.js=${RDOF_HOSTWEB_ROOT}/components/wifilist/web/wifilist.js")
set(RDOF_HOSTWEB_ASSETS_C ${CMAKE_CURRENT_BINARY_DIR}/hostweb_assets.c)
set(RDOF_HOSTWEB_DEPS)
foreach(p ${RDOF_HOSTWEB_PAGES})
    string(REGEX REPLACE "^[^=]*=" "" f "${p}")
    list(APPEND RDOF_HOSTWEB_DEPS ${f})
endforeach()
add_custom_command(
    OUTPUT ${RDOF_HOSTWEB_ASSETS_C}
    COMMAND ${CMAKE_COMMAND} "-DOUT=${RDOF_HOSTWEB_ASSETS_C}" "-DFILES=${RDOF_HOSTWEB_PAGES}" -P ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
    DEPENDS ${RDOF_HOSTWEB_DEPS} ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
    VERBATIM)
add_library(rdof_hostweb STATIC src/hostweb.c ${RDOF_HOSTWEB_ASSETS_C})
target_include_directories(rdof_hostweb PUBLIC include PRIVATE src)
target_link_libraries(rdof_hostweb PUBLIC rdof_common Threads::Threads)
set_target_properties(rdof_hostweb PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rdof_hostweb PRIVATE -Wall -Wno-unused-parameter)
target_compile_definitions(rdof_hostweb PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE _DEFAULT_SOURCE)
if(RDOF_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(rdof_hostweb PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
