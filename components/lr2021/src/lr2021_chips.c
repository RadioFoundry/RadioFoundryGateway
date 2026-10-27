// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The ROMs the raw paths are known on (lr2021_chips.h). */
#include "lr2021_chips.h"

#include <stddef.h>

static const lr2021_rom_t roms[] = {
    /* docs/spec/lr2021-iq.md: R1.1, section 2 (receive), section 3 (transmit) */
    {0x01, 0x18, "LR2021", true, true, "ROM 1.24: a LilyGO T-Display-P4 (2026-10-18) and a Meshnology W12 (2026-10-24)"},
};

const lr2021_rom_t *lr2021_rom_find(uint8_t major, uint8_t minor)
{
    for (size_t i = 0; i < sizeof roms / sizeof roms[0]; i++)
        if (roms[i].major == major && roms[i].minor == minor)
            return &roms[i];
    return NULL;
}

const lr2021_rom_t *lr2021_roms(int *n)
{
    *n = (int)(sizeof roms / sizeof roms[0]);
    return roms;
}
