// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The HAL on Linux (hal/linux.c): the chip on a spidev device (NSS its chip select), BUSY, NRESET and the IRQ
 * on GPIO lines of a GPIO character device (the kernel's v2 line API, no libgpiod). Built on Linux only
 * (LR2021_HAL_LINUX).
 *
 * The FIFO lasts 2 ms at 125 kS/s int16: run the reader thread with a real-time priority (SCHED_FIFO) and, on a
 * Raspberry Pi, keep the SPI core clock fixed so the bus runs at the speed asked.
 */
#ifndef LR2021_HAL_LINUX_H
#define LR2021_HAL_LINUX_H

#include <stddef.h>

#include "lr2021_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *spidev;   /* "/dev/spidev0.0" */
    int speed_hz;         /* 0: 16 MHz, the chip's most (DS 3.7.2) */
    const char *gpiochip; /* "/dev/gpiochip0" */
    int busy, nreset;     /* line offsets on gpiochip */
    int irq;              /* the line a DIO set to IRQ drives, -1 for none */
    int pa_en;            /* a line driven high while open (a front-end supply), -1 for none */
} lr2021_linux_cfg_t;

/* Open the bus and lines into *hal (lr2021_open takes it; lr2021_close releases it). LR2021_ERR_NOT_FOUND for a
 * missing device, LR2021_FAIL for one that wouldn't take the setup (a line another program holds: EBUSY),
 * LR2021_ERR_INVALID_ARG without busy and nreset; err (errlen bytes) says which. Needs Linux 5.10 or later (the
 * GPIO v2 API). */
lr2021_err_t lr2021_hal_linux_open(const lr2021_linux_cfg_t *cfg, lr2021_hal_t *hal, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
