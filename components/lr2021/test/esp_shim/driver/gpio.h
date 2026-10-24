// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
#include <stdint.h>
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE = 1 } gpio_int_type_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_int_type_t intr_type;
} gpio_config_t;
int gpio_config(const gpio_config_t *cfg);
int gpio_get_level(int pin);
int gpio_set_level(int pin, uint32_t level);
typedef void (*gpio_isr_t)(void *arg);
int gpio_install_isr_service(int flags);
int gpio_isr_handler_add(int pin, gpio_isr_t isr, void *arg);
int gpio_isr_handler_remove(int pin);
