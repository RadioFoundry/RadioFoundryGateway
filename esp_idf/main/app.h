// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * RadioFoundry Gateway on an ESP32-S3: the board's parts and what its two
 * roles share. The settings are one box config; the radios it names run here:
 * the roamer or repeater (roamer.c and the files it starts, CONFIG_RDOF_ROAMER),
 * the SDR (sdr.c and the files it starts, CONFIG_RDOF_SDR), or one of each.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netclock_tempcal.h"
#include "rdof/benchguard.h"
#include "rdof/config.h"
#include "rdof/json.h"
#include "sdkconfig.h"

#if CONFIG_RDOF_ROAMER
#include "rdof/hblink.h"
#include "rdof/modem.h"
#include "rdof/netcalls.h"
#include "rdof/repeater.h"
#include "rdof/txvr.h"
#endif
#if CONFIG_RDOF_SDR
#include "rdof/sdr/config.h"
#include "rdof/sdr/frontend.h"
#include "rdof/sdr/receiver.h"
#include "rdof/tracker.h"
#endif

/* ── settings: one box config JSON in NVS (settings.c); Wi-Fi and NTP in their components ── */

extern rdof_roles_t g_roles;     /* which radios run here */
extern bool g_cfg_ok;          /* false: the stored settings didn't parse (g_cfg_error says why); defaults run */
extern char g_cfg_error[160];
#if CONFIG_RDOF_ROAMER
extern rdof_config_t *g_rcfg[RDOF_MAX_RADIOS]; /* each roamer or repeater g_roles names (PSRAM), g_roles.nradios of them */
#endif
#if CONFIG_RDOF_SDR
extern rdof_sdr_config_t g_scfg; /* the SDR, when g_roles.has_sdr */
#endif

void settings_load(void);
/* The stored JSON (malloc'd; free it). */
char *settings_text(void);
/* The stored JSON with "wifi" (no passwords) and "ntp" added: what the page edits and exports (malloc'd). */
char *settings_export(void);
/* Validate, hand "wifi" and "ntp" to their components, then store the rest (json is edited in place).
   false with error set if it doesn't parse or asks for a role this build doesn't have. */
bool settings_save(char *json, size_t len, char *error, size_t cap);
/* A one-shot request kept across a restart (the self-test and spike buttons). */
void settings_set_flag(const char *key);
bool settings_take_flag(const char *key);

/* ── the board ────────────────────────────────────────────────────────────── */

/* Internal RAM an esp-mqtt client takes: its task's stack (6 KB), its in and out buffers (1 KB each), its state. */
#define MQTT_CLIENT_INTERNAL (6144 + 2 * 1024 + 1536)
/* The memory plan's claims of the roamer (roamer.c; true if a radio is on USB) and the IP gateway (ipgw_task.c). */
bool roamer_mem_claims(void);
void ipgw_mem_claims(void);

typedef struct {
    bool wifi_up, ap_mode;
    char ip[16];
    bool usb_either; /* both roles want the USB port: whichever device is plugged in decides */
} board_status_t;

extern board_status_t g_board;

/* The OLED (oled_task.c): the SDR's screen when the board runs one, else the first roamer's or repeater's. */
void oled_start(void);
void logring_init(void);
size_t logring_read(char *out, size_t cap);
void wifi_start(void);
void web_start(void);
/* The core dump in flash (crash.c): read and logged once at boot; the "crash" object in /api/status (nothing when
 * there's no dump); /api/coredump to download or erase it, and /api/crash with CONFIG_RDOF_CRASH_TEST. */
void crash_init(void);
void crash_status_json(rdof_json_t *j);
void crash_register(void *server); /* an httpd_handle_t */

/* UTC seconds: from netclock (NTP, or a browser's clock until then). */
double now_unix(void);

/* One MQTT connection (net.c); each role has its own, with its own status topic as the last will. */
typedef struct {
    const char *host, *user, *pass; /* "" for none */
    int port;
    bool tls;
    const char *client_id, *will_topic, *will_msg; /* the will: {"state": "offline"} */
    int outbox_kb;
    int queue_kb; /* net_mqtt_post's queue (PSRAM) */
    void (*on_connected)(void *ctx);
    void *ctx;
    volatile bool *up;
    /* MQTT 5 (CONFIG_MQTT_PROTOCOL_5) topic aliases: these topics, in this order (each channel's raw, then its json),
     * get aliases 1, 2, ... up to the broker's Topic Alias Maximum at each connect. Only QoS 0 posts use them. */
    const char *const *alias_topics;
    int n_alias_topics;
    struct esp_mqtt_client *client; /* net.c's */
    void *queue;
    volatile uint32_t dropped; /* posts dropped: the queue full, QoS 0 while not connected, or refused (net.c) */
    volatile uint32_t gen;     /* connections made (net.c): an alias is only good on the one it was sent on */
    volatile bool v5;          /* this connection is MQTT 5 (else 3.1.1: the broker didn't take 5) */
    volatile bool fall_back;   /* refused MQTT 5: the post task reconnects on 3.1.1 */
} net_mqtt_t;
struct esp_mqtt_client;
struct esp_mqtt_client *net_mqtt_start(net_mqtt_t *m);
/* Publish without waiting: esp-mqtt holds its lock through a whole connect attempt (up to its 10 s network
 * timeout), so a publish from the DSP or the radio's loop would stall there while the broker is unreachable.
 * The message goes on a queue a task of its own hands to esp-mqtt. QoS 0 while not connected is dropped, as
 * paho does; so is anything when the queue is full. Returns whether it was queued. */
bool net_mqtt_post(net_mqtt_t *m, const char *topic, const void *data, int len, int qos, bool retain); /* len < 0: a string */
/* A UDP socket connected to host:port, or -1. */
int net_udp_connect(const char *host, int port);

/* The USB host (usb_host.c): installed once, whichever roles use it. */
void usb_host_start(void);
void usb_host_cdc_install(void); /* the CDC-ACM host driver, once */
/* Both roles on USB without a hub (no "usb_hub" in the settings): run the role of the first device that's
   plugged in (a radio or an RTL-SDR); the other kind later restarts the board. */
void usb_host_either(void (*radio)(void), void (*rtl)(void));
bool usb_host_is_rtl(uint16_t vid, uint16_t pid);
/* Each radio on USB asks for its device: the one usb_host.c assigns it (by VID / PID, and usb.serial or usb.port
   to tell two of a kind apart; rdof/usbmatch.h). Blocks until one is there; returns its address, or 0 (any: the
   first one there, as with one radio) when there is nothing to tell apart. */
uint8_t usb_host_wait_device(int radio);
/* The radio's device went away: the next one that matches can be assigned to it. */
void usb_host_release(int radio);

/* ── the roamer or repeater (roamer.c) ────────────────────────────────────── */

#if CONFIG_RDOF_ROAMER

typedef struct {
    bool modem_open, radio_ready;
    char modem_desc[128];
    int modem_protocol;
    const char *radio_kind;
    char radio_error[192];
    bool hblink_up;
    uint32_t hblink_tx, hblink_rx;
    volatile bool mqtt_up;
    uint32_t bursts_heard;
    char heard[48];            /* the call being heard, for the OLED */
    bool hearing;
    char sending[48];
    bool on_air;
    bool started;
} roam_status_t;

/* The modem's byte transport (USB or UART). */
typedef struct {
    const char *name;
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    void *ctx;
} modem_transport_t;

struct roam_rx;
struct roam_hb;
struct roam_mq;

/* One roamer or repeater: everything that was the board's one radio. The first is allocated in internal RAM when
   there's room and its tasks' stacks are internal, as before; a second one's are in PSRAM (docs/reference.md, "More
   than one radio"). */
typedef struct roam {
    int i;                      /* 0: the first "run" names */
    const rdof_config_t *cfg;
    roam_status_t st;
    rdof_modem_t modem;
    rdof_radio_t radio;
#if RDOF_FEATURE_TX
    rdof_txvr_t *txvr;
#endif
#if RDOF_FEATURE_REPEATER
    rdof_repeater_t *rpt;
#endif
    void *lock;                 /* the transceiver's or repeater's (a FreeRTOS mutex) */
    /* the port under rdof_modem (modem_port.c) */
    void *rx_bytes, *reply_sem; /* a stream buffer, a semaphore */
    const modem_transport_t *volatile transport;
    modem_transport_t uart, usb, sdr; /* this radio's transports */
    struct roam_rx *rx;
    struct roam_hb *hb;
    struct roam_mq *mq;
    bool psram_stacks;          /* its tasks' stacks are in PSRAM */
} roam_t;

extern roam_t *g_roam[RDOF_MAX_RADIOS];
extern int g_nroam;

/* Start every roamer or repeater g_roles names. */
void roamers_start(void);
/* For the page: a "roamer" object for the first and a "roamers" list of them all; the settings as resolved. */
void roamer_status_json(rdof_json_t *j);
size_t roamer_resolved(char *out, size_t cap);
/* Any roamer or repeater on USB. */
bool roamer_uses_usb(void);
/* A task for radio r: stack internal for the first, PSRAM for the others. */
bool roam_task(roam_t *r, void (*fn)(void *), const char *name, uint32_t stack, int prio);

void modem_port_init(roam_t *r);
void modem_transport_up(roam_t *r, const modem_transport_t *t);
void modem_transport_down(roam_t *r);
void modem_on_bytes(roam_t *r, const uint8_t *data, size_t len);
bool modem_transport_ready(roam_t *r);

void usb_modem_start(roam_t *r);
void uart_modem_start(roam_t *r);
/* radio type split (split_port.c): the two halves as one modem. The hooks take a half's bytes and transport from
 * modem_port.c (false: r isn't a half). */
void split_modem_start(roam_t *r);
bool split_half_bytes(roam_t *r, const uint8_t *data, size_t len);
bool split_half_up(roam_t *r, const modem_transport_t *t);
bool split_half_down(roam_t *r);
void split_status_json(roam_t *r, rdof_json_t *j);
/* An sdr_transceiver's modem: the SDR's (modem_sdr.c), once the SDR is receiving. */
void sdr_modem_start(roam_t *r);

/* The roamer or repeater on the modem (radio_task.c). */
void radio_start(roam_t *r);
/* For DMRC: where the roamer is (0, 0 when untuned). */
void radio_channel_now(roam_t *r, uint32_t *freq, int *cc);

void roam_hb_start(roam_t *r);
void roam_hb_send(roam_t *r, const uint8_t *data, size_t len);
void roam_hb_send_config(roam_t *r, bool force);
#if CONFIG_RDOF_HBLINK_SCTP
/* The SCTP association's status as JSON (0 when HBlink4 isn't over SCTP). */
size_t roam_hb_sctp_status(roam_t *r, char *buf, size_t cap);
#endif

void roam_rx_start(roam_t *r);
/* What the radio hears, to the tracker (any task). */
void rx_hear(roam_t *r, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq);

void roam_mqtt_start(roam_t *r);
void roam_publish(roam_t *r, const char *kind, const char *json, int qos); /* {root}/{name}/{app}/{kind} */
void roam_publish_raw(roam_t *r, const rdof_rawburst_t *b); /* .../raw, binary (QoS 0) */
void roam_publish_status(roam_t *r);
void roam_tx_event(roam_t *r, const char *action, const rdof_tx_call_info_t *call, double duration);
void roam_tx_burst(roam_t *r, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b);

double now_mono(void);

#endif

/* The IP gateway (ipgw_task.c, CONFIG_RDOF_IPGW; docs/spec/fw/ipgw.md): DMR packet data to and from the IP network.
 * ipgw_start before the roles start (it reads "ip_gateway"); then each receiver's tracker takes ipgw_sink (false:
 * the gateway is off). source: an SDR channel's index, or IPGW_SOURCE_RADIO + a radio's; freq_now: a tracker's
 * frequency that follows its radio (NULL: freq). */
#if CONFIG_RDOF_IPGW
#include "rdof/tracker.h"
#define IPGW_SOURCE_RADIO 100
void ipgw_start(void);
bool ipgw_sink(int source, uint32_t freq, int cc, const double *freq_now, rdof_sink_t *out);
void ipgw_status_json(rdof_json_t *j); /* "ip_gateway": {...}, nothing when off */
#endif

/* The status LED (status_led.c): red without Wi-Fi, orange while a configured device (the SDR, a radio's modem)
   isn't working, else green. */
void status_led_start(void);

/* ── the SDR (sdr.c) ──────────────────────────────────────────────────────── */

#if CONFIG_RDOF_SDR

/* Stages of the DSP's time: level and AGC, float conversion and the spectrum; the channelizer; the demods
 * (with the second core's wait); the call trackers. */
enum { SDR_STAGE_INPUT, SDR_STAGE_CHANNELIZE, SDR_STAGE_DEMOD, SDR_STAGE_TRACK, SDR_STAGES };

typedef struct {
    bool rtl_open;              /* the front end is there and open (the names are the RTL-SDR's, from before others) */
    char rtl_desc[96];
    char rtl_error[96];
    bool receiving;
    int stream_restarts;        /* the stream stalled and was restarted */
    double center_hz;
    int channels_in;            /* channels in the tuner window */
    bool in_window[RDOF_SDR_MAX_CHANNELS];
    double cpu_load;            /* DSP time / wall time, last status period */
    int dsp_split;              /* dsp_cores 2: channels demodded on core 1 (the rest on core 0); -1 one core */
    double stage_load[4];       /* the same by stage (SDR_STAGE_*) */
    double demod_load[4];       /* the demods by step: FM (discriminator, matched filter), sync search, flywheel, history move */
    double gain_db;             /* NAN: tuner AGC */
    volatile bool mqtt_up;
    int hb_peers, hb_up;
    char selftest[200];         /* the last self-test's verdict */
    char spike[384];            /* the last spike's results */
    bool started;
} sdr_status_t;

extern sdr_status_t g_sstat;

void sdr_role_start(void);
void sdr_status_json(rdof_json_t *j);
size_t sdr_resolved(char *out, size_t cap);
/* The self-test, before anything else loads the CPU (any settings; no dongle needed). */
void sdr_boot_selftest(void);

/* The receiver and the lock the DSP task holds while it works on it (status readers take it briefly). */
extern rdof_sdr_rx_t g_rx;
extern bool g_rx_ready;
void rx_lock(void);
void rx_unlock(void);

void sdr_mqtt_start(void);
/* The tracker sink that publishes to {root}/{channel}/{app}/json|raw. */
rdof_sink_t sdr_mqtt_sink(void);
void sdr_publish_receivers(void);
void sdr_publish_status(void);
uint32_t sdr_mqtt_dropped(void); /* calls and bursts not published: see net_mqtt_post */

/* UTC slot timing (slot_timing.c, docs/spec/slot-timing.md): the stream's sample clock against netclock's UTC, the
 * gate, and the SDR modem's grid from them. */
struct rdof_sdrmodem;
struct rdof_sdr_rx;
void slot_timing_setup(const rdof_slot_timing_t *t);
void slot_timing_stream(double rate, int64_t base48);         /* a stream starts: its sample 0 is the receiver's base48 */
void slot_timing_rx(struct rdof_sdr_rx *rx);                    /* the map for the receiver's UTC slots, before a chunk */
void slot_timing_arrived(int64_t end_index, int64_t esp_us);  /* the stream's samples up to end_index arrived */
void slot_timing_apply(struct rdof_sdrmodem *m);                /* the grid, its map and the gate, before a pull */
void slot_timing_json(rdof_json_t *j);                          /* "slot_timing": {...} */

void sdr_hb_start(void);
/* The sink for channel c's HBlink4 peer, or false if it has none. */
bool sdr_hb_sink(int c, rdof_sink_t *out);
int sdr_hb_status(const char **ids, bool *up, int max);

/* The DSP's allocators (sdr.c): the hot arena, a window the DSP task opens while it builds a receiver, and the
 * internal allocations esp-dsp's plans and filters make (dsp_esp.c). */
void dsp_setup(void);
void sdr_hot_arena(void *base, size_t cap);
void sdr_hot_arena_open(void);
void sdr_hot_arena_close(void);
void sdr_hot_arena_use(size_t *used, size_t *peak, size_t *cap);
void sdr_hot_count(bool on);
size_t sdr_hot_counted(void);
void *sdr_hot_aligned(size_t align, size_t n, bool zero);
void sdr_hot_free(void *p);

/* The DSP (sdr_task.c). */
void sdr_start(void);
/* The memory plan's claims (mem.c): the DSP's, the network side's and the front end's. True if it's on USB. */
bool sdr_mem_claims(void);
void sdr_net_mem_claims(void);
void fe_lr2021_mem_claims(void);
void fe_rtlsdr_mem_claims(void);
/* Stop the front end and idle the DSP (an OTA: the SPI or USB, the DSP's core and its interrupts out of the way), or
 * start again. True once the stream has stopped (2 s at most). */
bool sdr_pause(bool on);
/* One chunk through the receiver, on one core or split over two (the self-test uses it too). */
void sdr_process_chunk(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, int split);
/* split for sdr_process_chunk: channels [0, split) on this core, the rest on the helper (0 < split < C), 0 for one
 * core, or SDR_SPLIT_AUTO: balanced by what each core took (dsp_cores 2). */
#define SDR_SPLIT_AUTO (-1)
/* The DSP task's and the helper's stack high-water marks (bytes never used; -1 if not running). */
void sdr_dsp_stacks(int *dsp, int *helper);
/* Level and gain for the status message. */
void sdr_level(bool *has_level, double *clip_pct, double *peak_dbfs, bool *has_gain, double *gain_db, bool *has_agc,
               double *ceiling, double *min_db, int *changes);
void sdr_counters(long *dropped, long *overflows, double *cpu_load);
/* The front end's counters since its stream started (false, all zero, before the role starts), and what it can
 * do (NULL before). With "sim", bytes are what the clip stood in for and overflows the chunks skipped late. */
bool sdr_fe_stats(rdof_fe_stats_t *out);
const rdof_fe_caps_t *sdr_fe_caps(void);
/* For the SDR's own modem (modem_sdr.c): the front end, its receive rate and center once it's receiving (false
 * before), and the receiver channel for a frequency (-1: none). */
bool sdr_tx_frontend(rdof_fe_t *fe, double *rate, double *center);
int sdr_rx_channel(uint32_t hz);
/* PUT /sdr (sdr_task.c): a retune at run time. center_hz 0: the settings' plan, NAN: as it is; gain_db NAN: as it
 * is, SDR_RETUNE_SETTINGS: the settings'; taps (receiver channels at any frequency in the window, ids T1..) ntaps -1:
 * as they are. NULL when applied (wait) or asked, else why not. Settings stay as they are. */
#define SDR_MAX_TAPS 4
#define SDR_RETUNE_SETTINGS (-1e9)
typedef struct {
    double center_hz, gain_db, taps_hz[SDR_MAX_TAPS];
    int ntaps;
} sdr_retune_t;
const char *sdr_retune(const sdr_retune_t *r, bool wait);
void sdr_retune_restore(void); /* back to the settings, without waiting (a lease ended) */
void sdr_retune_get(double *center_hz, double *gain_db, double *taps_hz, int *n); /* NAN: the settings' */
/* Receiver channel k: a configured one in the window, or a tap (NULL: none). sdr_is_tap: a tap's id. */
const rdof_sdr_cfg_channel_t *sdr_rx_cfg_channel(int k);
bool sdr_is_tap(const char *id);
/* The receiver's hooks into that modem (modem_sdr.c; nothing without an sdr_transceiver). */
void sdr_tx_heard(void *ctx, int channel, const rdof_sdr_burst_t *b, double t);
/* The bench API (bench_api.c, docs/spec/bench-api.md; CONFIG_RDOF_BENCH): its handlers on the web server, the
 * receiver's burst hook (which calls sdr_tx_heard first), and a new run of the sample clock at each receiver start. */
#if CONFIG_RDOF_BENCH
int bench_api_register(void *server); /* an httpd_handle_t */
void bench_on_burst(void *ctx, int channel, const rdof_sdr_burst_t *b, double t);
void bench_rx_started(void);
bool bench_uri_match(const char *tmpl, const char *uri, size_t len);
void bench_on_chunk(const rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n); /* after each chunk, from the DSP task */
bool bench_call_sink(int channel, rdof_sink_t *s); /* the calls stream's tracker sink for a receiver channel */
#else
#define bench_api_register(server) 0
#define bench_on_burst sdr_tx_heard
#define bench_rx_started() ((void)0)
#define bench_uri_match httpd_uri_match_wildcard
#define bench_on_chunk(rx, iq, n) ((void)0)
#define bench_call_sink(channel, s) false
#endif
/* bench_radio.c: the radios wired to the bench; their JSON (NULL: all, else just that name); the session's
 * declared TX frequencies for a radio link (-1: no such session) */
int bench_radio_register(void *server);
int bench_radio_count(void);
size_t bench_radios_json(char *buf, size_t cap, const char *only);
int bench_session_tx(const char *id, uint32_t *tx, int max, bool renew);
/* A transmit job holding an MMDVM bench radio (bench_tx.c): NULL, or why it can't. */
const char *bench_radio_tx_why(const char *name);
const char *bench_radio_tx_open(const char *name, const char *session, uint32_t freq,
                                void (*cb)(void *ctx, const uint8_t *b, size_t n), void *ctx);
void bench_radio_tx_write(const char *name, const uint8_t *b, size_t n);
void bench_radio_tx_close(const char *name);
size_t bench_radio_tx_vias(char *buf, size_t cap);
/* POST /radios/{name}/tune (a RadioFoundry radio over its debug link; odr: the request's proto=odr-debug for a radio set up
 * as mmdvm): NULL when tuned (rx_hz and power as the radio says after it), else why not, *http its status. Waits
 * for the radio (about half a second). A session's lease covering a frequency: bench_session_covers. */
const char *bench_radio_tune(const char *name, const char *session, const rdof_guard_tune_t *tune, bool odr, int *http,
                             uint32_t *rx_hz, int *power);
bool bench_session_covers(const char *id, double freq_hz);
/* bench_tx.c: WS /tx; info's and status's tx objects; a session's label (bench_api.c) for the keying log */
int bench_tx_register(void *server);
size_t bench_tx_info_json(char *buf, size_t cap);
size_t bench_tx_status_json(char *buf, size_t cap);
bool bench_tx_enabled(void);
bool bench_session_label(const char *id, char *label, size_t cap);
bool bench_query(void *req, const char *key, char *val, size_t cap); /* percent-decoded; false: absent */
/* modem_sdr.c: the SDR's modem for a bench transmit job (via=modem): NULL, or why it can't */
const char *sdr_modem_bench_why(void);
const char *sdr_modem_bench_open(void (*reply)(void *ctx, const uint8_t *frame, size_t len), void *ctx,
                                 double power_dbm);
void sdr_modem_bench_write(const uint8_t *data, size_t len);
void sdr_modem_bench_close(void);
bool sdr_tx_muted(void *ctx, int64_t position);
/* The tuner's crystal error, as learned from its sample clock or the AFC (sdr_task.c). NAN where there's nothing. */
typedef struct {
    bool on;
    double chip;      /* whole ppm the RTL is set to */
    double learned;   /* the tuner's error */
    bool measured;    /* learned from the sample clock (else the AFC) at the last look */
    int channels;     /* AFC: channels agreeing at the last look */
    double steady_s;  /* how long the estimate has held */
    double saved;     /* the value kept in NVS */
    double sim_ppm;   /* sim: the simulated tuner's error */
    /* the sample clock timed against the board's (rdof/sdr/rate.h) */
    bool clock_on;          /* this front end's sample clock says what its crystal does */
    double clock_ppm;       /* the crystal's error from it */
    double clock_err;       /* its standard error, the board's clock's included */
    bool clock_trusted;     /* good enough to be the learned value */
    double clock_raw;       /* the samples' rate against the local clock (ppm) */
    double clock_span_s, clock_rms_us;
    uint32_t clock_steps;   /* fits started over (samples lost) */
    double board_ppm;       /* the board's own crystal, from netclock (NAN: not known yet) */
    bool board_learned;     /* fitted this run (else stored) */
    bool settled;           /* the die temperature and the sample rate have held: what's learned is saved */
    double die_c;           /* the die temperature (C), NAN: no sensor */
    double die_spread;      /* its spread over the settle window, NAN until the window is covered */
    double rate_spread;     /* the recent sample rate's (ppm), likewise */
    double die_tol, rate_tol; /* the most each may move over the window (C, ppm) */
    nc_tcal_t cal;          /* the dongle's error against the die's temperature, learned when settled */
    double cal_ppm;         /* what it expects at the die's temperature now, NAN: empty */
} sdr_ppm_status_t;
void sdr_ppm_status(sdr_ppm_status_t *out);
/* The display spectrum: rows newer than since (at most max, the newest ones), SPEC bins each, into out; the
 * newest row's number in seq; each row's last wideband sample in sc (NULL: not wanted). Asking keeps it running
 * for 10 s. */
typedef struct {
    int bins, row_ms;
    double rate, center_hz;
} sdr_spectrum_info_t;
int sdr_spectrum_rows(uint32_t since, uint8_t *out, int max, uint32_t *seq, sdr_spectrum_info_t *info, long long *sc);

/* Self-test (selftest.c) and the USB spike. */
void selftest_run(void);
void kbench_run(void); /* the kernel shootout, from the self-test */
void spike_run(void);
/* esp-dsp backends (dsp_esp.c), checked against the built-in code before they're used. */
void dsp_esp_install(void);
void dsp_esp_fft_enable(bool on);
extern bool g_esp_fft, g_esp_fir;

#endif
