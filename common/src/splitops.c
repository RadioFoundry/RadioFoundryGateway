// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The 0xD6 additions' encodings (rdof/splitops.h). */
#include "rdof/splitops.h"

#include <string.h>

int64_t rdof_n_of_counter(uint32_t c, int64_t near_n)
{
    return near_n + (int32_t)(c - rdof_slot_counter(near_n));
}

uint16_t rdof_ops_caps(const uint8_t *info, size_t len)
{
    if (len < RDOF_OPS_CAPS_AT + 2)
        return 0;
    return (uint16_t)((info[RDOF_OPS_CAPS_AT] | info[RDOF_OPS_CAPS_AT + 1] << 8) & RDOF_OPS_BUILT);
}

int rdof_ops_guard_ms(const uint8_t *info, size_t len)
{
    return len > RDOF_OPS_GUARD_AT && info[RDOF_OPS_GUARD_AT] ? info[RDOF_OPS_GUARD_AT] : RDOF_OPS_GUARD_MS;
}

int rdof_ops_horizon(const uint8_t *info, size_t len)
{
    return len > RDOF_OPS_HORIZON_AT && info[RDOF_OPS_HORIZON_AT] ? info[RDOF_OPS_HORIZON_AT] : RDOF_OPS_HORIZON_SLOTS;
}

int rdof_ops_air_delay_us(const uint8_t *info, size_t len)
{
    if (len < RDOF_OPS_AIR_DELAY_AT + 2)
        return 0;
    return (int16_t)(info[RDOF_OPS_AIR_DELAY_AT] | info[RDOF_OPS_AIR_DELAY_AT + 1] << 8);
}

bool rdof_ops_can_align(uint16_t caps, int stream_mode)
{
    (void)stream_mode; /* either mode */
    return (caps & RDOF_OPS_ALIGN) != 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *d)
{
    return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}

size_t rdof_ops_align(uint8_t *out, uint32_t t_us, uint32_t period_ns, int64_t n, int max_slew_us)
{
    put32(out, t_us);
    put32(out + 4, period_ns);
    put32(out + 8, rdof_slot_counter(n));
    if (max_slew_us < 0)
        return 12;
    int v = max_slew_us > 0xFFFF ? 0xFFFF : max_slew_us;
    out[12] = (uint8_t)v;
    out[13] = (uint8_t)(v >> 8);
    return 14;
}

bool rdof_ops_aligned(bool ok, const uint8_t *d, size_t len, int32_t *phase_us, int *flags)
{
    if (!ok || !d || len < 5)
        return false;
    *phase_us = (int32_t)get32(d);
    *flags = d[4];
    return true;
}

size_t rdof_ops_schedule(uint8_t *out, int64_t n, int slot, uint8_t control, const uint8_t burst[33])
{
    if (((n & 1) == 0) != (slot == 1))
        return 0;
    put32(out, rdof_slot_counter(n));
    out[4] = (uint8_t)slot;
    out[5] = control;
    memcpy(out + 6, burst, 33);
    return 39;
}

bool rdof_ops_scheduled(const uint8_t *d, size_t len, uint32_t *counter, int *outcome)
{
    if (len < 5)
        return false;
    *counter = get32(d);
    *outcome = d[4];
    return true;
}


static uint16_t get16(const uint8_t *d) { return (uint16_t)(d[0] | d[1] << 8); }

bool rdof_ops_status_ext(const uint8_t *status, size_t len, rdof_ops_status_t *st)
{
    if (len < RDOF_OPS_STATUS_EXT_AT + 15)
        return false;
    const uint8_t *d = status + RDOF_OPS_STATUS_EXT_AT;
    st->sent = get16(d), st->late = get16(d + 2), st->early_full = get16(d + 4);
    st->replaced = get16(d + 6), st->cooling = get16(d + 8);
    st->phase_us = (int32_t)get32(d + 10);
    st->flags = d[14];
    return true;
}

void rdof_ops_put_status_ext(uint8_t *status, const rdof_ops_status_t *st)
{
    uint8_t *d = status + RDOF_OPS_STATUS_EXT_AT;
    const uint16_t v[5] = {st->sent, st->late, st->early_full, st->replaced, st->cooling};
    for (int i = 0; i < 5; i++)
        d[2 * i] = (uint8_t)v[i], d[2 * i + 1] = (uint8_t)(v[i] >> 8);
    put32(d + 10, (uint32_t)st->phase_us);
    d[14] = (uint8_t)st->flags;
    d[15] = 0;
}
