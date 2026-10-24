// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for wifilist's portable parts: ranking, the list checks, edits
// that keep passwords, and the JSON (which must never carry a password).
#include <stdio.h>
#include <string.h>

#include "wifilist_core.h"

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

static wifilist_net_t net(const char *ssid, const char *pass, int prio, bool hidden)
{
    wifilist_net_t w = {.priority = (int8_t)prio, .hidden = hidden};
    snprintf(w.ssid, sizeof(w.ssid), "%s", ssid);
    snprintf(w.password, sizeof(w.password), "%s", pass);
    return w;
}

static wifilist_ap_t ap(const char *ssid, int rssi, int ch, bool open)
{
    wifilist_ap_t a = {.rssi = (int8_t)rssi, .channel = (uint8_t)ch, .open = open};
    snprintf(a.ssid, sizeof(a.ssid), "%s", ssid);
    a.bssid[5] = (uint8_t)ch;
    return a;
}

// The order as "name,name,..." (the AP's channel after a colon, h: hidden).
static void order(const wifilist_net_t *n, int nn, const wifilist_ap_t *a, int na, const char *want)
{
    wifilist_cand_t c[WIFILIST_MAX];
    int k = wifilist_rank(n, nn, a, na, c, WIFILIST_MAX);
    char got[256] = "";
    for (int i = 0; i < k; i++) {
        char t[48];
        if (c[i].ap >= 0) {
            snprintf(t, sizeof(t), "%s%s:%d", i ? "," : "", n[c[i].net].ssid, a[c[i].ap].channel);
        } else {
            snprintf(t, sizeof(t), "%s%s:h", i ? "," : "", n[c[i].net].ssid);
        }
        strcat(got, t);
    }
    CHECK(!strcmp(got, want), "rank: want %s, got %s", want, got);
}

static void ranking(void)
{
    wifilist_net_t n[] = {net("home", "password1", 10, false), net("event", "password2", 50, false),
                          net("phone", "password3", 10, false), net("cafe", "", 0, false),
                          net("secret", "password4", 20, true)};
    wifilist_ap_t a[] = {ap("home", -70, 1, false), ap("event", -85, 6, false), ap("event", -60, 11, false),
                         ap("phone", -50, 3, false), ap("cafe", -40, 9, true), ap("other", -30, 4, false)};
    // event (50) at its strongest AP; secret (20, hidden, not seen); home and phone (10) by signal; cafe (0).
    order(n, 5, a, 6, "event:11,secret:h,phone:3,home:1,cafe:9");
    // Nothing in sight: only the hidden one.
    order(n, 5, a, 0, "secret:h");
    // The event network out of range: fall back down the list.
    order(n, 5, a + 3, 3, "secret:h,phone:3,cafe:9");
    // A protected network seen open (or the other way) isn't tried.
    wifilist_ap_t twin[] = {ap("home", -40, 5, true), ap("cafe", -40, 7, false)};
    order(n, 4, twin, 2, "");
    // Equal priority and signal: list order.
    wifilist_net_t e[] = {net("a", "", 1, false), net("b", "", 1, false)};
    wifilist_ap_t ea[] = {ap("b", -60, 2, true), ap("a", -60, 1, true)};
    order(e, 2, ea, 2, "a:1,b:2");
}

static void checking(void)
{
    char err[128];
    wifilist_net_t ok[] = {net("a", "", 0, false), net("b", "12345678", 100, false),
                           net("c", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", -100, false)};
    CHECK(wifilist_check(ok, 3, err, sizeof(err)), "valid list: %s", err);
    wifilist_net_t bad[][1] = {{net("", "", 0, false)}, {net("x", "short", 0, false)},
                               {net("x", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", 0, false)},
                               {net("x", "", 101, false)}};
    for (int i = 0; i < 4; i++) {
        CHECK(!wifilist_check(bad[i], 1, err, sizeof(err)), "bad entry %d accepted", i);
    }
    wifilist_net_t dup[] = {net("x", "", 0, false), net("x", "", 1, false)};
    CHECK(!wifilist_check(dup, 2, err, sizeof(err)) && strstr(err, "twice"), "duplicate: %s", err);
    wifilist_net_t many[WIFILIST_MAX + 1];
    for (int i = 0; i <= WIFILIST_MAX; i++) {
        char s[8];
        snprintf(s, sizeof(s), "n%d", i);
        many[i] = net(s, "", 0, false);
    }
    CHECK(wifilist_check(many, WIFILIST_MAX, err, sizeof(err)), "a full list");
    CHECK(!wifilist_check(many, WIFILIST_MAX + 1, err, sizeof(err)), "one too many");

    wifilist_net_t s[] = {net("a", "", 0, false), net("b", "", 5, false), net("c", "", 0, false), net("d", "", 5, false)};
    wifilist_sort(s, 4);
    CHECK(!strcmp(s[0].ssid, "b") && !strcmp(s[1].ssid, "d") && !strcmp(s[2].ssid, "a") && !strcmp(s[3].ssid, "c"),
          "stable sort: %s %s %s %s", s[0].ssid, s[1].ssid, s[2].ssid, s[3].ssid);
}

static void merging(void)
{
    char err[128];
    wifilist_net_t old[] = {net("home", "homepass1", 10, false), net("event", "eventpass", 50, false)};
    wifilist_edit_t in[4];
    memset(in, 0, sizeof(in));
    // Keep event's password; rename home to home5g (its password follows); add an open cafe; add one with a password.
    in[0].net = net("event", "", 60, false);
    in[1].net = net("home5g", "", 10, false);
    snprintf(in[1].was, sizeof(in[1].was), "home");
    in[2].net = net("cafe", "", 0, false);
    in[3].net = net("new", "newpass12", 70, true);
    in[3].has_password = true;
    wifilist_net_t out[WIFILIST_MAX];
    int n = wifilist_merge(old, 2, in, 4, out, err, sizeof(err));
    CHECK(n == 4, "merged 4: %d %s", n, err);
    CHECK(!strcmp(out[0].ssid, "new") && !strcmp(out[0].password, "newpass12") && out[0].hidden, "new first");
    CHECK(!strcmp(out[1].ssid, "event") && !strcmp(out[1].password, "eventpass") && out[1].priority == 60, "event kept");
    CHECK(!strcmp(out[2].ssid, "home5g") && !strcmp(out[2].password, "homepass1"), "renamed, password followed");
    CHECK(!strcmp(out[3].ssid, "cafe") && !out[3].password[0], "new without a password: open");
    // A password that's too short is refused.
    in[0].has_password = true;
    snprintf(in[0].net.password, sizeof(in[0].net.password), "short");
    CHECK(wifilist_merge(old, 2, in, 4, out, err, sizeof(err)) < 0 && strstr(err, "8 to 63"), "refused: %s", err);
    // Delete everything.
    CHECK(wifilist_merge(old, 2, in, 0, out, err, sizeof(err)) == 0, "empty list");
}

static void json(void)
{
    static wifilist_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = "connected";
    st.connected = true;
    snprintf(st.ssid, sizeof(st.ssid), "home");
    st.bssid[0] = 0xAA;
    st.rssi = -61;
    st.channel = 6;
    snprintf(st.ip, sizeof(st.ip), "10.0.0.5");
    st.uptime_s = 100;
    st.n_nets = 2;
    snprintf(st.nets[0].ssid, 33, "home");
    st.nets[0].priority = 10;
    st.nets[0].password_set = true;
    st.nets[0].rssi = -61;
    snprintf(st.nets[1].ssid, 33, "q\"uote");
    st.nets[1].rssi = -128;
    st.n_seen = 1;
    st.seen[0] = ap("home", -61, 6, false);
    st.n_fail = 1;
    st.fails[0] = (wifilist_fail_t){.t_s = 40, .reason = 15, .rssi = -80};
    snprintf(st.fails[0].ssid, 33, "event");
    char buf[4096];
    size_t n = wifilist_status_json(&st, buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf), "status JSON");
    CHECK(!strncmp(buf, "{\"state\":\"connected\",\"connected\":true,\"ssid\":\"home\",\"bssid\":\"aa:00:00:00:00:00\",\"rssi\":-61,", 90),
          "head %.100s", buf);
    CHECK(strstr(buf, "{\"ssid\":\"home\",\"priority\":10,\"hidden\":false,\"password_set\":true,\"rssi\":-61}") &&
              strstr(buf, "{\"ssid\":\"q\\\"uote\",\"priority\":0,\"hidden\":false,\"password_set\":false}"),
          "networks %.300s", strstr(buf, "\"networks\""));
    CHECK(strstr(buf, "\"why\":\"4-way handshake timeout (wrong password?)\""), "failure reason");
    CHECK(wifilist_status_json(&st, buf, 50) == 0 && !buf[0], "too small: 0");

    wifilist_net_t nets[] = {net("home", "secretpass", 10, false), net("hid", "otherpass1", 5, true)};
    n = wifilist_networks_json(nets, 2, buf, sizeof(buf));
    CHECK(!strcmp(buf, "{\"networks\":[{\"ssid\":\"home\",\"priority\":10},{\"ssid\":\"hid\",\"priority\":5,\"hidden\":true}]}"),
          "export %s", buf);
    CHECK(!strstr(buf, "secretpass") && !strstr(buf, "otherpass1"), "no passwords in the export");
    CHECK(!strcmp(wifilist_reason(201), "no AP found") && !strcmp(wifilist_reason(0), "timed out"), "reasons");
}

// The wait between scans: 5 s doubling up to the configured maximum, whatever that is.
static void backoff(void)
{
    static const uint32_t want30[] = {5, 10, 20, 30, 30, 30};
    for (int r = 0; r < 6; r++) {
        CHECK(wifilist_backoff_s(r, 30, false) == want30[r], "round %d with a cap of 30: %u", r, (unsigned)wifilist_backoff_s(r, 30, false));
    }
    static const uint32_t want600[] = {5, 10, 20, 40, 80, 160, 320, 600, 600};
    for (int r = 0; r < 9; r++) {
        CHECK(wifilist_backoff_s(r, 600, false) == want600[r], "round %d with a cap of 600: %u", r, (unsigned)wifilist_backoff_s(r, 600, false));
    }
    CHECK(wifilist_backoff_s(0, 5, false) == 5 && wifilist_backoff_s(7, 5, false) == 5, "a cap of 5");
    CHECK(wifilist_backoff_s(0, 3, false) == 3, "a cap under the first wait");
    CHECK(wifilist_backoff_s(-3, 30, false) == 5, "no rounds yet");
    CHECK(wifilist_backoff_s(1000000, 600, false) == 600, "many rounds");
    CHECK(wifilist_backoff_s(100, UINT32_MAX, false) >= 5u << 29, "an enormous cap doesn't overflow");
    CHECK(wifilist_backoff_s(0, 30, true) == 30 && wifilist_backoff_s(3, 600, true) == 600, "the setup AP up: always the cap");
}

static void ap_password(void)
{
    CHECK(wifilist_ap_password_ok(NULL) && wifilist_ap_password_ok(""), "none is an open AP");
    CHECK(!wifilist_ap_password_ok("1234567") && wifilist_ap_password_ok("12345678"), "8 characters");
    char p[80];
    memset(p, 'x', sizeof(p));
    p[63] = 0;
    CHECK(wifilist_ap_password_ok(p), "63 characters");
    p[63] = 'x';
    p[64] = 0;
    CHECK(!wifilist_ap_password_ok(p), "64 characters");
}

// The NVS blob: round trip, and what makes one not a list.
static void blob(void)
{
    wifilist_net_t nets[3] = {net("home", "password1", 10, false), net("hidden one", "", -5, true),
                              net("0123456789012345678901234567890X", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 100, false)};
    uint8_t b[2 + 3 * WIFILIST_BLOB_REC + 5];
    CHECK(wifilist_blob_size(3) == 2 + 3 * 100 && wifilist_blob_size(0) == 2 && wifilist_blob_size(-1) == 2, "sizes");
    memset(b, 0xEE, sizeof(b));
    CHECK(wifilist_blob_encode(nets, 3, b) == 302, "encoded size");
    CHECK(b[0] == 1 && b[1] == 3 && b[2] == 'h' && b[2 + 4] == 0 && b[2 + 33] == 'p' && (int8_t)b[2 + 98] == 10 && b[2 + 99] == 0,
          "layout of the first record");
    CHECK((int8_t)b[2 + 100 + 98] == -5 && b[2 + 100 + 99] == 1, "negative priority and the hidden flag");
    CHECK(b[2 + 100 + 33] == 0 && b[2 + 100 + 20] == 0, "padding is zero, not stale bytes");
    CHECK(b[302] == 0xEE, "nothing written past the blob");
    wifilist_net_t out[WIFILIST_MAX];
    memset(out, 0x55, sizeof(out));
    CHECK(wifilist_blob_decode(b, 302, out, WIFILIST_MAX) == 3, "decoded");
    CHECK(!strcmp(out[0].ssid, "home") && !strcmp(out[0].password, "password1") && out[0].priority == 10 && !out[0].hidden, "first");
    CHECK(!strcmp(out[1].ssid, "hidden one") && !out[1].password[0] && out[1].priority == -5 && out[1].hidden, "second");
    CHECK(strlen(out[2].ssid) == 32 && strlen(out[2].password) == 64 && out[2].priority == 100, "full-length strings");
    CHECK(wifilist_blob_decode(b, 302 + 5, out, WIFILIST_MAX) == 3, "trailing bytes are ignored");
    CHECK(wifilist_blob_decode(b, 302, out, 2) == 2, "at most `max`");
    CHECK(wifilist_blob_decode(b, 302, out, 0) == 0, "max 0");
    // empty list
    wifilist_blob_encode(nets, 0, b);
    CHECK(wifilist_blob_decode(b, 2, out, WIFILIST_MAX) == 0, "an empty list is a list");
    wifilist_blob_encode(nets, -4, b);
    CHECK(b[1] == 0, "negative count encodes as empty");
    // not lists
    wifilist_blob_encode(nets, 3, b);
    CHECK(wifilist_blob_decode(NULL, 302, out, WIFILIST_MAX) == -1, "no blob");
    CHECK(wifilist_blob_decode(b, 0, out, WIFILIST_MAX) == -1 && wifilist_blob_decode(b, 1, out, WIFILIST_MAX) == -1, "shorter than the header");
    CHECK(wifilist_blob_decode(b, 301, out, WIFILIST_MAX) == -1, "one byte short of its records");
    CHECK(wifilist_blob_decode(b, 202, out, WIFILIST_MAX) == -1, "counts more records than it holds");
    b[0] = 2;
    CHECK(wifilist_blob_decode(b, 302, out, WIFILIST_MAX) == -1, "another version");
    b[0] = 0;
    CHECK(wifilist_blob_decode(b, 302, out, WIFILIST_MAX) == -1, "version 0");
    b[0] = 1;
    b[1] = WIFILIST_MAX + 1;
    uint8_t big[2 + (WIFILIST_MAX + 1) * WIFILIST_BLOB_REC] = {1, WIFILIST_MAX + 1};
    CHECK(wifilist_blob_decode(big, sizeof(big), out, WIFILIST_MAX) == -1, "more networks than the list holds");
    big[1] = 255;
    CHECK(wifilist_blob_decode(big, sizeof(big), out, WIFILIST_MAX) == -1, "a count of 255");
    // strings that fill their field without an end are cut
    memset(b, 0, sizeof(b));
    b[0] = 1;
    b[1] = 1;
    memset(b + 2, 'A', 33);
    memset(b + 2 + 33, 'B', 65);
    CHECK(wifilist_blob_decode(b, 102, out, WIFILIST_MAX) == 1 && strlen(out[0].ssid) == 32 && strlen(out[0].password) == 64,
          "unterminated strings get a NUL");
}

// Strings in the JSON are UTF-8, whatever bytes an SSID holds.
static void utf8(void)
{
    static wifilist_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = "waiting";
    char buf[2048];
    struct {
        const char *ssid;
        const char *want;
    } cases[] = {
        {"caf\xC3\xA9", "caf\xC3\xA9"},                           // 2-byte: kept
        {"\xE2\x82\xAC", "\xE2\x82\xAC"},                         // 3-byte (euro): kept
        {"\xF0\x9F\x93\xB6", "\xF0\x9F\x93\xB6"},                 // 4-byte (emoji): kept
        {"a\xFFz", "a\\ufffdz"},                                  // not UTF-8 at all
        {"\x80", "\\ufffd"},                                      // a stray continuation byte
        {"\xC3", "\\ufffd"},                                      // a lead byte cut off by the end
        {"\xC3(", "\\ufffd("},                                    // a lead byte and no continuation
        {"\xC0\xAF", "\\ufffd\\ufffd"},                            // overlong
        {"\xE0\x80\x80", "\\ufffd\\ufffd\\ufffd"},                // overlong, 3 bytes
        {"\xED\xA0\x80", "\\ufffd\\ufffd\\ufffd"},                // a surrogate
        {"\xF0\x80\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},     // overlong, 4 bytes
        {"\xF4\x90\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},     // past U+10FFFF
        {"\xF5\x80\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},     // a lead byte that can't start anything
        {"\xE2\x82", "\\ufffd\\ufffd"},                           // a 3-byte sequence cut short
        {"\x01\x1f\x7f", "\\u0001\\u001f\x7f"},                   // controls under 0x20; DEL is allowed
        {"a\\b\"c", "a\\\\b\\\"c"},                               // backslash and quote
        {"\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF"},                 // U+10FFFF: kept
        {"\xED\x9F\xBF", "\xED\x9F\xBF"},                         // U+D7FF: kept
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st.n_nets = 1;
        snprintf(st.nets[0].ssid, sizeof(st.nets[0].ssid), "%s", cases[i].ssid);
        st.nets[0].rssi = -128;
        size_t n = wifilist_status_json(&st, buf, sizeof(buf));
        char want[128];
        snprintf(want, sizeof(want), "\"networks\":[{\"ssid\":\"%s\",", cases[i].want);
        CHECK(n > 0 && strstr(buf, want), "case %zu: want %s in %.200s", i, want, strstr(buf, "\"networks\""));
    }
    // the export too
    wifilist_net_t w = net("x", "", 0, false);
    w.ssid[0] = (char)0xFE;
    char out[256];
    CHECK(wifilist_networks_json(&w, 1, out, sizeof(out)) > 0 && strstr(out, "\"ssid\":\"\\ufffd\""), "export: %s", out);
}

// Every JSON shape and size boundary.
static void json_shapes(void)
{
    static wifilist_status_t st;
    memset(&st, 0, sizeof(st));
    char buf[8192];
    // not connected, nothing known: no bssid / rssi / channel, a NULL state is empty text
    size_t n = wifilist_status_json(&st, buf, sizeof(buf));
    CHECK(n > 0 && !strncmp(buf, "{\"state\":\"\",\"connected\":false,\"ssid\":\"\",\"ip\":\"\",\"ap_up\":false,\"ap_ssid\":\"\",\"scans\":0,"
                                 "\"connects\":0,\"uptime_s\":0,\"networks\":[],\"seen\":[],\"fails\":[]}",
                 n),
          "idle status: %s", buf);
    // the SSID of a connection that isn't up is not shown
    snprintf(st.ssid, sizeof(st.ssid), "stale");
    n = wifilist_status_json(&st, buf, sizeof(buf));
    CHECK(strstr(buf, "\"ssid\":\"\"") && !strstr(buf, "stale"), "no SSID while disconnected");
    // the access point and counters
    st.state = "waiting";
    st.ap_up = true;
    snprintf(st.ap_ssid, sizeof(st.ap_ssid), "box-1A2B");
    st.scans = 4000000000u;
    st.connects = 7;
    st.uptime_s = 4294967295u;
    n = wifilist_status_json(&st, buf, sizeof(buf));
    CHECK(strstr(buf, "\"ap_up\":true,\"ap_ssid\":\"box-1A2B\",\"scans\":4000000000,\"connects\":7,\"uptime_s\":4294967295,"), "AP and counters: %s", buf);
    // a full list of everything
    st.connected = true;
    st.n_nets = WIFILIST_MAX;
    st.n_seen = WIFILIST_SEEN;
    st.n_fail = WIFILIST_FAILS;
    for (int i = 0; i < WIFILIST_MAX; i++) {
        snprintf(st.nets[i].ssid, 33, "net%d", i);
        st.nets[i].rssi = i % 2 ? -128 : (int8_t)(-30 - i);
        st.nets[i].hidden = i % 3 == 0;
    }
    for (int i = 0; i < WIFILIST_SEEN; i++) {
        st.seen[i] = ap("seen", -40 - i, 1 + i % 13, i % 2);
        st.seen[i].bssid[0] = (uint8_t)(0xA0 + i);
    }
    for (int i = 0; i < WIFILIST_FAILS; i++) {
        st.fails[i] = (wifilist_fail_t){.t_s = (uint32_t)i * 10, .reason = (uint8_t)(i * 30), .rssi = -90, .lost = i & 1};
        snprintf(st.fails[i].ssid, 33, "f%d", i);
    }
    n = wifilist_status_json(&st, buf, sizeof(buf));
    CHECK(n > 1000 && n == strlen(buf) && buf[n - 1] == '}', "a full status (%zu bytes)", n);
    CHECK(strstr(buf, "{\"ssid\":\"net1\",\"priority\":0,\"hidden\":false,\"password_set\":false}") &&
              strstr(buf, "{\"ssid\":\"net0\",\"priority\":0,\"hidden\":true,\"password_set\":false,\"rssi\":-30}"),
          "rssi only for the networks seen");
    CHECK(strstr(buf, "\"bssid\":\"a0:00:00:00:00:01\",\"rssi\":-40,\"channel\":1,\"open\":false}") &&
              strstr(buf, "\"bssid\":\"af:00:00:00:00:03\",\"rssi\":-55,\"channel\":3,\"open\":true}"),
          "seen access points");
    CHECK(strstr(buf, "{\"t_s\":70,\"ssid\":\"f7\",\"reason\":210,\"why\":\"no AP with compatible security\",\"rssi\":-90,\"lost\":true}"),
          "failures");
    // a buffer of exactly the right size: n + 1 holds it, n does not
    char exact[8192];
    CHECK(wifilist_status_json(&st, exact, n + 1) == n && !strcmp(exact, buf), "exact fit");
    CHECK(wifilist_status_json(&st, exact, n) == 0 && !exact[0], "one byte short");
    CHECK(wifilist_status_json(&st, exact, 1) == 0 && !exact[0], "one byte");
    CHECK(wifilist_status_json(&st, exact, 0) == 0, "no buffer");
    // every size below the length fails cleanly
    int bad = 0;
    for (size_t cap = 1; cap < n; cap += 7) {
        memset(exact, 'z', sizeof(exact));
        if (wifilist_status_json(&st, exact, cap) != 0 || exact[0] != 0) {
            bad++;
        }
    }
    CHECK(!bad, "%d short buffers not refused", bad);
    // the export
    wifilist_net_t nets[WIFILIST_MAX];
    for (int i = 0; i < WIFILIST_MAX; i++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "n%d", i);
        nets[i] = net(nm, "secret123", i - 3, i & 1);
    }
    size_t en = wifilist_networks_json(nets, WIFILIST_MAX, buf, sizeof(buf));
    CHECK(en > 0 && !strstr(buf, "secret") && strstr(buf, "{\"ssid\":\"n0\",\"priority\":-3},{\"ssid\":\"n1\",\"priority\":-2,\"hidden\":true}"), "export of a full list");
    CHECK(wifilist_networks_json(nets, 0, exact, sizeof(exact)) == 15 && !strcmp(exact, "{\"networks\":[]}"), "empty export: %s", exact);
    CHECK(wifilist_networks_json(nets, WIFILIST_MAX, exact, en + 1) == en, "export exact fit");
    CHECK(wifilist_networks_json(nets, WIFILIST_MAX, exact, en) == 0 && !exact[0], "export one byte short");
    CHECK(wifilist_networks_json(nets, 2, exact, 0) == 0, "export, no buffer");
}

static void reasons(void)
{
    static const struct {
        int code;
        const char *text;
    } r[] = {{0, "timed out"}, {1, "unspecified"}, {2, "authentication expired"}, {3, "deauthenticated (AP leaving)"},
             {4, "disassociated (inactive)"}, {5, "AP full"}, {6, "not authenticated"}, {7, "not associated"},
             {8, "disassociated (leaving)"}, {14, "MIC failure"}, {15, "4-way handshake timeout (wrong password?)"},
             {16, "group key timeout"}, {23, "802.1X authentication failed"}, {200, "beacon timeout"}, {201, "no AP found"},
             {202, "authentication failed"}, {203, "association failed"}, {204, "handshake timeout (wrong password?)"},
             {205, "connection failed"}, {206, "AP TSF reset"}, {207, "roaming"}, {208, "association comeback too long"},
             {209, "SA query timeout"}, {210, "no AP with compatible security"}, {211, "no AP within the auth threshold"},
             {212, "no AP within the RSSI threshold"}};
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        CHECK(!strcmp(wifilist_reason((uint8_t)r[i].code), r[i].text), "reason %d: %s", r[i].code, wifilist_reason((uint8_t)r[i].code));
    }
    CHECK(!strcmp(wifilist_reason(9), "other") && !strcmp(wifilist_reason(213), "other") && !strcmp(wifilist_reason(255), "other"), "others");
}

// Edge cases of ranking, checking and merging.
static void edges(void)
{
    char err[128];
    // rank: the output holds at most `max`; a list longer than WIFILIST_MAX is cut; no networks
    wifilist_net_t n[WIFILIST_MAX + 2];
    wifilist_ap_t a[WIFILIST_MAX + 2];
    for (int i = 0; i < WIFILIST_MAX + 2; i++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "n%d", i);
        n[i] = net(nm, "", 50 - i, false); // a stored list: highest priority first
        a[i] = ap(nm, -50, 1, true);
    }
    wifilist_cand_t c[WIFILIST_MAX + 2];
    CHECK(wifilist_rank(n, WIFILIST_MAX + 2, a, WIFILIST_MAX + 2, c, WIFILIST_MAX + 2) == WIFILIST_MAX, "only the first %d networks", WIFILIST_MAX);
    CHECK(wifilist_rank(n, 5, a, 5, c, 3) == 3 && n[c[0].net].priority == 50 && n[c[2].net].priority == 48, "output limit of 3");
    CHECK(wifilist_rank(n, 5, a, 5, c, 0) == 0, "output limit of 0");
    CHECK(wifilist_rank(n, 0, a, 5, c, 3) == 0, "no networks");
    // a stronger AP of the same name wins even if the weaker one is listed first; an AP with another security is skipped
    wifilist_net_t one = net("same", "password1", 0, false);
    wifilist_ap_t twins[3] = {ap("same", -80, 1, false), ap("same", -30, 2, true), ap("same", -60, 3, false)};
    wifilist_cand_t cc[1];
    CHECK(wifilist_rank(&one, 1, twins, 3, cc, 1) == 1 && cc[0].ap == 2, "strongest protected AP of the name");
    // hidden networks seen are ranked by their signal like others; unseen ones are the weakest of their priority
    wifilist_net_t h[3] = {net("h1", "", 0, true), net("h2", "", 0, true), net("v", "", 0, false)};
    wifilist_ap_t ha[2] = {ap("h2", -90, 4, true), ap("v", -95, 5, true)};
    order(h, 3, ha, 2, "h2:4,v:5,h1:h");
    // an AP with no name never matches a network
    wifilist_ap_t blank[1] = {ap("", -20, 1, true)};
    wifilist_net_t vis = net("v", "", 0, false);
    CHECK(wifilist_rank(&vis, 1, blank, 1, cc, 1) == 0, "an unnamed AP matches nothing visible");
    // sort: nothing and one
    wifilist_sort(NULL, 0);
    wifilist_net_t lone = net("a", "", 3, false);
    wifilist_sort(&lone, 1);
    CHECK(!strcmp(lone.ssid, "a"), "sort of one");
    // check: error text for each refusal and no place to put it
    wifilist_net_t bad = net("x", "", 0, false);
    CHECK(!wifilist_check(&bad, -1, err, sizeof(err)) && strstr(err, "at most"), "negative count: %s", err);
    CHECK(wifilist_check(&bad, 0, err, sizeof(err)), "zero networks");
    bad = net("", "", 0, false);
    CHECK(!wifilist_check(&bad, 1, err, sizeof(err)) && strstr(err, "network 1: a name"), "empty name: %s", err);
    memset(bad.ssid, 'a', 33); // no NUL in the field: longer than 32
    CHECK(!wifilist_check(&bad, 1, err, sizeof(err)) && strstr(err, "network 1"), "33 bytes: %s", err);
    bad = net("x", "1234567", 0, false);
    CHECK(!wifilist_check(&bad, 1, err, sizeof(err)) && strstr(err, "x: a password of 8 to 63"), "short password: %s", err);
    memset(bad.password, 'p', 64); // 64 characters, not hex, with no NUL in the field
    CHECK(!wifilist_check(&bad, 1, err, sizeof(err)) && strstr(err, "must be hex"), "64 non-hex: %s", err);
    bad = net("x", "", -101, false);
    CHECK(!wifilist_check(&bad, 1, err, sizeof(err)) && strstr(err, "priority -100 to 100"), "priority: %s", err);
    bad = net("x", "", 100, false);
    CHECK(wifilist_check(&bad, 1, NULL, 0), "no error buffer is fine");
    bad.priority = 101;
    CHECK(!wifilist_check(&bad, 1, NULL, 0), "refused with no error buffer");
    CHECK(!wifilist_check(&bad, 1, err, 0), "refused with a zero-size error buffer");
    wifilist_net_t hex[1] = {net("x", "ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789", 0, false)};
    CHECK(wifilist_check(hex, 1, err, sizeof(err)), "a 64 digit PSK in either case: %s", err);
    char longpw[65];
    memset(longpw, '1', 64);
    longpw[63] = 0;
    wifilist_net_t p63[1] = {net("x", longpw, 0, false)};
    CHECK(wifilist_check(p63, 1, err, sizeof(err)), "63 characters");
    // merge: too many edits; a kept password for a name that is not in the old list is open; the old list is not modified
    wifilist_edit_t many[WIFILIST_MAX + 1];
    memset(many, 0, sizeof(many));
    wifilist_net_t out[WIFILIST_MAX + 1];
    CHECK(wifilist_merge(NULL, 0, many, WIFILIST_MAX + 1, out, err, sizeof(err)) == -1 && strstr(err, "at most"), "too many edits: %s", err);
    wifilist_edit_t e1 = {.net = net("fresh", "", 0, false)};
    wifilist_net_t old1[1] = {net("other", "otherpass1", 0, false)};
    CHECK(wifilist_merge(old1, 1, &e1, 1, out, err, sizeof(err)) == 1 && !out[0].password[0], "unknown name: open");
    snprintf(e1.was, sizeof(e1.was), "other"); // renamed from a network that exists
    CHECK(wifilist_merge(old1, 1, &e1, 1, out, err, sizeof(err)) == 1 && !strcmp(out[0].password, "otherpass1"), "was: the old password follows");
    snprintf(e1.was, sizeof(e1.was), "gone"); // renamed from one that doesn't
    CHECK(wifilist_merge(old1, 1, &e1, 1, out, err, sizeof(err)) == 1 && !out[0].password[0], "was: unknown, open");
    // a kept password goes through the checks as well: a stored one that is no longer valid is refused
    wifilist_net_t old2[1] = {net("fresh", "short", 0, false)};
    e1.was[0] = 0;
    CHECK(wifilist_merge(old2, 1, &e1, 1, out, err, sizeof(err)) == -1 && strstr(err, "8 to 63"), "stored password re-checked: %s", err);
    // an error without a buffer
    CHECK(wifilist_merge(old2, 1, &e1, 1, out, NULL, 0) == -1, "refused with no error buffer");
    CHECK(wifilist_merge(NULL, 0, many, WIFILIST_MAX + 1, out, NULL, 0) == -1, "too many, no error buffer");
}

int main(void)
{
    ranking();
    checking();
    merging();
    json();
    backoff();
    ap_password();
    blob();
    utf8();
    json_shapes();
    reasons();
    edges();
    printf("wifilist: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
