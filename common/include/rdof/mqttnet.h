// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MQTT network (docs/spec/fw/mqtt-net.md): calls between boxes and apps as decoded content over one broker.
 *
 *   The call message (section 4): a 14-byte header, then items [type][length][value]. rdof_mn_begin / rdof_mn_item
 *   build one; rdof_mn_parse / rdof_mn_next read one.
 *   Topics (section 3): {root}/tg/{tg}/{node}, {root}/pc/{dst}/{node}, {root}/dl/{node}, {root}/node/{node}/{leaf}.
 *   The uplink (rdof_mn_up_t): what a receiver hears (the tracker's frames, rdof/tracker.h) as call messages, one
 *   stream per call, and the activity events (section 7) as JSON.
 *   The downlink (rdof_mn_down_t): call messages from the broker as network bursts (rdof_net_burst_t), the FEC,
 *   sync and slot type rebuilt, for the router (rdof/siteroute.h) and the transmitters.
 *   The node's info JSON (section 6) and the talkgroups it subscribes to (section 8.3).
 *
 * Portable C, no allocation, no locking: call each object from one thread. The uplink and downlink are a few KB
 * each (a data call's buffer): on a board, put them in PSRAM.
 */
#ifndef RDOF_MQTTNET_H
#define RDOF_MQTTNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/site.h"
#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── the call message ─────────────────────────────────────────────────────── */

#define RDOF_MN_VERSION 3
#define RDOF_MN_HEADER 14
#define RDOF_MN_PATH_NONE 0xFF
#define RDOF_MN_NODE_MAX 39       /* a node name's characters */
#define RDOF_MN_DATA_MAX 3048     /* a data call's user data: 127 rate 1 blocks */
#define RDOF_MN_DATA_HEADERS 2    /* the data header and a proprietary second header */
#define RDOF_MN_MSG_MAX 3200      /* the largest message built here (a whole data call) */
#define RDOF_MN_DGRAM_MAX 1200    /* a message on a datagram transport (section 5): data calls go in parts */

/* Header flags. */
#define RDOF_MN_F_SLOT2 0x01
#define RDOF_MN_F_GROUP 0x02
#define RDOF_MN_F_EMERGENCY 0x04
#define RDOF_MN_F_PRIORITY 0x08
#define RDOF_MN_F_PRIVACY 0x10
#define RDOF_MN_F_LATE 0x20

/* Item types. */
enum {
    RDOF_MN_VOICE = 0x01,
    RDOF_MN_LC_HEADER = 0x02,
    RDOF_MN_TERMINATOR = 0x03,
    RDOF_MN_EMBEDDED_LC = 0x04,
    RDOF_MN_PI_HEADER = 0x05,
    RDOF_MN_CSBK = 0x06,
    RDOF_MN_DATA_PDU = 0x07,
    RDOF_MN_TALKER_ALIAS = 0x08,
    RDOF_MN_GPS = 0x09,
    RDOF_MN_QUALITY = 0x0A,
    RDOF_MN_ORIGIN = 0x0B,
    RDOF_MN_SETUP = 0x0C,
    RDOF_MN_RAW = 0x7F,
};

/* data_pdu rates */
enum { RDOF_MN_RATE_12 = 0, RDOF_MN_RATE_34 = 1, RDOF_MN_RATE_1 = 2 };

typedef struct {
    uint8_t flags, path, seq;
    uint32_t stream, src, dst;
} rdof_mn_hdr_t;

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    int items;     /* items written: the next message's seq is this one's plus items */
    bool overflow; /* an item didn't fit: rdof_mn_end returns 0 */
} rdof_mn_writer_t;

/* The header into buf (cap at least RDOF_MN_HEADER). */
void rdof_mn_begin(rdof_mn_writer_t *w, uint8_t *buf, size_t cap, const rdof_mn_hdr_t *h);
/* One item (len up to 65535; 255 and more take the u16 length). False: no room (the writer is then spoiled). */
bool rdof_mn_item(rdof_mn_writer_t *w, int type, const void *value, size_t len);
bool rdof_mn_voice(rdof_mn_writer_t *w, int index, const uint8_t ambe27[27]);
/* lc_header, terminator or embedded_lc: the 9-byte LC */
bool rdof_mn_lc(rdof_mn_writer_t *w, int type, const uint8_t lc9[9]);
bool rdof_mn_csbk(rdof_mn_writer_t *w, const uint8_t csbk10[10], int count);
/* bits corrected (saturating at 255), RSSI in dBm (0: not measured; stored as -dBm, 0..255) */
bool rdof_mn_quality(rdof_mn_writer_t *w, int bits, int rssi_dbm);
/* part 0..127, more: parts follow; rate RDOF_MN_RATE_*; headers 10 octets each (CRC left off) */
bool rdof_mn_data_pdu(rdof_mn_writer_t *w, int part, bool more, int rate, int nheaders, const uint8_t *headers10,
                      const uint8_t *data, size_t len);
/* The message's length; 0 when an item didn't fit. */
size_t rdof_mn_end(rdof_mn_writer_t *w);

/* A message's header. False: shorter than a header, or not version 3. */
bool rdof_mn_parse(const uint8_t *msg, size_t len, rdof_mn_hdr_t *h);

typedef struct {
    int type;
    const uint8_t *value;
    size_t len;
} rdof_mn_item_t;

typedef struct {
    const uint8_t *msg;
    size_t len, at;
    int count; /* items read */
    bool bad;  /* a length ran past the end: the rest is dropped */
} rdof_mn_reader_t;

void rdof_mn_reader(rdof_mn_reader_t *r, const uint8_t *msg, size_t len);
/* The next item, any type (the caller skips what it doesn't know). False at the end, or bad. */
bool rdof_mn_next(rdof_mn_reader_t *r, rdof_mn_item_t *it);
/* An item's value length is right for its type (unknown types: any). */
bool rdof_mn_item_ok(const rdof_mn_item_t *it);
/* How many items the message holds (-1: malformed). */
int rdof_mn_count(const uint8_t *msg, size_t len);
/* A data_pdu value's fields. False: malformed. */
typedef struct {
    int part;
    bool more;
    int rate, nheaders;
    const uint8_t *headers; /* 10 octets each */
    const uint8_t *data;
    size_t len;
} rdof_mn_pdu_t;
bool rdof_mn_pdu(const rdof_mn_item_t *it, rdof_mn_pdu_t *out);

/* ── call setup (section 9.4): the setup item ─────────────────────────────── */

/* setup ops */
enum {
    RDOF_MN_SETUP_REQUEST = 1,
    RDOF_MN_SETUP_ACCEPT = 2,
    RDOF_MN_SETUP_REJECT = 3,
    RDOF_MN_SETUP_ALERTING = 4,
    RDOF_MN_SETUP_CANCEL = 5,
    RDOF_MN_SETUP_ANSWERED = 6,
};
/* setup reasons */
enum {
    RDOF_MN_WHY_OK = 0,
    RDOF_MN_WHY_NOT_FOUND = 1,
    RDOF_MN_WHY_BUSY = 2,
    RDOF_MN_WHY_NO_ANSWER = 3,
    RDOF_MN_WHY_REFUSED = 4,
    RDOF_MN_WHY_NOT_SUPPORTED = 5,
    RDOF_MN_WHY_CONGESTION = 6,
    RDOF_MN_WHY_TIMEOUT = 7,
};
#define RDOF_MN_SETUP_FOACSU 0x01      /* flags: full off-air call setup (the called radio is alerted first) */
#define RDOF_MN_SETUP_UNCONFIRMED 0x02 /* flags: the answerer couldn't ask the radio (a Tier II box) */
#define RDOF_MN_SETUP_LEN 8

typedef struct {
    uint8_t op, reason, flags;
    uint8_t options; /* the service options, as C_RAND's: bit 7 emergency, bit 6 privacy, bits 1-0 priority */
    uint32_t ref;    /* the request's stream ID that this message answers; 0 in a request */
} rdof_mn_setup_t;

/* False: no room, or op not 1-6. */
bool rdof_mn_setup(rdof_mn_writer_t *w, const rdof_mn_setup_t *s);
/* A setup item's fields. False: not one, the wrong length, or op not 1-6. */
bool rdof_mn_setup_of(const rdof_mn_item_t *it, rdof_mn_setup_t *out);
/* A whole setup message: the header (individual, src, dst, stream, path; seq 0) and one setup item. Returns the
   length, 0 when cap is too small. */
size_t rdof_mn_setup_msg(uint8_t *out, size_t cap, uint32_t stream, uint32_t src, uint32_t dst, uint8_t path,
                         const rdof_mn_setup_t *s);
/* A setup message's header and item (its first item is a setup item). False: not a setup message. */
bool rdof_mn_setup_parse(const uint8_t *msg, size_t len, rdof_mn_hdr_t *h, rdof_mn_setup_t *s);
/* The op and reason as the activity event names them ("request", "not_found"); "?" for others. */
const char *rdof_mn_setup_op_name(int op);
const char *rdof_mn_setup_why_name(int reason);
/* A Tier II box's answer to a request on its dl (section 9.4): accept, unconfirmed, when the called radio was last
   heard on a path of this box that can transmit (heard_path, -1: none), else reject not_found. The answer goes on
   {root}/pc/{h->src}/{node}: src the called radio, dst the caller, ref the request's stream, path heard_path.
   stream: the answer's own stream ID. Returns the length, 0 when no answer is due (not a request) or cap is short. */
size_t rdof_mn_setup_answer(const rdof_mn_hdr_t *h, const rdof_mn_setup_t *req, int heard_path, uint32_t stream,
                            uint8_t *out, size_t cap);

/* A burst's data type to a data_pdu rate, and back (-1: not a data block type). */
int rdof_mn_rate_of_dt(int dt);
int rdof_mn_dt_of_rate(int rate);

/* ── topics ──────────────────────────────────────────────────────────────── */

typedef enum { RDOF_MN_T_NONE = 0, RDOF_MN_T_TG, RDOF_MN_T_PC, RDOF_MN_T_DL } rdof_mn_topic_kind_t;

/* A node name: letters, digits, '.', '_', '-'; 1 to 39 characters. */
bool rdof_mn_node_ok(const char *node);
/* {root}/tg/{id}/{node} or {root}/pc/{id}/{node} (kind RDOF_MN_T_TG / _PC), {root}/dl/{node} (RDOF_MN_T_DL).
   Returns the length, 0 when it doesn't fit. */
size_t rdof_mn_topic(char *out, size_t cap, const char *root, int kind, uint32_t id, const char *node);
/* {root}/node/{node}/{leaf} */
size_t rdof_mn_node_topic(char *out, size_t cap, const char *root, const char *node, const char *leaf);
/* A call topic's kind, id and node (copied, cut at cap). RDOF_MN_T_NONE: not one of the three, or not under root. */
int rdof_mn_topic_parse(const char *topic, const char *root, uint32_t *id, char *node, size_t cap);

/* ── call_key, and the JSON both sides write ─────────────────────────────── */

/* "{src}-{dst}-{g|p}-{t}", t the start in UTC seconds rounded down to a multiple of 2. */
size_t rdof_mn_call_key(uint32_t src, uint32_t dst, bool group, double wall, char *out, size_t cap);

/* ── the uplink: what a receiver hears, as call messages ─────────────────── */

#define RDOF_MN_UP_HOLD 6 /* voice bursts held for a late entry's addresses (section 4.3: at most 6) */

typedef struct {
    bool active;   /* a stream in progress on the slot */
    bool sending;  /* admitted: its messages are published */
    bool voice;
    uint32_t stream, src, dst;
    uint8_t flags, seq;
    int path;
    double start, last, start_wall;
    /* the call's measures (call_end) */
    int bursts, lost, next_n;
    long bits, errs;
    int rssi_n, rssi_min, rssi_max;
    long rssi_sum;
    /* the embedded LC of the superframe in progress */
    uint8_t frags[4][32];
    uint8_t have_frag;
    /* the talker alias being put together */
    int ta_format, ta_len, ta_have;
    uint8_t ta[32];
    bool ta_done;
    double header_at; /* when the lc_header item last went */
    /* voice heard before the late entry's addresses */
    rdof_frame_t held[RDOF_MN_UP_HOLD];
    int nheld;
    double held_at;
    /* CSBKs folded into one item: preambles wait for their data call */
    bool csbk;
    uint8_t csbk10[10];
    int csbk_count;
    uint32_t csbk_src, csbk_dst;
    bool csbk_group;
    double csbk_at;
    /* a data call being put together */
    bool data;
    int dpf, rate, nheaders, blocks_left, sap;
    bool confirmed, response_requested, bad;
    int ns;
    uint8_t headers[RDOF_MN_DATA_HEADERS][10];
    size_t len;
    uint8_t buf[RDOF_MN_DATA_MAX + 32];
    int q_bits, q_rssi_sum, q_n; /* the data call's quality */
} rdof_mn_upslot_t;

/* A data call heard and checked: for the confirmed-data answer (section 4.4) and the box's own records. */
typedef struct {
    int slot, path;
    uint32_t src, dst;
    bool group, confirmed, response_requested;
    int dpf, sap, ns, rate;
    const uint8_t *data;
    size_t len;
    bool sent;   /* published (the path's policy sends it to this network) */
    bool repeat; /* the same message again within 5 s: answered again, not published */
} rdof_mn_data_t;

typedef struct rdof_mn_up {
    const char *node;
    uint64_t tx_ok;   /* paths that can transmit (bit per path): the activity's tx_ok */
    size_t max_msg;   /* the largest message to publish (0: RDOF_MN_MSG_MAX); data calls past it go in parts */
    /* hooks; ctx below */
    int (*path_of)(void *ctx, int slot); /* the talk path slot is heard on, -1: none on this network */
    /* whether a stream goes to this network (the path's policy, rdof/router.h), asked once a stream */
    bool (*admit)(void *ctx, int slot, uint32_t src, uint32_t dst, bool group);
    /* a message for {root}/{tg|pc}/{dst}/{node} (kind RDOF_MN_T_TG / _PC) */
    void (*publish)(void *ctx, int kind, uint32_t dst, const uint8_t *msg, size_t len);
    void (*activity)(void *ctx, const char *json); /* an event for {root}/node/{node}/activity; NULL none */
    void (*data_done)(void *ctx, const rdof_mn_data_t *d); /* NULL none */
    /* a data response a radio sent (its ACK or NAK of data this box delivered): used here, not published */
    void (*response)(void *ctx, int slot, uint32_t src, uint32_t dst, int klass, int type, int status);
    uint32_t (*rand32)(void *ctx);
    double (*wall)(void *ctx); /* Unix time, for the events; NULL: none (at is the epoch) */
    void *ctx;
    rdof_mn_upslot_t slot[2];
    /* the data calls answered lately (repeats are answered again but not published) */
    struct {
        uint32_t hash;
        double at;
    } recent[4];
    int recent_next;
    uint32_t messages, not_sent, data_bad, too_big;
    uint8_t out[RDOF_MN_MSG_MAX];
} rdof_mn_up_t;

/* Clears it; set the hooks after. */
void rdof_mn_up_init(rdof_mn_up_t *u, const char *node);
/* A frame from the tracker (rdof_sink_t.on_frame), at now (seconds, any steady clock). */
void rdof_mn_up_frame(rdof_mn_up_t *u, const rdof_frame_t *f, double now);
/* Call a few times a second: folded CSBKs that waited long enough go out, held voice and data calls expire. */
void rdof_mn_up_tick(rdof_mn_up_t *u, double now);

/* ── the downlink: call messages, as network bursts ──────────────────────── */

#define RDOF_MN_DOWN_STREAMS 6
#define RDOF_MN_DOWN_IDLE_S 0.5 /* a stream with nothing this long ends */
/* While only headers are heard (a radio or modem may send them for half a second or more before the voice), the
   lc_header item goes again when nothing has gone for this long, so the far end's RDOF_MN_DOWN_IDLE_S doesn't end the
   stream before its voice comes. */
#define RDOF_MN_UP_HEADER_AGAIN_S 0.24

typedef struct {
    bool used;
    char node[RDOF_MN_NODE_MAX + 1];
    uint32_t stream;
    rdof_mn_hdr_t h;     /* the first message's */
    uint8_t next_seq;    /* the item seq expected next */
    uint8_t burst_seq;   /* the network bursts' sequence number (rdof_net_burst_t.seq) */
    double last;
    bool voice;          /* voice went out: a terminator ends it */
    bool ended;
    bool has_lc;
    uint8_t lc[9];       /* the call's LC: the header's, or the late entry's */
    uint8_t emb[9];      /* the embedded LC for the next superframe */
    int next_voice;      /* the voice index expected next */
    int items_per_burst; /* this stream's items for each voice burst, x16 (a gap's bursts from its items) */
    /* a data call in parts */
    bool parts;
    int next_part, rate, nheaders;
    uint8_t headers[RDOF_MN_DATA_HEADERS][10];
    size_t len;
    uint8_t pending_csbk[11]; /* a csbk item that goes before the data call's blocks */
    bool has_pending_csbk;
    uint8_t buf[RDOF_MN_DATA_MAX]; /* last: a reused slot clears everything before it */
} rdof_mn_dstream_t;

typedef struct rdof_mn_down {
    bool raw_tx; /* transmit raw items as heard (default: drop them) */
    int max_preambles; /* the most CSBK preambles sent for a csbk item's count (default 8) */
    /* a burst to transmit: path the message's path byte (RDOF_MN_PATH_NONE: none), dl: from {root}/dl/{node} */
    void (*burst)(void *ctx, const rdof_net_burst_t *b, int path, bool dl, uint8_t flags, const char *from);
    /* a data call's bursts went to burst (all of them, just now): for its data_result (rdof_mn_results_t); NULL none.
       dl: from {root}/dl/{node}, so h->path is this box's path (else it is the publisher's) */
    void (*data)(void *ctx, const rdof_mn_hdr_t *h, bool confirmed, bool dl, const char *from);
    /* a setup message (section 9.4): never bursts; NULL: dropped (counted in setups too) */
    void (*setup)(void *ctx, const rdof_mn_hdr_t *h, const rdof_mn_setup_t *s, bool dl, const char *from);
    void *ctx;
    rdof_mn_dstream_t s[RDOF_MN_DOWN_STREAMS];
    uint32_t messages, bad, gaps, late, parts_lost, raw_dropped, streams, replaced, setups;
} rdof_mn_down_t;

void rdof_mn_down_init(rdof_mn_down_t *d);
/* A message from node (the topic's), on dl or not, at now. False: not a call message, or one whose first item is a
   setup item that isn't valid (dropped, counted in bad). */
bool rdof_mn_down_message(rdof_mn_down_t *d, const char *node, bool dl, const uint8_t *msg, size_t len, double now);
/* Streams with nothing for RDOF_MN_DOWN_IDLE_S end (a terminator goes out after voice). */
void rdof_mn_down_tick(rdof_mn_down_t *d, double now);
/* The publisher of stream id lately, NULL none (for the tx event's "from"). */
const char *rdof_mn_down_from(const rdof_mn_down_t *d, uint32_t stream);

/* ── data_result: what became of a data call this box delivered ───────────── */

#define RDOF_MN_PENDING 4
#define RDOF_MN_RESPONSE_S 1.0 /* after the call's last burst on air, the radio's answer comes within this */
#define RDOF_MN_ON_AIR_S 5.0   /* a data call not on air within this failed (no transmitter) */

typedef struct {
    bool used, confirmed, sent;
    uint32_t stream, src, dst;
    int path, slot;
    char from[RDOF_MN_NODE_MAX + 1];
    double at; /* added; once sent, when its transmission ended */
} rdof_mn_pending_t;

typedef struct {
    rdof_mn_pending_t p[RDOF_MN_PENDING];
} rdof_mn_results_t;

/* A data call handed to the transmitters (the downlink's data hook). path, slot: this box's path and slot it goes
   out on, when known (a dl message's path byte), else -1 and 0: never the header's, which on a tg message are the
   publisher's. The oldest pending one makes room. */
void rdof_mn_results_add(rdof_mn_results_t *r, const rdof_mn_hdr_t *h, bool confirmed, const char *from, int path,
                         int slot, double now);
/* Each returns a result ("unconfirmed", "delivered", "failed", "no_response") with the call in out, NULL none.
   sent: the transmitter finished stream (its tx "end") on path and slot (-1, 0: not known; the ones held stay); an
   unconfirmed call is done then. */
const char *rdof_mn_results_sent(rdof_mn_results_t *r, uint32_t stream, int path, int slot, double now,
                                 rdof_mn_pending_t *out);
/* response: a radio's data response heard (rdof_mn_up_t.response): from src, to dst; ACK (class 0, type 1)
   delivered, the others failed. */
const char *rdof_mn_results_response(rdof_mn_results_t *r, uint32_t src, uint32_t dst, int klass, int type,
                                     rdof_mn_pending_t *out);
/* tick: no answer in RDOF_MN_RESPONSE_S (no_response), never on air (failed). Call until it returns NULL. */
const char *rdof_mn_results_tick(rdof_mn_results_t *r, double now, rdof_mn_pending_t *out);

/* ── data calls, both ways ───────────────────────────────────────────────── */

#if RDOF_FEATURE_DATA_BLOCKS
#define RDOF_MN_DATA_BURSTS 130 /* headers, and up to 127 blocks */
/* A data call rebuilt for the air (section 4.4): the headers (for DPF unconfirmed and confirmed: blocks to follow
   and pad octets recomputed, the CRC added), then blocks at rate (pad, the CRC-32 over the user data; confirmed:
   serial numbers 0.. and the CRC-9s). Other DPFs: data is the blocks' octets as heard. Returns the bursts written
   into bursts / controls (MMDVM DMR_DATA control bytes), 0 when it doesn't fit. */
int rdof_mn_data_bursts(int rate, int nheaders, const uint8_t *headers10, const uint8_t *data, size_t len,
                        uint64_t sync48, uint8_t controls[RDOF_MN_DATA_BURSTS],
                        uint8_t bursts[RDOF_MN_DATA_BURSTS][RDOF_BURST_BYTES]);
#endif

/* ── the node: info, subscriptions ───────────────────────────────────────── */

/* The info JSON (section 6) for site's paths on network net. firmware, transport: strings; claims: the box's apps'
   IDs, from the site. Returns the length (0: didn't fit). */
typedef struct {
    const char *node, *name, *firmware, *transport;
    uint32_t dmr_id;
    int bundle;
    bool has_location;
    double lat, lon;
    const char *location_source; /* "config", "gnss" */
    uint64_t trunk_paths; /* paths that are a trunked site's (a bit each): "trunk": true on them */
    const char *trunk;    /* the trunk object's JSON (rdof_mn_trunk_info_json), NULL: none */
} rdof_mn_info_t;
size_t rdof_mn_info_json(const rdof_site_t *s, int net, const rdof_mn_info_t *i, char *out, size_t cap);
/* paths_rev: CRC-16/CCITT-FALSE over, for each path on net in order, its index byte, its name and a zero byte;
 * 0xFFFF for none. */
uint16_t rdof_mn_paths_rev(const rdof_site_t *s, int net);
/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, not reflected, no final XOR): "123456789" gives 0x29B1. */
uint16_t rdof_mn_crc16(const uint8_t *data, size_t len);

#define RDOF_MN_SUBS 24      /* topic filters a box takes at once */
#define RDOF_MN_SUBS_EXPAND 16 /* a tg set with more than this after expanding: {root}/tg/+/+ */

/* The talkgroups a box subscribes to for its paths on net (section 8.3), held ptt TGs among them (held: a callback,
   NULL none). all: subscribe {root}/tg/+/+ and filter locally. Returns the TGs written to tgs (at most max). */
struct rdof_router;
int rdof_mn_sub_tgs(const rdof_site_t *s, int net, const struct rdof_router *r, double now, uint32_t *tgs, int max,
                    bool *all);

/* ── activity events from the platform ───────────────────────────────────── */

/* {"ev":"tx",...}: this box starts or stops transmitting a network stream (action "start" / "end"). */
size_t rdof_mn_tx_event(const char *node, double wall, int path, int slot, uint32_t stream, const char *action,
                        uint32_t src, uint32_t dst, const char *from, char *out, size_t cap);
/* The common fields of an event (section 7), for the events below. path -1: null. */
typedef struct {
    const char *node;
    double wall;
    int path, slot;
    const char *source; /* "rf", "net"; NULL: "rf" */
    uint32_t stream;
} rdof_mn_ev_t;

/* A trunked site's events (section 7, trunk.md): */
/* {"ev":"reg",...}: action "register", "deregister", "expire", "deny", "refuse"; tg the radio's talkgroups */
size_t rdof_mn_reg_event(const rdof_mn_ev_t *e, uint32_t src, const char *action, const uint32_t *tg, int ntg,
                         char *out, size_t cap);
/* {"ev":"grant",...}: a payload channel granted (chan the logical channel, lcn 0 or 1); late: a late entry grant */
size_t rdof_mn_grant_event(const rdof_mn_ev_t *e, uint32_t src, uint32_t dst, bool group, int chan, int lcn, bool late,
                           char *out, size_t cap);
/* {"ev":"queue",...}: action "in", "out", "drop"; why NULL: none */
size_t rdof_mn_queue_event(const rdof_mn_ev_t *e, uint32_t src, uint32_t dst, bool group, const char *action,
                           const char *why, char *out, size_t cap);
/* {"ev":"setup",...}: a call setup sent or received; peer the other node, NULL: none */
size_t rdof_mn_setup_event(const rdof_mn_ev_t *e, uint32_t src, uint32_t dst, int op, int reason, const char *peer,
                           char *out, size_t cap);

/* A Tier II box's side of call setup (section 9.4), for the downlink's setup hook. A setup on dl gets a setup event
   (the box received it); a request also gets its answer (rdof_mn_setup_answer) and an event for that. The answer is
   accept, unconfirmed, when heard_path (where the box last heard the called radio, -1 none) is a path of site on net
   that can send (rdof_mn_path_can_send), else reject not_found. Setups on tg are ignored. stream: the answer's own
   stream ID. Returns the events written. */
typedef struct {
    size_t len;  /* the answer, for {root}/pc/{to}/{node}; 0: none */
    uint32_t to; /* the caller */
    uint8_t msg[RDOF_MN_HEADER + 2 + RDOF_MN_SETUP_LEN];
    int nev;
    char ev[2][320]; /* the activity events, in order: the setup received, then the answer sent */
} rdof_mn_tier2_t;
int rdof_mn_setup_tier2(const rdof_site_t *site, int net, const char *node, double wall, const rdof_mn_hdr_t *h,
                        const rdof_mn_setup_t *s, bool dl, const char *from, int heard_path, uint32_t stream,
                        rdof_mn_tier2_t *out);
/* Path k of site is on network net and can deliver a call from it: a frequency that transmits, and a policy that lets
   the network out to RF (info's tx_ok, section 6). */
bool rdof_mn_path_can_send(const rdof_site_t *site, int net, int k);

/* {"ev":"data_result",...} */
size_t rdof_mn_data_result_event(const char *node, double wall, int path, int slot, uint32_t stream, uint32_t src,
                                 uint32_t dst, const char *from, uint32_t from_stream, const char *result, int tries,
                                 char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
