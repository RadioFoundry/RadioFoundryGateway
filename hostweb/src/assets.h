// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#ifndef RDOF_HOSTWEB_ASSETS_H
#define RDOF_HOSTWEB_ASSETS_H
#include <stddef.h>
typedef struct {
    const char *path; /* "/", "/netclock.js" */
    const unsigned char *data;
    size_t len;
} rdof_hostweb_asset_t;
extern const rdof_hostweb_asset_t rdof_hostweb_assets[]; /* ends with a NULL path */
#endif
