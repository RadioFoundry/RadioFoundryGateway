# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# netclock: an NTP / browser disciplined UTC clock. An ESP-IDF component when
# ESP_PLATFORM is set; otherwise a static library of the portable maths
# (netclock_portable) for host tests.
set(NETCLOCK_PORTABLE_SRCS
    src/netclock_model.c
    src/netclock_select.c
    src/netclock_ntp.c
    src/netclock_core.c
    src/netclock_pps.c
    src/netclock_status.c
    src/netclock_tempcal.c
)

if(ESP_PLATFORM)
    set(srcs ${NETCLOCK_PORTABLE_SRCS} src/netclock.c src/netclock_settings.c)
    # Requirements can't depend on Kconfig (ESP-IDF reads them before it has the config).
    set(priv esp_timer esp_netif lwip nvs_flash json esp_driver_tsens esp_driver_gpio)
    if(CONFIG_NETCLOCK_HTTP)
        list(APPEND srcs src/netclock_http.c)
    endif()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS include
        REQUIRES esp_event esp_http_server
        PRIV_REQUIRES ${priv})
    if(CONFIG_NETCLOCK_HTTP)
        # The page section, gzipped at build time and embedded.
        idf_build_get_property(python PYTHON)
        set(js ${CMAKE_CURRENT_LIST_DIR}/web/netclock.js)
        set(js_gz ${CMAKE_CURRENT_BINARY_DIR}/netclock.js.gz)
        add_custom_command(
            OUTPUT ${js_gz}
            COMMAND ${python} -c "import gzip, sys; open(sys.argv[2], 'wb').write(gzip.compress(open(sys.argv[1], 'rb').read(), 9, mtime=0))" ${js} ${js_gz}
            DEPENDS ${js}
            VERBATIM)
        add_custom_target(netclock_js DEPENDS ${js_gz})
        add_dependencies(${COMPONENT_LIB} netclock_js)
        target_add_binary_data(${COMPONENT_LIB} ${js_gz} BINARY)
    endif()
else()
    cmake_minimum_required(VERSION 3.16)
    project(netclock C)
    add_library(netclock_portable STATIC ${NETCLOCK_PORTABLE_SRCS})
    target_include_directories(netclock_portable PUBLIC include)
    set_target_properties(netclock_portable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(netclock_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_library(MATH_LIB m)
    if(MATH_LIB)
        target_link_libraries(netclock_portable PUBLIC ${MATH_LIB})
    endif()
    # Standalone: cmake -S <this dir> -B build && cmake --build build && ctest --test-dir build
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
        add_executable(test_netclock test/test_netclock.c)
        target_link_libraries(test_netclock PRIVATE netclock_portable)
        add_test(NAME netclock COMMAND test_netclock)
    endif()
endif()
