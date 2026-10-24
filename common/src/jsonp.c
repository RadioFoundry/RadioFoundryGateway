// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/jsonp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    rdof_jdoc_t *doc;
    const char *start, *p, *end;
    int depth;
} parser_t;

static int fail(parser_t *ps, const char *why)
{
    snprintf(ps->doc->error, sizeof ps->doc->error, "%s at offset %d", why, (int)(ps->p - ps->start));
    return -1;
}

static void ws(parser_t *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int new_node(parser_t *ps, rdof_jtype_t type)
{
    rdof_jdoc_t *d = ps->doc;
    if (d->n >= d->max)
        return -1;
    rdof_jnode_t *n = &d->nodes[d->n];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->child = n->next = -1;
    return d->n++;
}

static char *put_char(parser_t *ps, char c)
{
    rdof_jdoc_t *d = ps->doc;
    if (d->used >= d->cap)
        return NULL;
    d->buf[d->used] = c;
    return &d->buf[d->used++];
}

static void put_utf8(parser_t *ps, unsigned cp)
{
    if (cp < 0x80) {
        put_char(ps, (char)cp);
    } else if (cp < 0x800) {
        put_char(ps, (char)(0xC0 | (cp >> 6)));
        put_char(ps, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put_char(ps, (char)(0xE0 | (cp >> 12)));
        put_char(ps, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_char(ps, (char)(0x80 | (cp & 0x3F)));
    } else {
        put_char(ps, (char)(0xF0 | (cp >> 18)));
        put_char(ps, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put_char(ps, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_char(ps, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Four hex digits, all of them (strtoul would take "12G4" as 0x12). */
static bool hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return false;
        v = v << 4 | (unsigned)d;
    }
    *out = v;
    return true;
}

static const char *string(parser_t *ps)
{
    rdof_jdoc_t *d = ps->doc;
    size_t start = d->used;
    if (ps->p >= ps->end || *ps->p != '"')
        return NULL;
    ps->p++;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20)
            return NULL;
        if (c != '\\') {
            if (!put_char(ps, c))
                return NULL;
            continue;
        }
        if (ps->p >= ps->end)
            return NULL;
        c = *ps->p++;
        switch (c) {
        case '"': case '\\': case '/': put_char(ps, c); break;
        case 'b': put_char(ps, '\b'); break;
        case 'f': put_char(ps, '\f'); break;
        case 'n': put_char(ps, '\n'); break;
        case 'r': put_char(ps, '\r'); break;
        case 't': put_char(ps, '\t'); break;
        case 'u': {
            unsigned cp, l;
            if (ps->end - ps->p < 4 || !hex4(ps->p, &cp))
                return NULL;
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u' &&
                hex4(ps->p + 2, &l)) {
                if (l >= 0xDC00 && l < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (l - 0xDC00);
                    ps->p += 6;
                }
            }
            put_utf8(ps, cp);
            break;
        }
        default: return NULL;
        }
    }
    if (ps->p >= ps->end || !put_char(ps, 0))
        return NULL;
    ps->p++;
    return &d->buf[start];
}

static int value(parser_t *ps);

static int container(parser_t *ps, bool object)
{
    int self = new_node(ps, object ? RDOF_JOBJ : RDOF_JARR);
    if (self < 0)
        return fail(ps, "too many values");
    if (++ps->depth > 32)
        return fail(ps, "nested too deep");
    ps->p++;
    ws(ps);
    int last = -1;
    if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return self;
    }
    for (;;) {
        const char *key = NULL;
        ws(ps);
        if (object) {
            key = string(ps);
            if (!key)
                return fail(ps, "bad key");
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':')
                return fail(ps, "expected :");
            ps->p++;
        }
        ws(ps);
        int v = value(ps);
        if (v < 0)
            return -1;
        ps->doc->nodes[v].key = key;
        if (last < 0)
            ps->doc->nodes[self].child = v;
        else
            ps->doc->nodes[last].next = v;
        last = v;
        ps->doc->nodes[self].count++;
        ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return self;
        }
        return fail(ps, object ? "expected , or }" : "expected , or ]");
    }
}

static int value(parser_t *ps)
{
    ws(ps);
    if (ps->p >= ps->end)
        return fail(ps, "unexpected end");
    char c = *ps->p;
    if (c == '{' || c == '[')
        return container(ps, c == '{');
    if (c == '"') {
        int n = new_node(ps, RDOF_JSTR);
        if (n < 0)
            return fail(ps, "too many values");
        const char *s = string(ps);
        if (!s)
            return fail(ps, "bad string");
        ps->doc->nodes[n].str = s;
        return n;
    }
    /* the text needn't be NUL terminated (rdof_jsonc_strip's isn't): never look past end */
    const size_t left = (size_t)(ps->end - ps->p);
    if ((left >= 4 && !memcmp(ps->p, "true", 4)) || (left >= 5 && !memcmp(ps->p, "false", 5))) {
        int n = new_node(ps, RDOF_JBOOL);
        if (n < 0)
            return fail(ps, "too many values");
        ps->doc->nodes[n].b = c == 't';
        ps->p += c == 't' ? 4 : 5;
        return n;
    }
    if (left >= 4 && !memcmp(ps->p, "null", 4)) {
        int n = new_node(ps, RDOF_JNULL);
        if (n < 0)
            return fail(ps, "too many values");
        ps->p += 4;
        return n;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[48];
        size_t k = 0;
        bool integer = true;
        while (ps->p < ps->end && k < sizeof tmp - 1 && *ps->p && strchr("+-0123456789.eE", *ps->p)) {
            if (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E')
                integer = false;
            tmp[k++] = *ps->p++;
        }
        tmp[k] = 0;
        char *endp;
        double v = strtod(tmp, &endp);
        if (*endp)
            return fail(ps, "bad number");
        int n = new_node(ps, RDOF_JNUM);
        if (n < 0)
            return fail(ps, "too many values");
        ps->doc->nodes[n].num = v;
        ps->doc->nodes[n].integer = integer;
        return n;
    }
    return fail(ps, "unexpected character");
}

int rdof_jparse(rdof_jdoc_t *doc, const char *text, size_t len)
{
    parser_t ps = {doc, text, text, text + len, 0};
    doc->n = 0;
    doc->used = 0;
    doc->error[0] = 0;
    int root = value(&ps);
    if (root < 0)
        return -1;
    ws(&ps);
    if (ps.p != ps.end)
        return fail(&ps, "extra text after the value");
    return root;
}

int rdof_jget(const rdof_jdoc_t *doc, int obj, const char *key)
{
    if (obj < 0 || obj >= doc->n || doc->nodes[obj].type != RDOF_JOBJ)
        return -1;
    int found = -1;
    for (int c = doc->nodes[obj].child; c >= 0; c = doc->nodes[c].next)
        if (doc->nodes[c].key && !strcmp(doc->nodes[c].key, key))
            found = c;
    return found;
}

int rdof_jat(const rdof_jdoc_t *doc, int node, int i)
{
    if (node < 0 || node >= doc->n)
        return -1;
    int c = doc->nodes[node].child;
    for (; c >= 0 && i > 0; i--)
        c = doc->nodes[c].next;
    return c;
}

const rdof_jnode_t *rdof_jnode(const rdof_jdoc_t *doc, int node)
{
    return node >= 0 && node < doc->n ? &doc->nodes[node] : NULL;
}

/* ── JSON with comments, as text ─────────────────────────────────────────────── */

/* JSONC: // and block comments and trailing commas out, strings untouched. */
size_t rdof_jsonc_strip(const char *in, size_t n, char *out)
{
    size_t o = 0, i = 0;
    while (i < n) {
        if (in[i] == '"') {
            size_t j = i + 1;
            while (j < n && in[j] != '"')
                j += in[j] == '\\' ? 2 : 1;
            if (j >= n)
                j = n - 1;
            memcpy(out + o, in + i, j - i + 1);
            o += j - i + 1;
            i = j + 1;
        } else if (in[i] == '/' && i + 1 < n && in[i + 1] == '/') {
            while (i < n && in[i] != '\n')
                i++;
        } else if (in[i] == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/'))
                i++;
            i += 2;
            out[o++] = ' ';
        } else if (in[i] == ']' || in[i] == '}') {
            /* drop a trailing comma before it */
            size_t k = o;
            while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\n' || out[k - 1] == '\t' || out[k - 1] == '\r'))
                k--;
            if (k > 0 && out[k - 1] == ',')
                out[k - 1] = ' ';
            out[o++] = in[i++];
        } else {
            out[o++] = in[i++];
        }
    }
    return o;
}

/* Past the whitespace and comments at i. */
static size_t skip_ws(const char *t, size_t n, size_t i)
{
    for (;;) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
            i++;
        if (i + 1 < n && t[i] == '/' && t[i + 1] == '/') {
            while (i < n && t[i] != '\n')
                i++;
        } else if (i + 1 < n && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/'))
                i++;
            i = i + 2 < n ? i + 2 : n;
        } else {
            return i;
        }
    }
}

/* Past the string starting at i (a quote). */
static size_t skip_str(const char *t, size_t n, size_t i)
{
    i++;
    while (i < n && t[i] != '"')
        i += t[i] == '\\' ? 2 : 1;
    return i < n ? i + 1 : n;
}

/* Past the value at i: a string, a container, or a bare word. */
static size_t skip_value(const char *t, size_t n, size_t i)
{
    if (i >= n)
        return n;
    if (t[i] == '"')
        return skip_str(t, n, i);
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        while (i < n) {
            i = skip_ws(t, n, i);
            if (i >= n)
                break;
            if (t[i] == '"') {
                i = skip_str(t, n, i);
                continue;
            }
            if (t[i] == '{' || t[i] == '[')
                depth++;
            else if ((t[i] == '}' || t[i] == ']') && --depth == 0)
                return i + 1;
            i++;
        }
        return n;
    }
    while (i < n && !strchr(",}] \t\r\n/", t[i]))
        i++;
    return i;
}

/* The top-level member `key` from i (inside the object): its key's and value's
   spans, or false. *next is where to keep looking. */
static bool find_member(const char *t, size_t n, size_t *i, const char *key, size_t *ks, size_t *ve)
{
    while (*i < n) {
        size_t p = skip_ws(t, n, *i);
        if (p >= n || t[p] == '}')
            return false;
        if (t[p] == ',') {
            *i = p + 1;
            continue;
        }
        if (t[p] != '"')
            return false;
        size_t ke = skip_str(t, n, p);
        size_t c = skip_ws(t, n, ke);
        if (c >= n || t[c] != ':')
            return false;
        size_t v = skip_ws(t, n, c + 1);
        size_t e = skip_value(t, n, v);
        size_t kl = strlen(key);
        *i = e;
        if (ke - p == kl + 2 && !memcmp(t + p + 1, key, kl)) {
            *ks = p;
            *ve = e;
            return true;
        }
    }
    return false;
}

size_t rdof_jsonc_drop(char *t, size_t n, const char *key)
{
    size_t open = skip_ws(t, n, 0);
    if (open >= n || t[open] != '{')
        return n;
    size_t i = open + 1, ks, ve;
    while (find_member(t, n, &i, key, &ks, &ve)) {
        /* With its comma after it; a comma left before a last member is a trailing comma (fine in JSONC). */
        size_t end = ve, a = skip_ws(t, n, ve);
        if (a < n && t[a] == ',') {
            end = a + 1;
        }
        /* The whole line, when the member had it to itself. */
        size_t s = ks;
        while (s > 0 && (t[s - 1] == ' ' || t[s - 1] == '\t'))
            s--;
        size_t e = end;
        while (e < n && (t[e] == ' ' || t[e] == '\t'))
            e++;
        if ((s == 0 || t[s - 1] == '\n') && e < n && t[e] == '\n') {
            ks = s;
            end = e + 1;
        }
        memmove(t + ks, t + end, n - end);
        n -= end - ks;
        t[n] = 0;
        i = ks;
    }
    return n;
}

size_t rdof_jsonc_add(const char *t, size_t n, const char *key, const char *value, char *out, size_t cap)
{
    size_t open = skip_ws(t, n, 0);
    if (open >= n || t[open] != '{')
        return 0;
    /* After the last member (and its comma, if it has one), or after the brace. */
    size_t i = open + 1, last = open + 1;
    bool any = false, comma = false;
    for (;;) {
        size_t p = skip_ws(t, n, i);
        if (p >= n)
            return 0;
        if (t[p] == '}')
            break;
        if (t[p] == ',') {
            i = last = p + 1;
            comma = true;
            continue;
        }
        if (t[p] != '"')
            return 0;
        size_t c = skip_ws(t, n, skip_str(t, n, p));
        if (c >= n || t[c] != ':')
            return 0;
        i = last = skip_value(t, n, skip_ws(t, n, c + 1));
        any = true;
        comma = false;
    }
    /* Copied as bytes: the text may hold a NUL (in a string), which %.*s would stop at. */
    const char *parts[] = {t, any && !comma ? "," : "", "\n  \"", key, "\": ", value, t + last};
    size_t lens[7] = {last, 0, 0, 0, 0, 0, n - last}, w = 0;
    for (int k = 1; k < 6; k++)
        lens[k] = strlen(parts[k]);
    for (int k = 0; k < 7; k++)
        w += lens[k];
    if (w >= cap) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    char *o = out;
    for (int k = 0; k < 7; k++) {
        memcpy(o, parts[k], lens[k]);
        o += lens[k];
    }
    *o = 0;
    return w;
}
