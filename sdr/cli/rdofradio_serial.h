// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A serial port, raw, for rdofradio's split TX half: a RadioFoundry radio's hotspot on its USB (CDC-ACM, where the
 * speed doesn't matter) or on a programming cable's UART. POSIX termios: Linux, macOS, the PlutoSDR.
 */
#ifndef RDOFRADIO_SERIAL_H
#define RDOFRADIO_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The port open, raw, 8N1, no flow control, non-blocking: its fd, or -1 with the reason in err. */
int rdofradio_serial_open(const char *path, int baud, char *err, size_t cap);
/* What's there, without waiting: the bytes read, 0 none, -1 the port is gone. */
int rdofradio_serial_read(int fd, uint8_t *buf, size_t cap);
/* All of it, waiting up to 200 ms for room: false if it couldn't. */
bool rdofradio_serial_write(int fd, const uint8_t *data, size_t len);

#endif
