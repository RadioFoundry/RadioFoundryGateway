// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// gnss's portable parts: the NMEA parser, the burst timing and the JSON.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gnss.h"
#include "gnss_nmea.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                 \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(c)) {                                                                                                   \
            fails++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

// A sentence with its checksum: body is what goes between '$' and '*'.
static const char *sent(const char *body)
{
    static char out[160];
    unsigned sum = 0;
    for (const char *p = body; *p; p++) {
        sum ^= (unsigned char)*p;
    }
    snprintf(out, sizeof(out), "$%s*%02X\r\n", body, sum);
    return out;
}

static gnss_nmea_kind_t feed(gnss_nmea_t *n, const char *body)
{
    const char *s = sent(body);
    return gnss_nmea_line(n, s, strlen(s));
}

static void t_checksum(void)
{
    gnss_nmea_t n;
    gnss_nmea_init(&n);
    // A real receiver's line (u-blox's manual example) with its own checksum
    const char *rmc = "$GPRMC,083559.00,A,4717.11437,N,00833.91522,E,0.004,77.52,091202,,,A*57\r\n";
    CHECK(gnss_nmea_line(&n, rmc, strlen(rmc)) == GNSS_NMEA_RMC, "the manual's RMC");
    CHECK(n.valid && n.pos_ok && NEAR(n.lat, 47 + 17.11437 / 60, 1e-9) && NEAR(n.lon, 8 + 33.91522 / 60, 1e-9),
          "its position %f %f", n.lat, n.lon);
    CHECK(n.year == 2002 && n.month == 12 && n.day == 9 && n.hour == 8 && n.min == 35 && NEAR(n.sec, 59, 1e-9),
          "its date and time");
    const char *bad = "$GPRMC,083559.00,A,4717.11437,N,00833.91522,E,0.004,77.52,091202,,,A*58\r\n";
    CHECK(gnss_nmea_line(&n, bad, strlen(bad)) == GNSS_NMEA_BAD && n.bad == 1, "a wrong checksum");
    CHECK(gnss_nmea_line(&n, "GPRMC,1*00", 10) == GNSS_NMEA_BAD, "no $");
    CHECK(gnss_nmea_line(&n, "$GPRMC,1*0", 10) == GNSS_NMEA_BAD, "a short checksum");
    CHECK(gnss_nmea_line(&n, "$GPRMC,1*G0", 11) == GNSS_NMEA_BAD, "not hex");
    char longl[200];
    memset(longl, 'A', sizeof(longl));
    longl[0] = '$';
    CHECK(gnss_nmea_line(&n, longl, sizeof(longl)) == GNSS_NMEA_BAD, "too long");
    CHECK(feed(&n, "GPXX") == GNSS_NMEA_BAD, "an address that isn't 5 characters");
    CHECK(feed(&n, "GPVTG,77.52,T,,M,0.004,N,0.008,K,A") == GNSS_NMEA_OTHER && n.other == 1, "a sentence not used");
    const char *lower = "$GPZDA,083559.00,09,12,2002,00,00*6c";  // lower-case hex is allowed
    gnss_nmea_kind_t k = gnss_nmea_line(&n, lower, strlen(lower));
    CHECK(k == GNSS_NMEA_ZDA || k == GNSS_NMEA_BAD, "lower case hex parses");
}

static void t_fix(void)
{
    gnss_nmea_t n;
    gnss_nmea_init(&n);
    int64_t t;
    CHECK(!gnss_nmea_utc(&n, &t), "nothing yet: no time");
    // An L76K before its fix: time from its own clock, status V, no position
    CHECK(feed(&n, "GNRMC,000012.000,V,,,,,0.00,0.00,060180,,,N") == GNSS_NMEA_RMC, "RMC with no fix");
    CHECK(!n.valid && !n.pos_ok && !gnss_nmea_utc(&n, &t), "no fix: no time given");
    CHECK(feed(&n, "GNGGA,000012.000,,,,,0,00,25.5,,,,,,") == GNSS_NMEA_GGA && n.quality == 0, "GGA with no fix");
    CHECK(isnan(n.lat) && !n.alt_ok, "no position from it");
    // The fix
    CHECK(feed(&n, "GNGGA,153012.000,3957.1234,N,08259.5678,W,1,09,0.98,231.4,M,-33.9,M,,") == GNSS_NMEA_GGA, "GGA");
    CHECK(n.quality == 1 && n.sats_used == 9 && NEAR(n.hdop, 0.98, 1e-9) && n.alt_ok && NEAR(n.alt_m, 231.4, 1e-9) &&
              NEAR(n.geoid_m, -33.9, 1e-9),
          "its fields");
    CHECK(NEAR(n.lat, 39 + 57.1234 / 60, 1e-9) && NEAR(n.lon, -(82 + 59.5678 / 60), 1e-9), "west is negative");
    CHECK(!gnss_nmea_utc(&n, &t), "a fix and a time, but no date yet: no time");
    CHECK(feed(&n, "GNRMC,153012.000,A,3957.1234,N,08259.5678,W,12.5,271.3,241026,,,A") == GNSS_NMEA_RMC, "RMC");
    CHECK(n.valid && NEAR(n.speed_kn, 12.5, 1e-9) && NEAR(n.course, 271.3, 1e-9), "speed and course");
    CHECK(gnss_nmea_utc(&n, &t) && t == 1792855812LL, "2026-10-24T15:30:12Z = %lld", (long long)t);
    CHECK(feed(&n, "GNGSA,A,3,05,13,15,18,23,24,,,,,,,1.71,0.98,1.40,1") == GNSS_NMEA_GSA, "GSA");
    CHECK(n.mode == 3 && NEAR(n.pdop, 1.71, 1e-9) && NEAR(n.hdop, 0.98, 1e-9) && NEAR(n.vdop, 1.40, 1e-9), "DOPs");
    CHECK(feed(&n, "GPGSV,3,1,11,05,45,123,40,13,30,050,38,15,10,300,,18,60,200,42") == GNSS_NMEA_GSV, "GSV");
    CHECK(feed(&n, "GPGSV,3,2,11,23,20,100,35,24,15,080,30,25,05,010,,26,01,020,") == GNSS_NMEA_GSV, "GSV 2");
    CHECK(feed(&n, "BDGSV,1,1,04,01,45,123,40,02,30,050,38,03,10,300,,04,60,200,42") == GNSS_NMEA_GSV, "BDS");
    CHECK(gnss_nmea_in_view(&n) == 15 && n.n_talkers == 2, "in view %d", gnss_nmea_in_view(&n));
    CHECK(feed(&n, "GPGSV,x,1,11") == GNSS_NMEA_GSV, "the count is field 3");
    CHECK(feed(&n, "GPGSV,1,1,zz") == GNSS_NMEA_BAD, "a count that isn't a number");
    // Past the talkers kept: the rest aren't counted, but parse
    feed(&n, "GLGSV,1,1,1");
    feed(&n, "GAGSV,1,1,1");
    feed(&n, "GQGSV,1,1,1");
    feed(&n, "GBGSV,1,1,1");
    CHECK(feed(&n, "GIGSV,1,1,1") == GNSS_NMEA_GSV && n.n_talkers == GNSS_NMEA_TALKERS, "6 talkers kept");
    CHECK(feed(&n, "GPTXT,01,01,02,ANTSTATUS=OK") == GNSS_NMEA_TXT && !strcmp(n.txt, "ANTSTATUS=OK"), "TXT");
    // ZDA gives the date too
    gnss_nmea_init(&n);
    CHECK(feed(&n, "GPZDA,235959.50,31,12,2026,00,00") == GNSS_NMEA_ZDA && n.date_ok && n.year == 2026, "ZDA");
    CHECK(!gnss_nmea_utc(&n, &t), "ZDA alone: no fix known");
    CHECK(feed(&n, "GPGGA,235959.50,3957.1234,N,08259.5678,W,2,12,0.7,231.4,M,-33.9,M,1.0,0001") == GNSS_NMEA_GGA &&
              !gnss_nmea_utc(&n, &t),
          "a GGA fix alone: not enough (no RMC A)");
    CHECK(feed(&n, "GPRMC,235959.50,A,3957.1234,N,08259.5678,W,0,0,311226,,,A") == GNSS_NMEA_RMC &&
              gnss_nmea_utc(&n, &t) && t == 1798761599LL,
          "a whole second, the .5 dropped: %lld", (long long)t);
    CHECK(feed(&n, "GPRMC,235959.50,A,3957.1234,N,08259.5678,W,0,0,150807,,,A") == GNSS_NMEA_RMC &&
              !gnss_nmea_utc(&n, &t),
          "a 2007 date (a week rollover): no time");
    // Bad fields
    CHECK(feed(&n, "GPRMC,256060,A,,,,,,,010126,,,A") == GNSS_NMEA_BAD, "hour 25");
    CHECK(feed(&n, "GPRMC,12345,A,,,,,,,010126,,,A") == GNSS_NMEA_BAD, "a short time");
    CHECK(feed(&n, "GPRMC,120000,A,9157.1,N,00000.0,E,,,010126,,,A") == GNSS_NMEA_RMC && !n.pos_ok, "latitude 91");
    CHECK(feed(&n, "GPRMC,120000,A,3957.1,X,00000.0,E,,,010126,,,A") == GNSS_NMEA_RMC && !n.pos_ok, "hemisphere X");
    CHECK(feed(&n, "GPRMC,120000,A,3960.0,N,00000.0,E,,,010126,,,A") == GNSS_NMEA_RMC && !n.pos_ok, "60 minutes");
    CHECK(feed(&n, "GPRMC,120000,A") == GNSS_NMEA_BAD, "too few fields");
    CHECK(feed(&n, "GPGGA,120000") == GNSS_NMEA_BAD, "GGA too few");
    CHECK(feed(&n, "GPGSA,A,3") == GNSS_NMEA_BAD, "GSA too few");
    CHECK(feed(&n, "GPZDA,120000,32,1,2026") == GNSS_NMEA_ZDA && n.year == 2026, "ZDA day 32 leaves the date");
    CHECK(feed(&n, "GPTXT,1") == GNSS_NMEA_BAD, "TXT too few");
    CHECK(feed(&n, "GPRMC,120060,A,,,,,,,010126,,,A") == GNSS_NMEA_RMC && NEAR(n.sec, 60, 1e-9), "a leap second");
}

static void t_days(void)
{
    CHECK(gnss_days_from_civil(1970, 1, 1) == 0, "the epoch");
    CHECK(gnss_days_from_civil(2000, 3, 1) == 11017, "2000-03-01");
    CHECK(gnss_days_from_civil(2024, 2, 29) == 19782, "a leap day");
    CHECK(gnss_days_from_civil(2100, 3, 1) == 47541, "2100 isn't a leap year");
}

static void t_burst(void)
{
    gnss_burst_t b;
    gnss_burst_init(&b);
    gnss_nmea_t n;
    gnss_nmea_init(&n);
    int64_t start = 0, utc = 0;
    CHECK(!gnss_burst_time(&b, &n, &start, &utc), "nothing read");
    // A burst starting at 1.100 s: 8 bytes at 9600 baud take 8.33 ms, the read ending then
    const int64_t t0 = 1100000;
    gnss_burst_bytes(&b, t0 + 8333, 8, 9600);
    CHECK(b.bursts == 1 && llabs(b.start_us - t0) < 2, "the first byte worked back: %lld", (long long)b.start_us);
    feed(&n, "GNRMC,153012.000,A,3957.1234,N,08259.5678,W,0,0,241026,,,A");
    CHECK(gnss_burst_time(&b, &n, &start, &utc) && start == b.start_us && utc == 1792855812LL, "the second, once");
    CHECK(!gnss_burst_time(&b, &n, &start, &utc), "not twice a burst");
    // More of it, close together
    gnss_burst_bytes(&b, t0 + 200000, 100, 9600);
    gnss_burst_bytes(&b, t0 + 300000, 100, 9600);
    CHECK(b.bursts == 1, "still the first burst");
    // The next second's, after a gap
    gnss_burst_bytes(&b, t0 + 1000000 + 8333, 8, 9600);
    CHECK(b.bursts == 2 && llabs(b.start_us - (t0 + 1000000)) < 2, "a new burst");
    feed(&n, "GNRMC,153013.000,A,3957.1234,N,08259.5678,W,0,0,241026,,,A");
    CHECK(gnss_burst_time(&b, &n, &start, &utc) && utc == 1792855813LL, "its second");
    // A link too slow: the next second's sentence with no gap first
    gnss_burst_bytes(&b, t0 + 1050000, 50, 9600);
    feed(&n, "GNRMC,153014.000,A,3957.1234,N,08259.5678,W,0,0,241026,,,A");
    CHECK(!gnss_burst_time(&b, &n, &start, &utc) && b.no_gap == 1, "counted, not given");
    gnss_burst_bytes(&b, 5000000, 0, 9600);
    gnss_burst_bytes(&b, 5000000, 5, 0);
    CHECK(b.bursts == 2, "nothing read, or no baud: ignored");
}

static void t_json(void)
{
    gnss_status_t s;
    memset(&s, 0, sizeof(s));
    gnss_nmea_init(&s.nmea);
    s.running = true;
    s.rx_gpio = 39;
    s.tx_gpio = 38;
    s.baud = 9600;
    s.last_s = -1;
    char buf[1024];
    size_t n = gnss_json(&s, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "\"fix\":\"none\"") && strstr(buf, "\"lat\":null") && strstr(buf, "\"utc_s\":null"),
          "no fix: %s", buf);
    feed(&s.nmea, "GNGGA,153012.000,3957.1234,N,08259.5678,W,1,09,0.98,231.4,M,-33.9,M,,");
    feed(&s.nmea, "GNRMC,153012.000,A,3957.1234,N,08259.5678,W,10.0,271.3,241026,,,A");
    feed(&s.nmea, "GNGSA,A,3,05,13,15,18,23,24,,,,,,,1.71,0.98,1.40,1");
    feed(&s.nmea, "GPGSV,1,1,07,05,45,123,40");
    feed(&s.nmea, "GPTXT,01,01,02,say \"hi\"\\");
    n = gnss_json(&s, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "\"fix\":\"3d\"") && strstr(buf, "\"lat\":39.9520567") && strstr(buf, "\"lon\":-82.9927967") &&
              strstr(buf, "\"speed_kmh\":18.5") && strstr(buf, "\"utc\":\"2026-10-24T15:30:12Z\"") &&
              strstr(buf, "\"constellations\":{\"GP\":7}") && strstr(buf, "\"txt\":\"say \\\"hi\\\"\\\\\""),
          "a fix: %s", buf);
    CHECK(buf[0] == '{' && buf[n - 1] == '}' && strlen(buf) == n, "one object");
    CHECK(gnss_json(&s, buf, 100) == 0, "too small: 0");
    // The short form inside another message
    char m[300] = "{\"state\":\"online\"}";
    size_t ml = gnss_json_append(&s, m, strlen(m), sizeof(m));
    CHECK(s.found == false && ml == strlen(m) && !strcmp(m, "{\"state\":\"online\"}"), "not found: nothing added");
    s.found = true;
    ml = gnss_json_append(&s, m, strlen(m), sizeof(m));
    CHECK(ml == strlen(m) && !strcmp(m, "{\"state\":\"online\",\"gnss\":{\"fix\":\"3d\",\"sats\":9,\"lat\":39.9520567,"
                                         "\"lon\":-82.9927967,\"alt_m\":231.4,\"hdop\":0.98}}"),
          "appended: %s", m);
    char e[200] = "{}";
    CHECK(gnss_json_append(&s, e, 2, sizeof(e)) > 2 && !strncmp(e, "{\"gnss\":{", 9), "into an empty object: %s", e);
    char small[40] = "{\"a\":1}";
    CHECK(gnss_json_append(&s, small, strlen(small), sizeof(small)) == strlen("{\"a\":1}") && !strcmp(small, "{\"a\":1}"),
          "no room: unchanged");
    CHECK(gnss_json_append(&s, small, 0, sizeof(small)) == 0, "empty: unchanged");
    CHECK(gnss_json(&s, buf, 0) == 0, "no room");
}

int main(void)
{
    t_checksum();
    t_fix();
    t_days();
    t_burst();
    t_json();
    printf("gnss: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
