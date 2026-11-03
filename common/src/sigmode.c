// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The signal modes' registry (rdof/sigmode.h). */
#include "rdof/sigmode.h"

#include <stdio.h>
#include <string.h>

const rdof_sig_mode_t *rdof_sig_find(const rdof_sig_mode_t *const *modes, const char *name)
{
    for (int i = 0; modes && name && modes[i]; i++)
        if (!strcmp(modes[i]->name, name))
            return modes[i];
    return NULL;
}

size_t rdof_sig_names_json(const rdof_sig_mode_t *const *modes, bool tx, char *out, size_t cap)
{
    size_t at = 0;
    int k = 0;
    at += (size_t)snprintf(out, cap, "[");
    for (int i = 0; modes && modes[i] && at < cap; i++)
        if (tx ? modes[i]->tx_new != NULL : modes[i]->rx_new != NULL)
            at += (size_t)snprintf(out + at, cap - at, "%s\"%s\"", k++ ? "," : "", modes[i]->name);
    if (at < cap)
        at += (size_t)snprintf(out + at, cap - at, "]");
    return at < cap ? at : 0;
}

static const rdof_jnode_t *member(const rdof_jdoc_t *d, int obj, const char *key)
{
    return rdof_jnode(d, rdof_jget(d, obj, key));
}

double rdof_sig_num(const rdof_jdoc_t *d, int obj, const char *key, double dflt)
{
    const rdof_jnode_t *n = member(d, obj, key);
    return n && n->type == RDOF_JNUM ? n->num : dflt;
}

bool rdof_sig_flag(const rdof_jdoc_t *d, int obj, const char *key)
{
    const rdof_jnode_t *n = member(d, obj, key);
    return n && ((n->type == RDOF_JBOOL && n->b) || (n->type == RDOF_JNUM && n->num != 0));
}

bool rdof_sig_text(const rdof_jdoc_t *d, int obj, const char *key, char *out, size_t cap)
{
    const rdof_jnode_t *n = member(d, obj, key);
    if (!n || n->type != RDOF_JSTR) {
        if (cap)
            out[0] = 0;
        return false;
    }
    snprintf(out, cap, "%s", n->str);
    return true;
}
