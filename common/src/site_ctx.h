// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The version 3 parse's document and where each entry is in it, shared by site.c and siteplan.c (not a public
   header). */
#ifndef RDOF_SITE_CTX_H
#define RDOF_SITE_CTX_H

#include <stdbool.h>
#include <stddef.h>

#include "rdof/jsonp.h"
#include "rdof/site.h"

#define RDOF_SITE_MAX_NODES 2000
#define RDOF_SITE_STR_BUF 16384

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[RDOF_SITE_MAX_NODES];
    char strs[RDOF_SITE_STR_BUF];
    char *clean;
    int root;
    char *error;
    size_t cap;
    /* where each entry is in the document */
    int fnode[RDOF_SITE_MAX_FREQS], dnode[RDOF_SITE_MAX_DEVICES], anode[RDOF_SITE_MAX_APPS],
        nnode[RDOF_SITE_MAX_NETS], pnode[RDOF_SITE_MAX_POLICIES];
    int defaults;
} rdof_site_ctx_t;

/* The document, parsed as JSON (comments stripped): NULL with error set when there's no memory. A parse error is
   in doc.error (root < 0). Errors after go to error[cap]. */
rdof_site_ctx_t *rdof_site_ctx_open(const char *text, size_t len, char *error, size_t cap);
void rdof_site_ctx_close(rdof_site_ctx_t *c);
/* rdof_site_parse on an open document. */
bool rdof_site_ctx_parse(rdof_site_ctx_t *c, rdof_site_t *s);

/* Device d hears (receives) / transmits on frequency f: its hears and transmits, and the frequency's devices. */
bool rdof_site_dev_hears(const rdof_site_t *s, int d, int f);
bool rdof_site_dev_sends(const rdof_site_t *s, int d, int f);
/* The first ID of a set, 0 if it's empty. */
uint32_t rdof_site_first_id(const rdof_idset_t *set);
/* The hblink network, -1 none. */
int rdof_site_hb_net(const rdof_site_t *s);

#endif
