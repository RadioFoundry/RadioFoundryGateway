// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * ID sets: the talkgroups and private-call IDs a talk path carries (docs/spec/fw/config-v3.md section 3).
 *
 *   202  "3100-3199"  "*"  "!350"  "202,300-399,!350"  [202, "300-399", "!350"]  false
 *
 * A set is the union of its plain items less its "!" items; a set of only "!" items starts from "*". An ID is
 * "listed" when a plain item other than "*" holds it (a listed talkgroup beats one matched only by "*").
 * Portable C, no allocation.
 */
#ifndef RDOF_IDSET_H
#define RDOF_IDSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_ID_MAX 16777215u /* 24 bits */
#define RDOF_IDSET_MAX 12     /* plain and "!" ranges together, after merging */

typedef struct {
    uint32_t lo, hi;
} rdof_idrange_t;

typedef struct {
    bool star;          /* "*", or only "!" items */
    uint8_t nin, nout;  /* r[0, nin): the plain ranges; r[nin, nin + nout): the "!" ranges */
    rdof_idrange_t r[RDOF_IDSET_MAX];
} rdof_idset_t;

/* The empty set (false). */
void rdof_idset_clear(rdof_idset_t *s);
/* One item: "202", "3100-3199", "*", "!350", "!4000-4099"; or several, comma separated. false with err set. Call
   rdof_idset_finish after the last. */
bool rdof_idset_add(rdof_idset_t *s, const char *items, char *err, size_t cap);
/* One ID as a plain item. */
bool rdof_idset_add_id(rdof_idset_t *s, uint32_t id, char *err, size_t cap);
/* Sorts and merges; a set of only "!" items becomes "*" less them. */
void rdof_idset_finish(rdof_idset_t *s);

bool rdof_idset_has(const rdof_idset_t *s, uint32_t id);
/* In a plain item other than "*" (and not excluded). */
bool rdof_idset_listed(const rdof_idset_t *s, uint32_t id);
bool rdof_idset_empty(const rdof_idset_t *s);
/* The one ID the set holds, or 0 when it holds none or more than one. */
uint32_t rdof_idset_single(const rdof_idset_t *s);
/* The IDs held, as sorted ranges (at most max; returns how many there are). */
int rdof_idset_ranges(const rdof_idset_t *s, rdof_idrange_t *out, int max);
/* The set in its own syntax, canonical: "*,!9,!4000-4099", "202,300-349,351-399", "" for none. Returns the length. */
size_t rdof_idset_format(const rdof_idset_t *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
