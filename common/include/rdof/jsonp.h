// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A small JSON reader into a caller's node array (no malloc): enough for the
 * firmware's settings. Strings are unescaped into a caller's buffer (\uXXXX
 * to UTF-8). Objects keep their keys in order; duplicate keys: the last wins
 * for rdof_jget.
 */
#ifndef RDOF_JSONP_H
#define RDOF_JSONP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RDOF_JNULL = 0, RDOF_JBOOL, RDOF_JNUM, RDOF_JSTR, RDOF_JARR, RDOF_JOBJ } rdof_jtype_t;

typedef struct {
    rdof_jtype_t type;
    const char *key; /* in an object: its key */
    const char *str; /* RDOF_JSTR: the text (NUL terminated) */
    double num;
    bool b;
    bool integer; /* RDOF_JNUM written without a fraction or exponent */
    int child;    /* first child (arrays, objects), -1 none */
    int next;     /* next sibling, -1 none */
    int count;    /* children */
} rdof_jnode_t;

typedef struct {
    rdof_jnode_t *nodes;
    int max, n;
    char *buf;
    size_t cap, used;
    char error[64];
} rdof_jdoc_t;

/* Returns the root node, or -1 (doc->error says why). */
int rdof_jparse(rdof_jdoc_t *doc, const char *text, size_t len);

/* Nodes are the last parse's: one it didn't make (negative, or past doc->n) gives -1 or NULL. */
/* In object `obj`: the node for key (the last one if repeated), or -1. */
int rdof_jget(const rdof_jdoc_t *doc, int obj, const char *key);
/* The i-th child of an array or object, or -1. */
int rdof_jat(const rdof_jdoc_t *doc, int node, int i);
const rdof_jnode_t *rdof_jnode(const rdof_jdoc_t *doc, int node);

/* JSON with comments, as text (the stored settings keep their comments). */

/* Comments and trailing commas out, strings untouched: out
   needs n bytes. Returns the length (not NUL terminated). */
size_t rdof_jsonc_strip(const char *in, size_t n, char *out);
/* Remove the top-level member `key` (every copy) in place; returns the new
   length (t is NUL terminated at it when anything went). */
size_t rdof_jsonc_drop(char *t, size_t n, const char *key);
/* A copy of the object with  "key": value  added after its last member;
   `value` is JSON. Returns the length, or 0 (out empty) if it didn't fit or t isn't an object. */
size_t rdof_jsonc_add(const char *t, size_t n, const char *key, const char *value, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
