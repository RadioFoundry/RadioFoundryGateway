// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the patch RAM loader (src/lr2021_patch_load.c, src/lr2021_patch.c) over the shimmed ESP-IDF calls
// and a chip model with a memory: WriteRegMem32 stores words, ReadRegMem32 reads them, EnablePram starts the image
// (it writes its magic and info words, as Semtech's does). When the image is ours, the model keeps its state in bank 2
// as pram/rdof_pram.S lays it out and answers 0x02F0 as it does.
#include <stdio.h>
#include <string.h>

#include "mock_hal.h"
#include "lr2021.h"
#include "lr2021_patch.h"

static int checks, fails;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static struct {
    uint32_t pram[1024];  /* 0x801000 on */
    bool rdof;            /* our image runs */
    struct {              /* 0x802000 on, word for word */
        uint32_t version, magic, enabled, count, sync_runs, rf_runs, samp_runs, cmds;
        lr2021_rdof_entry_t table[LR2021_RDOF_MAX_ENTRIES];
    } st;
    int rdof_cmds, appends, rdof_fails;
    uint8_t last_status;
    uint8_t prev;         /* the last command's status, which the next frame starts with (6.7.1) */
    uint32_t magic, info; /* 0x800FF8, 0x800FFC */
    int writes, max_words, enables, standbys;
    bool broken;          /* EnablePram doesn't start it */
    uint16_t ops[64];     /* the opcodes written, in order */
    int nops;
    uint8_t pending[64];  /* a read's answer for the next frame */
    size_t pending_n;
} M;

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static uint32_t *word(uint32_t a)
{
    if (a == LR2021_PATCH_MAGIC_ADDR)
        return &M.magic;
    if (a == LR2021_PATCH_INFO_ADDR)
        return &M.info;
    if (a >= LR2021_PATCH_ADDR && a < LR2021_PATCH_ADDR + 4 * 1024)
        return &M.pram[(a - LR2021_PATCH_ADDR) / 4];
    if (a >= LR2021_RDOF_STATE_ADDR && a < LR2021_RDOF_STATE_ADDR + sizeof M.st)
        return (uint32_t *)&M.st + (a - LR2021_RDOF_STATE_ADDR) / 4;
    return NULL;
}

/* The 0x02F0 filter of rdof_pram.S. */
static void rdof_cmd(const uint8_t *p, size_t n)
{
    M.rdof_cmds++;
    M.st.cmds++;
    int st = LR2021_CMD_OK;
    if (n == 0) {
        st = LR2021_CMD_PERR;
    } else if (p[0] == LR2021_RDOF_CLEAR) {
        M.st.count = M.st.enabled = 0; /* the run counts stay */
    } else if (p[0] == LR2021_RDOF_APPEND) {
        size_t k = n - 1;
        if (k == 0 || k % 12 || M.st.count + k / 12 > LR2021_RDOF_MAX_ENTRIES) {
            st = LR2021_CMD_PERR;
        } else {
            for (size_t i = 0; i < k / 12 && st == LR2021_CMD_OK; i++) {
                uint32_t a = be32(p + 1 + 12 * i);
                if (a & 3 || !((a >> 16) == 0x80 || ((a >> 16) >= 0xF0 && (a >> 16) < 0xF5)))
                    st = LR2021_CMD_PERR;
            }
            for (size_t i = 0; i < k / 12 && st == LR2021_CMD_OK; i++)
                M.st.table[M.st.count + i] = (lr2021_rdof_entry_t){be32(p + 1 + 12 * i), be32(p + 5 + 12 * i),
                                                                   be32(p + 9 + 12 * i)};
            if (st == LR2021_CMD_OK)
                M.st.count += (uint32_t)(k / 12), M.appends++;
        }
    } else if (p[0] == LR2021_RDOF_ENABLE) {
        M.st.enabled = 1;
    } else if (p[0] == LR2021_RDOF_DISABLE) {
        M.st.enabled = 0;
    } else if (p[0] == LR2021_RDOF_REPORT) {
        uint8_t *a = M.pending;
        a[0] = (uint8_t)(M.st.version >> 24), a[1] = (uint8_t)(M.st.version >> 16);
        a[2] = (uint8_t)(M.st.version >> 8), a[3] = (uint8_t)M.st.version;
        a[4] = (uint8_t)M.st.count, a[5] = (uint8_t)M.st.enabled;
        for (int i = 0; i < 4; i++) {
            a[6 + i] = (uint8_t)(M.st.sync_runs >> (24 - 8 * i)), a[10 + i] = (uint8_t)(M.st.rf_runs >> (24 - 8 * i));
            a[14 + i] = (uint8_t)(M.st.samp_runs >> (24 - 8 * i));
        }
        M.pending_n = LR2021_RDOF_REPORT_LEN;
        st = LR2021_CMD_DATA;
    } else {
        st = LR2021_CMD_PERR;
    }
    if (st == LR2021_CMD_PERR)
        M.rdof_fails++;
    M.last_status = M.prev = (uint8_t)st;
}

static void respond(int idx, const uint8_t *tx, size_t len, uint8_t *rx)
{
    (void)idx;
    rx[0] = (uint8_t)(M.prev << 1);
    rx[1] = LR2021_MODE_STBY_RC;
    M.prev = LR2021_CMD_OK;
    const uint16_t op = (uint16_t)(tx[0] << 8 | tx[1]);
    if (op && M.nops < 64)
        M.ops[M.nops++] = op;
    if (op == 0 && M.pending_n) {
        memcpy(rx + 2, M.pending, M.pending_n < len - 2 ? M.pending_n : len - 2);
        M.pending_n = 0;
        return;
    }
    if (op == 0x0104 && len >= 9) { /* WriteRegMem32: addr u24, then words */
        uint32_t a = (uint32_t)tx[2] << 16 | (uint32_t)tx[3] << 8 | tx[4];
        int n = (int)(len - 5) / 4;
        M.writes++;
        M.max_words = n > M.max_words ? n : M.max_words;
        for (int i = 0; i < n; i++) {
            uint32_t *w = word(a + 4 * (uint32_t)i);
            if (w)
                *w = be32(tx + 5 + 4 * i);
        }
    } else if (op == 0x0106 && len >= 6) { /* ReadRegMem32: addr u24, count */
        uint32_t a = (uint32_t)tx[2] << 16 | (uint32_t)tx[3] << 8 | tx[4];
        int n = tx[5];
        for (int i = 0; i < n && 4 * i + 4 <= (int)sizeof M.pending; i++) {
            uint32_t *w = word(a + 4 * (uint32_t)i), v = w ? *w : 0;
            M.pending[4 * i] = (uint8_t)(v >> 24), M.pending[4 * i + 1] = (uint8_t)(v >> 16);
            M.pending[4 * i + 2] = (uint8_t)(v >> 8), M.pending[4 * i + 3] = (uint8_t)v;
        }
        M.pending_n = (size_t)(4 * n);
    } else if (op == LR2021_ENABLE_PRAM && len >= 3 && tx[2] != 0) {
        M.prev = LR2021_CMD_FAIL; /* entry n: not modeled */
    } else if (op == LR2021_ENABLE_PRAM) {
        M.enables++;
        if (!M.broken)
            M.magic = M.pram[0], M.info = M.pram[1];
        if (!M.broken && !memcmp(M.pram, lr2021_rdof_pram_image, 4 * lr2021_rdof_pram_words)) {
            M.rdof = true; /* our init: the state, unless bank 2 still holds it */
            if (M.st.magic != LR2021_RDOF_STATE_MAGIC)
                memset(&M.st, 0, sizeof M.st), M.st.magic = LR2021_RDOF_STATE_MAGIC;
            M.st.version = LR2021_RDOF_VERSION;
            M.st.sync_runs = M.st.rf_runs = M.st.samp_runs = 0; /* per EnablePram */
        }
    } else if (op == LR2021_RDOF_CMD && M.rdof) {
        rdof_cmd(tx + 2, len - 2);
    } else if (op == LR2021_SET_STANDBY) {
        M.standbys++;
    }
}

static lr2021_t *open_dev(void)
{
    shim_reset();
    memset(&M, 0, sizeof M);
    M.prev = LR2021_CMD_OK;
    F.respond = respond;
    lr2021_config_t c = mock_config();
    lr2021_t *d = NULL;
    lr2021_open(&c, &d);
    return d;
}

int main(void)
{
    // the image: Semtech's, type 3 version 0x13, 560 words starting with the magic
    lr2021_patch_info_t im = lr2021_patch_image_info();
    CHECK(im.loaded && im.type == 3 && im.version == 0x13, "image info %d %u %u", im.loaded, im.type, im.version);
    CHECK(lr2021_patch_image[0] == LR2021_PATCH_MAGIC, "magic first");
    lr2021_patch_info_t none = lr2021_patch_parse(0x12345678, 0x31304);
    CHECK(!none.loaded && none.type == 0, "no magic, no patch");

    // a load: every word in place, in frames of 32 words at most, then EnablePram, and the info reads back
    lr2021_t *d = open_dev();
    lr2021_patch_info_t info;
    CHECK(lr2021_patch_info(d, &info) == LR2021_OK && !info.loaded, "none before");
    lr2021_err_t e = lr2021_patch_load(d, NULL, 0);
    CHECK(e == LR2021_OK, "load: %d", e);
    CHECK(!memcmp(M.pram, lr2021_patch_image, sizeof lr2021_patch_image), "image in place");
    CHECK(M.writes == 18 && M.max_words == 32 && M.enables == 1 && M.standbys == 1, "writes %d max %d enables %d",
          M.writes, M.max_words, M.enables);
    CHECK(lr2021_patch_info(d, &info) == LR2021_OK && info.loaded && info.type == 3 && info.version == 0x13, "info after");

    // a second load is refused: only a reset removes the first
    int w = M.writes;
    CHECK(lr2021_patch_load(d, NULL, 0) == LR2021_ERR_INVALID_STATE && M.writes == w, "second load");
    lr2021_close(d);

    // enabled but not running: the magic never shows up
    d = open_dev();
    M.broken = true;
    CHECK(lr2021_patch_load(d, NULL, 0) == LR2021_ERR_INVALID_RESPONSE, "not started");
    lr2021_close(d);

    // a caller's own image (a short one: one frame)
    d = open_dev();
    static const uint32_t mine[5] = {LR2021_PATCH_MAGIC, 0x00070100, 1, 2, 3};
    CHECK(lr2021_patch_load(d, mine, 5) == LR2021_OK && M.writes == 1 && M.pram[4] == 3, "own image");
    CHECK(lr2021_patch_info(d, &info) == LR2021_OK && info.type == 7 && info.version == 1, "own info");
    lr2021_close(d);

    // our patch's frames
    uint8_t fr[3 + 12 * LR2021_RDOF_APPEND_MAX];
    CHECK(lr2021_rdof_cmd(fr, LR2021_RDOF_REPORT) == 3 && fr[0] == 0x02 && fr[1] == 0xF0 && fr[2] == 4, "report frame");
    CHECK(lr2021_rdof_cmd(fr, LR2021_RDOF_APPEND) == 0 && lr2021_rdof_cmd(fr, 5) == 0, "bad sub");
    const lr2021_rdof_entry_t ent[2] = {{0xF30C14, 0x7F, 0x0C}, {0xF30C3C, 0, 0x00290102}};
    static const uint8_t want[] = {0x02, 0xF0, 0x01, 0x00, 0xF3, 0x0C, 0x14, 0, 0, 0, 0x7F, 0, 0, 0, 0x0C,
                                   0x00, 0xF3, 0x0C, 0x3C, 0, 0, 0, 0, 0x00, 0x29, 0x01, 0x02};
    CHECK(lr2021_rdof_append(fr, ent, 2) == sizeof want && !memcmp(fr, want, sizeof want), "append frame");
    CHECK(lr2021_rdof_append(fr, ent, 0) == 0 && lr2021_rdof_append(fr, ent, LR2021_RDOF_APPEND_MAX + 1) == 0,
          "append count");
    static const uint8_t ans[] = {0x52, 0x44, 0x02, 0x00, 3, 1, 0, 0, 1, 2, 0, 0, 0, 9, 0, 1, 0, 0};
    lr2021_rdof_report_t r;
    CHECK(lr2021_rdof_parse_report(ans, sizeof ans, &r) && r.version == LR2021_RDOF_VERSION && r.count == 3 &&
              r.enabled && r.sync_runs == 0x102 && r.rf_runs == 9 && r.samp_runs == 0x10000,
          "parse report");
    CHECK(!lr2021_rdof_parse_report(ans, sizeof ans - 1, &r), "short report");

    // our image in place of Semtech's: the header the ROM checks, then a load
    CHECK(lr2021_rdof_pram_words > 16 && 4 * lr2021_rdof_pram_words <= 0x8C0, "rdof words %zu", lr2021_rdof_pram_words);
    lr2021_patch_info_t ri = lr2021_patch_parse(lr2021_rdof_pram_image[0], lr2021_rdof_pram_image[1]);
    CHECK(ri.loaded && ri.type == LR2021_RDOF_TYPE && (lr2021_rdof_pram_image[1] & 0xFF) == 1, "rdof header");
    CHECK(lr2021_rdof_pram_image[2] > 0x101000 && lr2021_rdof_pram_image[2] < 0x1018C0, "rdof entry 0 %08x",
          (unsigned)lr2021_rdof_pram_image[2]);

    d = open_dev();
    M.st.magic = LR2021_RDOF_STATE_MAGIC, M.st.count = 5, M.st.enabled = 1, M.st.rf_runs = 7; /* an old table */
    memset(&r, 0, sizeof r);
    e = lr2021_patch_load_rdof(d, &r);
    CHECK(e == LR2021_OK && M.rdof, "rdof load: %d", e);
    CHECK(!memcmp(M.pram, lr2021_rdof_pram_image, 4 * lr2021_rdof_pram_words) && M.enables == 1, "ours in bank 1");
    CHECK(r.version == LR2021_RDOF_VERSION && r.count == 0 && !r.enabled && r.rf_runs == 0 && r.cmds == 1,
          "first state %08x %u %d %u", (unsigned)r.version, r.count, r.enabled, (unsigned)r.cmds);
    CHECK(lr2021_patch_info(d, &info) == LR2021_OK && info.loaded && info.type == LR2021_RDOF_TYPE, "our info");
    CHECK(lr2021_patch_load_rdof(d, NULL) == LR2021_ERR_INVALID_STATE, "second rdof load");

    // a table of 23 entries: clear, three appends of 10, 10 and 3, enable; read back both ways
    lr2021_rdof_entry_t tab[23];
    for (int i = 0; i < 23; i++)
        tab[i] = (lr2021_rdof_entry_t){0xF30C00 + 4 * (uint32_t)(i % 16), (uint32_t)i, (uint32_t)i * 3};
    int a0 = M.appends;
    CHECK(lr2021_rdof_set(d, tab, 23, true) == LR2021_OK && M.appends - a0 == 3 && M.st.count == 23 && M.st.enabled,
          "set 23");
    CHECK(!memcmp(M.st.table, tab, sizeof tab), "table in order");
    M.st.sync_runs = 4, M.st.samp_runs = 2;
    CHECK(lr2021_rdof_report(d, &r) == LR2021_OK && r.count == 23 && r.enabled && r.sync_runs == 4 && r.samp_runs == 2,
          "report 23");
    CHECK(lr2021_rdof_state(d, &r) == LR2021_OK && r.count == 23 && r.enabled && r.sync_runs == 4 && r.cmds == 7,
          "state 23: cmds %u", (unsigned)r.cmds);
    CHECK(lr2021_rdof_enable(d, false) == LR2021_OK && !M.st.enabled, "disable");
    CHECK(lr2021_rdof_set(d, NULL, 0, false) == LR2021_OK && M.st.count == 0 && M.st.sync_runs == 4, "clear keeps the counts");
    CHECK(lr2021_rdof_set(d, tab, LR2021_RDOF_MAX_ENTRIES + 1, false) == LR2021_ERR_INVALID_ARG, "too many");
    // a bad address is refused by the chip, and nothing of that append is kept
    const lr2021_rdof_entry_t bad[2] = {{0xF30C14, 0x7F, 1}, {0x100000, 0, 0}};
    CHECK(lr2021_rdof_set(d, bad, 2, false) != LR2021_OK && M.st.count == 0 && M.last_status == LR2021_CMD_PERR,
          "bad address");
    lr2021_close(d);

    // DMR 4-FSK reception: the frames in order, then the BS table, enabled
    d = open_dev();
    CHECK(lr2021_fsk4_dmr_rx(d, LR2021_DMR_BS) == LR2021_ERR_INVALID_STATE, "dmr rx without ours");
    CHECK(lr2021_patch_load_rdof(d, NULL) == LR2021_OK, "rdof for dmr");
    CHECK(lr2021_fsk4_dmr_rx(d, 2) == LR2021_ERR_INVALID_ARG, "dmr rx bad family");
    M.nops = 0;
    CHECK(lr2021_fsk4_dmr_rx(d, LR2021_DMR_BS) == LR2021_OK, "dmr rx bs");
    {
        static const uint16_t want[] = {0x0207, LR2021_SET_FSK_VARIANT, 0x0240, 0x0241, 0x0244, LR2021_SET_FSK_DEMOD_OPT,
                                        0x02F0, 0x02F0, 0x02F0};
        uint16_t got[16];
        int k = 0;
        for (int i = 0; i < M.nops && k < 16; i++)
            if (M.ops[i] != 0x0106 && M.ops[i] != 0x0100) /* reads and the status check after each write */
                got[k++] = M.ops[i];
        CHECK(k == 9 && !memcmp(got, want, sizeof want), "dmr rx order (%d frames)", k);
    }
    CHECK(M.st.count == LR2021_DMR_RX_ENTRIES && M.st.enabled && M.st.table[0].addr == 0xF30C18 &&
              M.st.table[0].value == 0x004D263D && M.st.table[1].value == 0x00B2D9C2 && M.st.table[5].value == 0x7D75DF5D &&
              M.st.table[6].value == 0xD7DF75F7,
          "dmr bs table");
    lr2021_rdof_entry_t ms[LR2021_DMR_RX_ENTRIES];
    CHECK(lr2021_rdof_dmr_rx_table(LR2021_DMR_MS, ms) == LR2021_DMR_RX_ENTRIES && ms[0].value == 0x0089EB91 &&
              ms[1].value == 0x0076146E && ms[5].value == 0xF77FD757 && lr2021_dmr_sync(LR2021_DMR_MS, true) == 0x7F7D5DD57DFDull,
          "dmr ms table");
    lr2021_close(d);

    // Semtech's image running: ours isn't loaded over it
    d = open_dev();
    CHECK(lr2021_patch_load(d, NULL, 0) == LR2021_OK && lr2021_patch_load_rdof(d, NULL) == LR2021_ERR_INVALID_STATE && !M.rdof,
          "rdof over semtech");
    lr2021_close(d);

    // not started: the marker never shows up
    d = open_dev();
    M.broken = true;
    CHECK(lr2021_patch_load_rdof(d, NULL) == LR2021_ERR_INVALID_RESPONSE && !M.rdof, "rdof not started");
    lr2021_close(d);

    // state words: no magic, no state
    uint32_t sw[LR2021_RDOF_STATE_WORDS] = {LR2021_RDOF_VERSION, 0, 1, 2, 3, 4, 5, 6};
    CHECK(!lr2021_rdof_parse_state(sw, &r), "state without magic");
    sw[1] = LR2021_RDOF_STATE_MAGIC;
    CHECK(lr2021_rdof_parse_state(sw, &r) && r.enabled && r.count == 2 && r.sync_runs == 3 && r.rf_runs == 4 &&
              r.samp_runs == 5 && r.cmds == 6,
          "state words");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
