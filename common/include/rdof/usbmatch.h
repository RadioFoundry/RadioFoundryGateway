// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Which of a board's radios a USB device belongs to, when several want the
 * same kind (two OpenGD77s on a hub, all 1FC9:0094): by serial number where
 * a radio's settings give one, by where it's plugged in (the hub ports from
 * the board down) where they give that, else the first radio of that kind
 * that has nothing yet. Portable C; usb_host.c asks it for each device.
 */
#ifndef RDOF_USBMATCH_H
#define RDOF_USBMATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t vid, pid;
    char serial[32];   /* its serial number string ("" if it has none) */
    char port[16];     /* hub ports from the board down, "1.3"; "" on the board's own port */
} rdof_usb_dev_t;

typedef struct {
    uint16_t vid, pid;
    const char *serial; /* "" or NULL: any */
    const char *port;   /* "" or NULL: any */
} rdof_usb_want_t;

/* The want that takes dev: of those that match it (vid, pid, and serial and port where they're set) and aren't
   taken, the one that named the most (serial and port, then either, then neither), the first among equals. -1 for
   none. */
int rdof_usb_match(const rdof_usb_dev_t *dev, const rdof_usb_want_t *wants, int n, const bool *taken);
/* The port string for a device n hubs down: ports[0] is the port on the hub nearest the board. */
void rdof_usb_port_path(const uint8_t *ports, int n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
