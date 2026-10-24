# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# wifilist: a Wi-Fi station with a prioritized list of networks. An ESP-IDF
# component when ESP_PLATFORM is set; otherwise a static library of the
# portable logic (wifilist_portable) for host tests.
set(WIFILIST_PORTABLE_SRCS src/wifilist_core.c)

if(ESP_PLATFORM)
    set(srcs ${WIFILIST_PORTABLE_SRCS} src/wifilist.c src/wifilist_settings.c)
    if(CONFIG_WIFILIST_HTTP)
        list(APPEND srcs src/wifilist_http.c)
    endif()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS include
        REQUIRES esp_netif esp_http_server
        PRIV_REQUIRES esp_wifi esp_event esp_timer nvs_flash json)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WIFILIST_MAX=${CONFIG_WIFILIST_MAX_NETWORKS})
    if(CONFIG_WIFILIST_HTTP)
        # The page section, gzipped at build time and embedded.
        idf_build_get_property(python PYTHON)
        set(js ${CMAKE_CURRENT_LIST_DIR}/web/wifilist.js)
        set(js_gz ${CMAKE_CURRENT_BINARY_DIR}/wifilist.js.gz)
        add_custom_command(
            OUTPUT ${js_gz}
            COMMAND ${python} -c "import gzip, sys; open(sys.argv[2], 'wb').write(gzip.compress(open(sys.argv[1], 'rb').read(), 9, mtime=0))" ${js} ${js_gz}
            DEPENDS ${js}
            VERBATIM)
        add_custom_target(wifilist_js DEPENDS ${js_gz})
        add_dependencies(${COMPONENT_LIB} wifilist_js)
        target_add_binary_data(${COMPONENT_LIB} ${js_gz} BINARY)
    endif()
else()
    cmake_minimum_required(VERSION 3.16)
    project(wifilist C)
    add_library(wifilist_portable STATIC ${WIFILIST_PORTABLE_SRCS})
    target_include_directories(wifilist_portable PUBLIC include)
    set_target_properties(wifilist_portable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(wifilist_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # Standalone: cmake -S <this dir> -B build && cmake --build build && ctest --test-dir build
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
        add_executable(test_wifilist test/test_wifilist.c)
        target_link_libraries(test_wifilist PRIVATE wifilist_portable)
        add_test(NAME wifilist COMMAND test_wifilist)
    endif()
endif()
