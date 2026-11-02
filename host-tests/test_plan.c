// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The plan (rdof/siteplan.h) and the parsers on it. Each argument a version 3 file: good-* opens, makes its units
   and gives each part its settings, the text forms the same structs as the plan's; bad-* is refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/config.h"
#include "rdof/ipgw.h"
#include "rdof/sdr/config.h"
#include "rdof/siteplan.h"

static int checks, fails;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(c)) {                                                                                                \
            fails++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    *len = fread(t, 1, (size_t)n, f);
    t[*len] = 0;
    fclose(f);
    return t;
}

static rdof_config_t A, B;
static rdof_sdr_config_t SA, SB;
static char da[8192], db[8192];

static void radio(const char *f, const char *t, size_t len, const rdof_plan_t *p, const char *name)
{
    char ea[300] = "", eb[300] = "";
    bool oa = rdof_config_parse_radio(t, len, name, &A, ea, sizeof ea);
    bool ob = rdof_plan_radio(p, name, &B, eb, sizeof eb);
    CHECK(oa == ob, "%s: radio %s: text %s, plan %s", f, name ? name : "(first)", oa ? "ok" : ea, ob ? "ok" : eb);
    if (!oa || !ob)
        return;
    rdof_config_describe(&A, da, sizeof da);
    rdof_config_describe(&B, db, sizeof db);
    CHECK(!strcmp(da, db), "%s: radio %s:\n  text %s\n  plan %s", f, name ? name : "(first)", da, db);
    if (memcmp(&A, &B, sizeof A)) {
        const unsigned char *a = (const void *)&A, *b = (const void *)&B;
        size_t k = 0;
        while (a[k] == b[k])
            k++;
        CHECK(0, "%s: radio %s: structs differ at byte %zu (of %zu)", f, name ? name : "(first)", k, sizeof A);
    }
}

static void sdr(const char *f, const char *t, size_t len, const rdof_plan_t *p, const char *name)
{
    char ea[300] = "", eb[300] = "";
    bool oa = rdof_sdr_config_parse_device(t, len, name, &SA, ea, sizeof ea);
    bool ob = rdof_plan_sdr(p, name, &SB, eb, sizeof eb);
    CHECK(oa == ob, "%s: sdr %s: text %s, plan %s", f, name ? name : "(first)", oa ? "ok" : ea, ob ? "ok" : eb);
    if (!oa || !ob)
        return;
    rdof_sdr_config_describe(&SA, da, sizeof da);
    rdof_sdr_config_describe(&SB, db, sizeof db);
    CHECK(!strcmp(da, db), "%s: sdr %s:\n  text %s\n  plan %s", f, name ? name : "(first)", da, db);
    if (memcmp(&SA, &SB, sizeof SA)) {
        const unsigned char *a = (const void *)&SA, *b = (const void *)&SB;
        size_t k = 0;
        while (a[k] == b[k])
            k++;
        CHECK(0, "%s: sdr %s: structs differ at byte %zu (of %zu)", f, name ? name : "(first)", k, sizeof SA);
    }
}

static void good(const char *f, const char *t, size_t len)
{
    char err[300] = "", e2[300] = "";
    rdof_plan_t *p = rdof_plan_open(t, len, err, sizeof err);
    CHECK(p, "%s: plan: %s", f, err);
    if (!p)
        return;
    static rdof_roles_t ra, rb;
    bool oa = rdof_box_roles(t, len, &ra, err, sizeof err);
    bool ob = rdof_plan_roles(p, &rb, e2, sizeof e2);
    CHECK(oa == ob, "%s: roles: text %s, plan %s", f, oa ? "ok" : err, ob ? "ok" : e2);
    if (oa && ob)
        CHECK(!memcmp(&ra, &rb, sizeof ra), "%s: roles differ: radios %d/%d (%s/%s) sdr %d/%d (%s/%s) box %s/%s", f,
              ra.nradios, rb.nradios, ra.radio, rb.radio, ra.has_sdr, rb.has_sdr, ra.sdr, rb.sdr, ra.box, rb.box);
    radio(f, t, len, p, NULL);
    for (int k = 0; k < rdof_plan_nunits(p); k++)
        radio(f, t, len, p, rdof_plan_unit(p, k)->name);
    sdr(f, t, len, p, NULL);
    for (int k = 0; k < rdof_plan_nunits(p); k++)
        sdr(f, t, len, p, rdof_plan_unit(p, k)->name);
    const rdof_site_t *s = rdof_plan_site(p);
    for (int d = 0; d < s->ndev; d++)
        if (s->dev[d].kind == RDOF_DEV_SDR)
            sdr(f, t, len, p, s->dev[d].name);
    static rdof_ipgw_config_t ia, ib;
    oa = rdof_ipgw_settings_parse(t, len, &ia, err, sizeof err);
    ob = rdof_plan_ipgw(p, &ib, e2, sizeof e2);
    CHECK(oa == ob, "%s: ipgw: text %s, plan %s", f, oa ? "ok" : err, ob ? "ok" : e2);
    if (oa && ob)
        CHECK(!memcmp(&ia, &ib, sizeof ia), "%s: ipgw differs", f);
    static rdof_bench_radio_t ba[RDOF_BENCH_MAX_RADIOS], bb[RDOF_BENCH_MAX_RADIOS];
    memset(ba, 0, sizeof ba);
    memset(bb, 0, sizeof bb);
    int na = rdof_bench_radios_parse(t, len, ba, RDOF_BENCH_MAX_RADIOS, err, sizeof err);
    int nb = rdof_plan_bench_radios(p, bb, RDOF_BENCH_MAX_RADIOS, e2, sizeof e2);
    CHECK(na == nb, "%s: bench radios: text %d %s, plan %d %s", f, na, err, nb, e2);
    if (na == nb && na > 0)
        CHECK(!memcmp(ba, bb, sizeof ba), "%s: bench radios differ", f);
    static rdof_benchtx_policy_t ta, tb;
    na = rdof_bench_tx_parse(t, len, &ta, err, sizeof err);
    nb = rdof_plan_bench_tx(p, &tb, e2, sizeof e2);
    CHECK(na == nb, "%s: bench tx: text %d %s, plan %d %s", f, na, err, nb, e2);
    if (na == nb && !na)
        CHECK(!memcmp(&ta, &tb, sizeof ta), "%s: bench tx differs", f);
    printf("  %s: %d units, %d extra\n", f, rdof_plan_nunits(p), rdof_plan_nextra(p));
    rdof_plan_close(p);
}

static void bad(const char *f, const char *t, size_t len)
{
    char err[300] = "";
    rdof_plan_t *p = rdof_plan_open(t, len, err, sizeof err);
    bool refused = !p;
    if (p) {
        static rdof_roles_t r;
        refused = !rdof_plan_roles(p, &r, err, sizeof err);
        for (int k = 0; !refused && k < r.nradios; k++)
            refused = !rdof_plan_radio(p, r.radios[k], &B, err, sizeof err);
        if (!refused && r.has_sdr)
            refused = !rdof_plan_sdr(p, NULL, &SB, err, sizeof err);
        rdof_plan_close(p);
    }
    CHECK(refused, "%s: taken", f);
    printf("  %s: %s\n", f, err);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        size_t len;
        char *t = slurp(argv[i], &len);
        CHECK(t, "%s: can't read", argv[i]);
        if (!t)
            continue;
        const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!strncmp(base, "bad-", 4))
            bad(base, t, len);
        else
            good(base, t, len);
        free(t);
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
