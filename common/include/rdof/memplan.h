// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The memory plan: what each role needs of the board's RAM, declared at boot
 * before anything starts, laid out in a few regions and checked against what
 * is free, with a margin kept for Wi-Fi, lwIP and what isn't planned.
 *
 * Portable: the platform measures the heap (rdof_mem_avail_t), allocates the
 * regions this lays out and hands each role its pointers (esp_idf/main/mem.c on
 * the S3). A role stopped later can give its memory back (rdof_memplan_release)
 * and another can claim some at run time (rdof_memplan_add_now). See
 * docs/spec/fw/memory.md.
 */
#ifndef RDOF_MEMPLAN_H
#define RDOF_MEMPLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_MEM_MAX_CLAIMS 72
#define RDOF_MEM_MAX_ROLES 12
#define RDOF_MEM_MAX_REGIONS 40
#define RDOF_MEM_MAX_COUNT 16 /* blocks a claim at most */

/* Where a claim may go. */
#define RDOF_MEM_INTERNAL 0x1u /* internal RAM */
#define RDOF_MEM_DMA 0x2u      /* DMA capable: internal, aligned for the DMA (on the S3, 4 bytes) */
#define RDOF_MEM_PSRAM 0x4u    /* external RAM; internal on a board without it */
#define RDOF_MEM_FALLBACK 0x8u /* with INTERNAL: internal if the spare above the margin holds it, else PSRAM */

typedef enum {
    RDOF_MEM_ALLOC,  /* allocated in its role's region; the role takes the pointer */
    RDOF_MEM_HOLD,   /* allocated on its own, a block at a time, and freed just before a driver allocates the same
                      * (rdof_memplan_hand_over): the room is kept until then */
    RDOF_MEM_BUDGET, /* counted against the free memory, not allocated: something allocates it itself, soon */
} rdof_mem_how_t;

typedef enum { RDOF_MEM_NOWHERE, RDOF_MEM_IN_INTERNAL, RDOF_MEM_IN_DMA, RDOF_MEM_IN_PSRAM } rdof_mem_where_t;

typedef enum { RDOF_ROLE_PLANNED, RDOF_ROLE_REFUSED, RDOF_ROLE_RELEASED } rdof_role_state_t;

/* The heap as the platform sees it. */
typedef struct {
    uint32_t internal_free, internal_largest;
    uint32_t dma_free, dma_largest;
    uint32_t psram_free, psram_largest;
} rdof_mem_avail_t;

typedef struct {
    char name[16];
    int role;
    uint32_t size;  /* bytes a block; an elastic claim's least */
    uint32_t max;   /* elastic: grown from the spare up to this a block (0: fixed) */
    uint32_t unit;  /* elastic: in steps of this */
    uint16_t align; /* 4 at least */
    uint16_t count; /* blocks: 1, or a HOLD's several (handed over one at a time) */
    uint8_t caps;   /* RDOF_MEM_* */
    uint8_t how;    /* rdof_mem_how_t */
    /* the plan's */
    uint8_t where;   /* rdof_mem_where_t */
    bool taken;      /* ALLOC: the role has its pointer */
    uint16_t handed; /* HOLD: blocks handed over so far */
    uint32_t got;    /* bytes a block, size up to max */
    int region;      /* ALLOC: its region, -1 for none */
    uint32_t offset; /* ALLOC: in its region */
} rdof_mem_claim_t;

typedef struct {
    int role;
    uint8_t where;
    uint16_t align;
    uint32_t bytes;
    bool released;
    void *base; /* the platform's allocation; NULL until then */
} rdof_mem_region_t;

typedef struct {
    char name[12];
    uint8_t state; /* rdof_role_state_t */
    char why[112];
} rdof_mem_role_t;

typedef struct {
    rdof_mem_claim_t c[RDOF_MEM_MAX_CLAIMS];
    int n;
    rdof_mem_role_t role[RDOF_MEM_MAX_ROLES];
    int nroles;
    rdof_mem_region_t r[RDOF_MEM_MAX_REGIONS];
    int nr;
    uint32_t margin; /* internal RAM kept free for Wi-Fi, lwIP and what isn't planned */
    bool strict;     /* a role that would cut into the margin is refused (else it runs, and ok is false) */
    bool solved;
    rdof_mem_avail_t at_plan; /* the heap when it was solved */
    int64_t spare;            /* internal above the margin after the plan (negative: short by that much) */
    bool ok;
    char error[160];
} rdof_memplan_t;

void rdof_memplan_init(rdof_memplan_t *p, uint32_t margin, bool strict);

/* A claim for role's name: size bytes aligned to align (0: 4), caps RDOF_MEM_*. Roles are planned in the order
 * they first appear. Returns its index, or -1 (the table is full, or bad arguments: error says which). */
int rdof_memplan_add(rdof_memplan_t *p, const char *role, const char *name, uint32_t size, uint16_t align,
                     uint8_t caps, rdof_mem_how_t how);
/* A claim of count blocks of its size (a HOLD's are handed over one at a time). */
void rdof_memplan_count(rdof_memplan_t *p, int i, int count);
/* An elastic claim: from its size up to max a block, in steps of unit, from what's spare above the margin. A
 * FALLBACK one grows in its turn among the FALLBACK claims, ahead of those after it; the others, from what's left
 * after all of them. */
void rdof_memplan_elastic(rdof_memplan_t *p, int i, uint32_t max, uint32_t unit);

/* Place every claim against a, the heap before anything is allocated, and lay out the regions. A role whose needs
 * don't fit above the margin: with strict, refused; else placed, and ok false. A role whose PSRAM claims don't fit
 * is refused. Returns ok. */
bool rdof_memplan_solve(rdof_memplan_t *p, const rdof_mem_avail_t *a);

int rdof_memplan_find(const rdof_memplan_t *p, const char *role, const char *name);
int rdof_memplan_role(const rdof_memplan_t *p, const char *role);
bool rdof_memplan_role_ok(const rdof_memplan_t *p, const char *role); /* planned (not refused or released) */
/* The platform couldn't allocate a role's region: refused, with why (ok false). */
void rdof_memplan_refuse(rdof_memplan_t *p, int role, const char *why);
/* A HOLD's next block handed over (the platform frees it just before the driver allocates): false if none left. */
bool rdof_memplan_hand_over(rdof_memplan_t *p, int claim);

/* A stopped role gives its memory back: its claims go nowhere and its regions are released (the platform frees
 * them first, and the HOLD blocks not handed over). Returns the internal bytes it held. */
uint32_t rdof_memplan_release(rdof_memplan_t *p, const char *role);
/* A claim at run time, placed against now (the margin always kept, strict or not), in a region of its own. Returns
 * its index, or -1 with why (it doesn't fit, or the tables are full); the plan's ok and error stay as they were. */
int rdof_memplan_add_now(rdof_memplan_t *p, const char *role, const char *name, uint32_t size, uint16_t align,
                         uint8_t caps, const rdof_mem_avail_t *now, char *why, size_t why_cap);

/* Bytes a role holds by where (RDOF_MEM_IN_*), ALLOC and HOLD (not handed over), and its BUDGET claims. */
uint32_t rdof_memplan_role_bytes(const rdof_memplan_t *p, int role, int where);
uint32_t rdof_memplan_role_budget(const rdof_memplan_t *p, int role);
/* Internal (with DMA) bytes the plan has allocated now: regions not released, holds not handed over. */
uint32_t rdof_memplan_internal_held(const rdof_memplan_t *p);

const char *rdof_mem_where_name(int where);
const char *rdof_mem_how_name(int how);

/* The "memory" object of the status (docs/spec/fw/interfaces-http.md): the plan, and now, the heap at the time. */
void rdof_memplan_json(const rdof_memplan_t *p, const rdof_mem_avail_t *now, rdof_json_t *j);
/* One line a role for the log: name, state, internal / DMA / PSRAM / budget KB. Returns the length. */
size_t rdof_memplan_describe(const rdof_memplan_t *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
