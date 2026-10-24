A host stand-in for the few ESP-IDF calls src/lr2021.c makes (SPI master, GPIO, timer, heap caps, FreeRTOS delay),
driven by a scripted fake chip (esp_shim.c). Only for test_lr2021_driver; nothing here is a general ESP-IDF emulation.
