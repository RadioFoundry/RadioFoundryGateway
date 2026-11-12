// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Which hardware a box runs (docs/spec/fw/rxtx-combinations.md section 1.1): each file in fixtures/config/combos is
   one mix of modems, radios and SDRs with a network, and expect.txt says what the open makes of it and what each
   platform answers:

     file|units|board|rdofradio

   units: "name:kind,..." as the open makes them, or "refused: <the open's reason>". board and rdofradio: "ok", or
   the reason that platform refuses the file (rdof_plan_check and rdof_plan_roles; rdofradio_cfg_parse). A limit that
   is lifted changes its line here. A file named examples/<name> is config/examples/<name> (the second argument). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/siteplan.h"
#include "rdofradio_cfg.h"

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

static const char *const KIND[] = {"?", "repeater", "sfr", "dcdm", "transceiver", "receiver"};
static rdofradio_cfg_t RC;
static const char *examples_dir = ".";

static void one(const char *dir, char *line)
{
    char *file = line, *units = strchr(line, '|');
    char *board = units ? strchr(units + 1, '|') : NULL, *host = board ? strchr(board + 1, '|') : NULL;
    CHECK(host, "%s: file|units|board|rdofradio", line);
    if (!host)
        return;
    *units++ = *board++ = *host++ = 0;
    char path[512];
    if (!strncmp(file, "examples/", 9)) /* config/examples, beside the fixtures */
        snprintf(path, sizeof path, "%s/%s", examples_dir, file + 9);
    else
        snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    CHECK(f, "%s: can't read", path);
    if (!f)
        return;
    static char t[65536];
    const size_t n = fread(t, 1, sizeof t - 1, f);
    fclose(f);
    t[n] = 0;
    char err[400] = "", got[1024] = "";
    rdof_plan_t *p = rdof_plan_open(t, n, err, sizeof err);
    if (!p) {
        snprintf(got, sizeof got, "refused: %s", err);
        CHECK(!strcmp(got, units), "%s: the open: %s (expected %s)", file, got, units);
        return;
    }
    size_t o = 0;
    for (int k = 0; k < rdof_plan_nunits(p) && o < sizeof got; k++) {
        const rdof_unit_t *u = rdof_plan_unit(p, k);
        o += (size_t)snprintf(got + o, sizeof got - o, "%s%s:%s", k ? "," : "", u->name, KIND[u->kind]);
    }
    CHECK(!strcmp(got, units), "%s: units %s (expected %s)", file, got, units);
    static rdof_roles_t roles;
    char b[400] = "ok", h[400] = "ok";
    if (rdof_plan_check(p, &rdof_plan_board, b, sizeof b))
        rdof_plan_roles(p, &roles, b, sizeof b);
    CHECK(!strcmp(b, board), "%s: the board: %s (expected %s)", file, b, board);
    memset(&RC, 0, sizeof RC);
    if (rdofradio_cfg_parse(t, n, &RC, h, sizeof h))
        strcpy(h, "ok");
    CHECK(!strcmp(h, host), "%s: rdofradio: %s (expected %s)", file, h, host);
    if (!strncmp(file, "examples/", 9) && !strcmp(h, "ok")) {
        /* every example: the public NTP servers (none local, no pool), and a bench that may transmit in each US
           amateur band from 6 m to 13 cm (8, bench.tx's most), for 10 s at most, at the least power */
        static const uint32_t BANDS[8][2] = {{50000000, 54000000},     {144000000, 148000000},
                                             {222000000, 225000000},   {420000000, 450000000},
                                             {902000000, 928000000},   {1240000000, 1300000000},
                                             {2300000000, 2310000000}, {2390000000, 2450000000}};
        int nsrv = 0;
        for (const char *q = RC.ntp_servers; *q;) {
            while (*q == ' ')
                q++;
            if (*q)
                nsrv++;
            while (*q && *q != ' ')
                q++;
        }
        CHECK(RC.own_clock && nsrv == 6 && strlen(RC.ntp_servers) < 128 && strstr(RC.ntp_servers, "nist.gov") &&
                  !strstr(RC.ntp_servers, "192.168.") && !strstr(RC.ntp_servers, "pool"),
              "%s: ntp: %s (%d servers)", file, RC.ntp_servers, nsrv);
        const rdof_benchtx_policy_t *b = &RC.bench_tx;
        bool bands = RC.has_bench_tx && b->nbands == 8;
        for (int i = 0; bands && i < 8; i++)
            bands = b->bands_hz[i][0] == BANDS[i][0] && b->bands_hz[i][1] == BANDS[i][1];
        CHECK(bands && b->nfreqs == 0 && b->max_key_s == 10 && !b->has_max_dbm && b->rf_level == 0 &&
                  b->arm_gpio == -1,
              "%s: bench.tx: %d bands, %d freqs, key %.1f s", file, b->nbands, b->nfreqs, b->max_key_s);
    }
    rdof_plan_close(p);
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s fixtures/config/combos [config/examples]\n", argv[0]);
        return 2;
    }
    if (argc == 3)
        examples_dir = argv[2];
    char path[512];
    snprintf(path, sizeof path, "%s/expect.txt", argv[1]);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: can't read\n", path);
        return 2;
    }
    static char line[2048], listed[8192] = "|";
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "examples/", 9)) { /* "|examples/a.json|examples/b.json|" */
            const size_t k = strlen(listed);
            snprintf(listed + k, sizeof listed - k, "%.*s|", (int)strcspn(line, "|"), line);
        }
        if (line[0] && line[0] != '#')
            one(argv[1], line), n++;
    }
    fclose(f);
    if (argc == 3) { /* every example has its line */
        DIR *dir = opendir(examples_dir);
        CHECK(dir, "%s: can't list", examples_dir);
        for (struct dirent *e; dir && (e = readdir(dir));) {
            const size_t len = strlen(e->d_name);
            if (len < 5 || strcmp(e->d_name + len - 5, ".json"))
                continue;
            char want[300];
            snprintf(want, sizeof want, "|examples/%s|", e->d_name);
            CHECK(strstr(listed, want), "config/examples/%s has no line in expect.txt", e->d_name);
        }
        if (dir)
            closedir(dir);
    }
    CHECK(n >= 60, "%d combinations: some are missing", n);
    printf("%d combinations, %d checks, %d failed\n", n, checks, fails);
    return fails != 0;
}
