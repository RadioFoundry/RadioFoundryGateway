// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * fakemodem: a simulated MMDVM hotspot modem on a pseudo terminal, for no-RF end-to-end tests of rdofradio (with
 * fakebm, its BrandMeister client). It answers as host-tests/fake_radio.c's fake of `--kind` (mmdvm_hs: a simplex
 * MMDVM_HS board), plays a radio's calls into rdofradio as bursts heard, and logs every burst rdofradio has it send.
 *
 *   fakemodem [--kind mmdvm_hs] [--cc 1] [--src ID] [--delay-s 8] [--gap-s 6] [--rssi N]
 *             [--slot 1|2] [--call DST:private|group:SECONDS]... [--text DST:"TEXT"]... [--confirmed]
 *             [--preambles 16]
 *             [--log FILE] [--path-file FILE] [--exit-after-s S] [--trace]
 *
 * It prints the pty's path (and writes it to --path-file); run `rdofradio --allow-tx --mmdvm PATH ...` on it. Once
 * rdofradio has set the modem up (its first SET_MODE to DMR), it waits --delay-s, then plays each --call and --text
 * in the order given, --gap-s apart: DMR_DATA2 bursts every 60 ms, as a hotspot hands up what it heard on its one
 * slot. --slot sets the slot of the calls and texts after it (2 by default; slot 1 for a duplex modem, `--kind mmdvm`,
 * as a repeater hears a radio on its first slot). A call is a voice LC header, superframes A-F with the call's
 * embedded LC (silence), and a terminator; a text is --preambles preamble CSBKs (16, as a radio sends: BrandMeister
 * answers data only after a long preamble; like --slot, it sets the texts after it), an IPv4/UDP TMS message (port 4007) in a data header and rate 1/2
 * blocks. Both are built by the bench's job builder (rdof/benchjob.h), as a radio sends them (MS sync).
 *
 * The log (stdout, and --log) has a line for each burst rdofradio asks it to transmit (DMR_DATA1/2): "TX s<slot>"
 * with the burst's kind and, where it carries one, its LC or header's source and destination; the voice's embedded
 * LC as each superframe's E burst completes it. Lines "RX" are the bursts it played in, "MODEM" SET_FREQ, SET_MODE
 * and DMR_START. The transmitter drains one burst a 60 ms slot, so GET_STATUS's space is as a modem's.
 */
#define _DARWIN_C_SOURCE /* macOS: posix_openpt, ptsname, cfmakeraw */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "fake_radio.h"
#include "rdof/benchjob.h"
#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

#define MAX_PARTS 16
#define MAX_BURSTS 4096

typedef struct {
    bool text;
    uint32_t dst;
    bool group;
    double seconds;
    int slot; /* 1 or 2 */
    int preambles; /* a text's preamble CSBKs */
    char msg[RDOF_BJ_TEXT];
} part_t;

static struct {
    fake_radio_t radio;
    rdof_mmdvm_reader_t tap; /* what rdofradio writes, for the log */
    int master, slave;
    FILE *log;
    int cc, rssi;
    uint32_t src;
    bool confirmed;
    int preambles; /* --preambles: CSBKs before a text's data header */
    double delay_s, gap_s, exit_after_s;
    part_t parts[MAX_PARTS];
    int nparts, next_part;
    /* playing: bursts, the next one's index and when */
    rdof_bj_burst_t *bursts;
    int nbursts, at, slot;
    double next_at, start_at, done_at;
    bool set_up;
    /* the transmitter's slot clock */
    double drain_at;
    /* the embedded LC of what rdofradio sends, per slot */
    uint8_t frag[2][3][4][32]; /* [TX, RX][slot] */
    unsigned have[2][3];
    unsigned tx_bursts;
    int texts; /* sent: the next TMS sequence */
    bool trace; /* --trace: every command but GET_STATUS */
} M;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    const time_t t = tv.tv_sec;
    gmtime_r(&t, &tm);
    char ts[32];
    snprintf(ts, sizeof ts, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    printf("%s %s\n", ts, line);
    fflush(stdout);
    if (M.log) {
        fprintf(M.log, "%s %s\n", ts, line);
        fflush(M.log);
    }
}

static const char *dt_name(int dt)
{
    static const char *const N[] = {"PI",   "VLC",  "TLC",  "CSBK", "MBCH", "MBCC", "DATA", "R12",
                                    "R34",  "IDLE", "R1",   "USBD", "dt12", "dt13", "dt14", "dt15"};
    return N[dt & 15];
}

/* A burst's line: kind, and the addresses it carries (the voice's embedded LC kept per direction and slot). */
static void describe(const char *dir, int slot, uint8_t control, const uint8_t *burst, char *out, size_t cap)
{
    int dt, n;
    rdof_control_to_dt(control, &dt, &n);
    size_t k = (size_t)snprintf(out, cap, "%s s%d ", dir, slot);
    if (dt == RDOF_DT_VOICE_SYNC || dt == RDOF_DT_VOICE) {
        unsigned *have = &M.have[dir[0] != 'T'][slot & 1 ? 1 : 2];
        uint8_t(*frag)[32] = M.frag[dir[0] != 'T'][slot & 1 ? 1 : 2];
        const int pos = dt == RDOF_DT_VOICE_SYNC ? 0 : n;
        k += (size_t)snprintf(out + k, cap - k, "voice %c", 'A' + pos);
        if (slot < 1 || slot > 2)
            return;
        if (pos == 0) {
            *have = 0;
        } else if (pos <= 4) {
            int cc, pi, lcss, err;
            if (rdof_decode_emb(burst, &cc, &pi, &lcss, &err)) {
                if (pos == 1)
                    *have = 0;
                rdof_embedded_signalling(burst, frag[pos - 1]);
                *have |= 1u << (pos - 1);
            }
            if (pos == 4 && *have == 0xF) {
                uint8_t raw[128], lc9[9];
                for (int i = 0; i < 4; i++)
                    memcpy(raw + 32 * i, frag[i], 32);
                rdof_full_lc_t lc;
                if (rdof_embedded_lc_decode(raw, lc9)) {
                    rdof_parse_full_lc(lc9, &lc);
                    if (lc.src_id != RDOF_NONE)
                        snprintf(out + k, cap - k, " emb LC %ld -> %ld %s", (long)lc.src_id, (long)lc.dst_id,
                                 lc.group ? "TG" : "PC");
                    else
                        snprintf(out + k, cap - k, " emb LC flco %d", lc.flco);
                } else {
                    snprintf(out + k, cap - k, " emb LC bad");
                }
            }
        }
        return;
    }
    k += (size_t)snprintf(out + k, cap - k, "%s", dt_name(dt));
    if (dt == RDOF_DT_VOICE_LC_HEADER || dt == RDOF_DT_TERMINATOR_WITH_LC) {
        rdof_full_lc_t lc;
        if (rdof_decode_voice_lc(burst, dt, &lc) && lc.src_id != RDOF_NONE)
            snprintf(out + k, cap - k, " %ld -> %ld %s", (long)lc.src_id, (long)lc.dst_id, lc.group ? "TG" : "PC");
        else
            snprintf(out + k, cap - k, " LC bad");
    } else if (dt == RDOF_DT_CSBK) {
        rdof_csbk_t c;
        rdof_decode_csbk(burst, &c);
        k += (size_t)snprintf(out + k, cap - k, " opcode %02X%s", c.opcode, c.crc_ok ? "" : " CRC bad");
        if (c.src_id != RDOF_NONE)
            snprintf(out + k, cap - k, " %ld -> %ld %s", (long)c.src_id, (long)c.dst_id, c.group ? "TG" : "PC");
    } else if (dt == RDOF_DT_DATA_HEADER) {
        rdof_data_header_t h;
        rdof_decode_data_header(burst, &h);
        snprintf(out + k, cap - k, " dpf %d sap %d %u -> %u %s blocks %d%s%s", h.dpf, h.sap, (unsigned)h.src_id,
                 (unsigned)h.dst_id, h.group ? "TG" : "PC", h.blocks, h.response_requested ? " response asked" : "",
                 h.crc_ok ? "" : " CRC bad");
        if (h.dpf == RDOF_DPF_RESPONSE)
            snprintf(out + strlen(out), cap - strlen(out), " response class %d type %d status %d", h.response_class,
                     h.response_type, h.response_status);
    }
}

/* What rdofradio wrote, seen before the fake takes it. */
static void tapped(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    char line[256];
    if (M.trace && cmd != RDOF_MMDVM_GET_STATUS)
        say("TRACE cmd %02X len %zu control %02X", cmd, len, len ? p[0] : 0);
    if ((cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2) && len >= 34) {
        M.tx_bursts++;
        describe("TX", cmd == RDOF_MMDVM_DMR_DATA1 ? 1 : 2, p[0], p + 1, line, sizeof line);
        say("%s", line);
    } else if (cmd == RDOF_MMDVM_SET_FREQ && len >= 10) {
        const uint32_t rx = p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24;
        const uint32_t tx = p[5] | p[6] << 8 | p[7] << 16 | (uint32_t)p[8] << 24;
        say("MODEM SET_FREQ rx %.4f tx %.4f MHz level %d", rx / 1e6, tx / 1e6, p[9]);
    } else if (cmd == RDOF_MMDVM_SET_MODE && len >= 1) {
        say("MODEM SET_MODE %d", p[0]);
        if (p[0] == RDOF_MODE_DMR && !M.set_up) {
            M.set_up = true;
            M.start_at = now_s() + M.delay_s;
        }
    } else if (cmd == RDOF_MMDVM_SET_CONFIG && len >= 7) {
        say("MODEM SET_CONFIG %s cc %d", p[0] & 0x80 ? "simplex" : "duplex", p[6]);
    } else if (cmd == RDOF_MMDVM_DMR_START && len >= 1) {
        say("MODEM DMR_START %d", p[0]);
    }
}

static void to_rdofradio(void)
{
    uint8_t buf[4096];
    size_t n;
    while ((n = fake_take(&M.radio, buf, sizeof buf)) > 0)
        for (size_t o = 0; o < n;) {
            const ssize_t w = write(M.master, buf + o, n - o);
            if (w > 0)
                o += (size_t)w;
            else if (w < 0 && errno != EAGAIN && errno != EINTR)
                return;
        }
}

/* the next part's bursts */
static bool build_part(const part_t *p)
{
    rdof_bj_job_t j;
    memset(&j, 0, sizeof j);
    j.cc = j.burst_cc = M.cc;
    j.family = RDOF_FAMILY_MS;
    j.src = M.src;
    j.src2 = M.src;
    j.dst = p->dst;
    j.calls = 1;
    j.late = -1;
    j.udp_port = j.udp_src_port = -1;
    j.udp_payload_len = -1;
    j.repeat = 1;
    if (p->text) {
        j.cmd = RDOF_BJ_DATA;
        j.has_tms = true;
        snprintf(j.tms, sizeof j.tms, "%s", p->msg);
        j.tms_seq = 1 + M.texts++ % 31;
        j.preambles = p->preambles;
        j.confirmed = M.confirmed;
    } else {
        j.cmd = RDOF_BJ_VOICE;
        j.priv = !p->group;
        j.superframes = (int)(p->seconds / 0.36 + 0.5);
        if (j.superframes < 1)
            j.superframes = 1;
        j.headers = 1; /* as a radio keys up: one header, one terminator */
        j.terminators = 1;
    }
    const int cap = rdof_bj_max_bursts(&j);
    free(M.bursts);
    M.bursts = calloc((size_t)cap, sizeof *M.bursts);
    char note[200], done[200], err[200];
    M.nbursts = M.bursts ? rdof_bj_build(&j, 0, M.bursts, cap, note, done, sizeof note, err, sizeof err) : -1;
    if (M.nbursts < 0) {
        say("part %d: %s", M.next_part + 1, M.bursts ? err : "no memory");
        return false;
    }
    if (p->text)
        say("PLAY text %u -> %u \"%s\" (%s, TMS sequence %d) on slot %d: %d bursts", (unsigned)M.src,
            (unsigned)p->dst, p->msg, M.confirmed ? "confirmed" : "unconfirmed", j.tms_seq, p->slot, M.nbursts);
    else
        say("PLAY call %u -> %u %s %.1f s on slot %d: %d bursts", (unsigned)M.src, (unsigned)p->dst,
            p->group ? "TG" : "PC", p->seconds, p->slot, M.nbursts);
    M.at = 0;
    M.slot = p->slot;
    return true;
}

static void play(double now)
{
    if (!M.set_up || now < M.start_at)
        return;
    if (M.at >= M.nbursts) {
        if (M.next_part >= M.nparts) {
            if (!M.done_at)
                M.done_at = now;
            return;
        }
        if (!build_part(&M.parts[M.next_part])) {
            M.next_part++;
            return;
        }
        M.next_part++;
        M.next_at = now;
    }
    while (M.at < M.nbursts && now >= M.next_at) {
        const rdof_bj_burst_t *b = &M.bursts[M.at++];
        char line[256];
        describe("RX", M.slot, b->control, b->burst, line, sizeof line);
        if (b->control & RDOF_SYNC_DATA || M.at == 1 || (b->control & 0x0F) == 0)
            say("%s", line); /* voice: A bursts and the data, not every B-F */
        fake_hear(&M.radio, b->control, b->burst, M.rssi, M.slot);
        M.next_at += 0.060;
        if (M.at == M.nbursts) {
            say("PLAY done");
            M.start_at = now + M.gap_s;
            M.at = M.nbursts = 0;
        }
    }
    to_rdofradio();
}

/* one burst a slot leaves the transmitter's buffer */
static void drain(double now)
{
    if (now < M.drain_at)
        return;
    M.drain_at = now + 0.060;
    if (M.radio.held > 0)
        M.radio.held--;
    if (M.radio.held1 > 0)
        M.radio.held1--;
    if (!M.radio.held && !M.radio.held1 && !M.radio.duplex_tx)
        M.radio.transmitting = false;
}

static void usage(void)
{
    fprintf(stderr, "usage: fakemodem [--kind mmdvm_hs] [--cc 1] [--src ID] [--delay-s 8] [--gap-s 6] [--rssi N]\n"
                    "                 [--slot 1|2] [--call DST:private|group:SECONDS]... [--text DST:TEXT]... [--confirmed]\n"
                    "                 [--preambles 16]\n"
                    "                 [--log FILE] [--path-file FILE] [--exit-after-s S] [--trace]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *kind = "mmdvm_hs", *path_file = NULL;
    int slot = 2;
    M.cc = 1;
    M.src = 3100001;
    M.delay_s = 8;
    M.gap_s = 6;
    M.preambles = 16; /* as a radio sends: BrandMeister answers data only after a long preamble */
    M.rssi = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--kind") && v)
            kind = v, i++;
        else if (!strcmp(a, "--cc") && v)
            M.cc = atoi(v), i++;
        else if (!strcmp(a, "--src") && v)
            M.src = (uint32_t)strtoul(v, NULL, 10), i++;
        else if (!strcmp(a, "--delay-s") && v)
            M.delay_s = atof(v), i++;
        else if (!strcmp(a, "--gap-s") && v)
            M.gap_s = atof(v), i++;
        else if (!strcmp(a, "--rssi") && v)
            M.rssi = atoi(v), i++;
        else if (!strcmp(a, "--slot") && v && (atoi(v) == 1 || atoi(v) == 2))
            slot = atoi(v), i++;
        else if (!strcmp(a, "--preambles") && v && atoi(v) >= 0 && atoi(v) <= 64)
            M.preambles = atoi(v), i++;
        else if (!strcmp(a, "--trace"))
            M.trace = true;
        else if (!strcmp(a, "--confirmed"))
            M.confirmed = true;
        else if (!strcmp(a, "--log") && v)
            M.log = fopen(v, "a"), i++;
        else if (!strcmp(a, "--path-file") && v)
            path_file = v, i++;
        else if (!strcmp(a, "--exit-after-s") && v)
            M.exit_after_s = atof(v), i++;
        else if ((!strcmp(a, "--call") || !strcmp(a, "--text")) && v && M.nparts < MAX_PARTS) {
            part_t *p = &M.parts[M.nparts];
            p->slot = slot;
            p->preambles = M.preambles;
            char *e;
            p->dst = (uint32_t)strtoul(v, &e, 10);
            if (*e != ':' || !p->dst)
                usage();
            e++;
            if (a[2] == 't') {
                p->text = true;
                snprintf(p->msg, sizeof p->msg, "%s", e);
            } else {
                if (!strncmp(e, "private:", 8))
                    e += 8;
                else if (!strncmp(e, "group:", 6))
                    p->group = true, e += 6;
                else
                    usage();
                p->seconds = atof(e);
                if (p->seconds <= 0)
                    p->seconds = 2;
            }
            M.nparts++, i++;
        } else
            usage();
    }
    fake_init(&M.radio, kind);
    if (!M.radio.protocol) {
        fprintf(stderr, "fakemodem: --kind %s: one of host-tests/fake_radio.c's (mmdvm_hs, mmdvm, odr, ...)\n", kind);
        return 2;
    }
    rdof_mmdvm_reader_init(&M.tap);
    M.master = posix_openpt(O_RDWR | O_NOCTTY);
    if (M.master < 0 || grantpt(M.master) || unlockpt(M.master)) {
        perror("fakemodem: posix_openpt");
        return 1;
    }
    const char *slave = ptsname(M.master);
    /* held open here, raw, so the master never reads EIO between rdofradio's runs */
    M.slave = slave ? open(slave, O_RDWR | O_NOCTTY) : -1;
    if (M.slave < 0) {
        perror("fakemodem: the pty's slave");
        return 1;
    }
    struct termios t;
    if (!tcgetattr(M.slave, &t)) {
        cfmakeraw(&t);
        tcsetattr(M.slave, TCSANOW, &t);
    }
    fcntl(M.master, F_SETFL, fcntl(M.master, F_GETFL) | O_NONBLOCK);
    printf("%s\n", slave);
    fflush(stdout);
    if (path_file) {
        FILE *f = fopen(path_file, "w");
        if (f) {
            fprintf(f, "%s\n", slave);
            fclose(f);
        }
    }
    say("fakemodem: %s (%s) on %s, cc %d, source %u, %d part(s)", kind, M.radio.description, slave, M.cc,
        (unsigned)M.src, M.nparts);
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(M.master, &fds);
        struct timeval tv = {0, 5000};
        if (select(M.master + 1, &fds, NULL, NULL, &tv) > 0) {
            uint8_t buf[2048];
            const ssize_t n = read(M.master, buf, sizeof buf);
            if (n > 0) {
                rdof_mmdvm_reader_feed(&M.tap, buf, (size_t)n, tapped, NULL);
                fake_write(&M.radio, buf, (size_t)n);
                to_rdofradio();
            }
        }
        const double now = now_s();
        drain(now);
        play(now);
        if (M.exit_after_s > 0 && M.done_at && now - M.done_at > M.exit_after_s) {
            say("fakemodem: done, %u bursts transmitted", M.tx_bursts);
            return 0;
        }
    }
}
