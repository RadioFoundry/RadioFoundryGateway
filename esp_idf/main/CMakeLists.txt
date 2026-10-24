# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The board (settings, Wi-Fi, web, MQTT, USB host) and each role's files, by CONFIG_RDOF_ROAMER and CONFIG_RDOF_SDR.
# REQUIRES can't depend on Kconfig (it's resolved before the config is): a role that's off leaves its components
# built but not linked.
set(srcs main.c mem.c settings.c logring.c crash.c wifi.c web.c net.c usb_host.c status_led.c oled_task.c heard.c)
if(CONFIG_RDOF_ROAMER)
    list(APPEND srcs roamer.c modem_port.c usb_modem.c uart_modem.c split_port.c radio_task.c roam_hblink.c rx_path.c roam_mqtt.c)
endif()
if(CONFIG_RDOF_ROAMER OR CONFIG_RDOF_SDR)
    list(APPEND srcs modem_sdr.c)
endif()
if(CONFIG_RDOF_IPGW)
    list(APPEND srcs ipgw_task.c)
endif()
set(sdr_dsp sdr_task.c dsp_esp.c kbench.c fir_s3.S)
if(CONFIG_RDOF_SDR)
    list(APPEND srcs sdr.c ${sdr_dsp} slot_timing.c sdr_mqtt.c sdr_hblink.c selftest.c selftest_core.c fe_rtlsdr.c fe_sim.c)
    if(CONFIG_RDOF_BENCH)
        list(APPEND srcs bench_api.c bench_radio.c bench_tx.c)
    endif()
    if(CONFIG_RDOF_SDR_LR2021)
        list(APPEND srcs fe_lr2021.c)
        list(APPEND sdr_dsp fe_lr2021.c) # its read() converts and resamples every sample
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    LDFRAGMENTS linker.lf
    REQUIRES common sdr rtlsdr lr2021 netclock wifilist gnss tinysctp json nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_timer esp_driver_uart esp_driver_i2c esp_driver_gpio esp_driver_rmt usb mqtt app_update esp_app_format mbedtls lwip esp_hw_support
             espcoredump spi_flash)

if(CONFIG_RDOF_SDR AND CONFIG_RDOF_SDR_FAST)
    set_source_files_properties(${sdr_dsp} PROPERTIES COMPILE_OPTIONS -O2)
endif()

# The web page, gzipped at build time and embedded: the full one or the minimal one (CONFIG_RDOF_WEB_FULL).
if(CONFIG_RDOF_WEB_FULL)
    set(page ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
else()
    set(page ${CMAKE_CURRENT_SOURCE_DIR}/web/mini.html)
endif()
idf_build_get_property(python PYTHON)
set(page_gz ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(
    OUTPUT ${page_gz}
    COMMAND ${python} -c "import gzip, sys; open(sys.argv[2], 'wb').write(gzip.compress(open(sys.argv[1], 'rb').read(), 9, mtime=0))" ${page} ${page_gz}
    DEPENDS ${page}
    VERBATIM)
add_custom_target(web_page DEPENDS ${page_gz})
add_dependencies(${COMPONENT_LIB} web_page)
target_add_binary_data(${COMPONENT_TARGET} ${page_gz} BINARY)

# The SDR self-test's IQ clip (selftest/clip.cu8).
if(CONFIG_RDOF_SDR AND CONFIG_RDOF_SDR_SELFTEST)
    target_add_binary_data(${COMPONENT_TARGET} ${CMAKE_CURRENT_SOURCE_DIR}/selftest/clip.cu8 BINARY)
endif()
