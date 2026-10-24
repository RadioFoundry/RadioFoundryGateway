// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Which radio a USB device belongs to (rdof/usbmatch.h). */
#include "rdof/usbmatch.h"

#include <stdio.h>
#include <string.h>

static bool set(const char *s) { return s && *s; }

int rdof_usb_match(const rdof_usb_dev_t *d, const rdof_usb_want_t *w, int n, const bool *taken)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < n; i++) {
        if ((taken && taken[i]) || w[i].vid != d->vid || w[i].pid != d->pid)
            continue;
        if (set(w[i].serial) && strcmp(w[i].serial, d->serial))
            continue;
        if (set(w[i].port) && strcmp(w[i].port, d->port))
            continue;
        int score = set(w[i].serial) * 2 + set(w[i].port);
        if (score > best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

void rdof_usb_port_path(const uint8_t *ports, int n, char *out, size_t cap)
{
    size_t o = 0;
    if (cap)
        out[0] = 0;
    for (int i = 0; i < n && o < cap; i++) {
        int k = snprintf(out + o, cap - o, "%s%u", i ? "." : "", ports[i]);
        if (k < 0)
            break;
        o += (size_t)k;
    }
    if (cap && o >= cap)
        out[cap - 1] = 0;
}
