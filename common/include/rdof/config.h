// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * One unit that transmits (rdof/siteplan.h): a repeater app, an SFR or DCDM frequency, or a device transmitting on
 * simplex frequencies (a transceiver), from the settings (version 3, docs/spec/fw/config-v3.md section 12),
 * resolved: its device's port, type and radio settings, its frequencies, its app, the networks and the board's keys.
 * JSON with // and block comments and trailing commas is taken.
 *
 * Keys read: the unit's device (port, type, mmdvm {...}, the talker keys, tx.power_dbm on an SDR), its frequencies
 * (freq, cc, power, ts, and an SFR or DCDM frequency's own keys), its repeater app (hang_s, active_slots, split
 * {...}, ...), the hblink and mqtt networks, and the top level's name, callsign, location, oled and slot_timing.
 * An SDR's frequencies are `covered`. The firmware's wifi and ntp keys aren't read here: the board's Wi-Fi and
 * clock components keep those.
 *
 * "${VAR}" values come out empty (no environment on the board).
 */
#ifndef RDOF_CONFIG_H
#define RDOF_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/slotgrid.h"

#include "rdof/hblink.h"
#include "rdof/mmdvm.h"
#include "rdof/modem.h"
#include "rdof/txpower.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_CFG_CHANNELS 32
/* Units that transmit one board runs (rdof_box_roles refuses more). */
#ifndef RDOF_MAX_RADIOS
#define RDOF_MAX_RADIOS 2
#endif
/* Units that transmit on a host at most (rdofradio: split repeaters on one SDR, each with its own radio). */
#define RDOF_ROLES_MAX 8

typedef struct {
    uint32_t freq;
    int cc, power;
    int slot; /* 1 or 2: a direct mode call's here (the plan's slot; nothing on the air says) */
} rdof_cfg_channel_t;

typedef enum { RDOF_CFG_ROAMER = 1, RDOF_CFG_REPEATER } rdof_cfg_mode_t;
/* A unit on the SDR: none (a modem), half (a transceiver, or a DCDM frequency), full (a repeater, or a DCDM
   frequency with duplex full). */
typedef enum { RDOF_SDR_DUPLEX_NONE = 0, RDOF_SDR_DUPLEX_HALF, RDOF_SDR_DUPLEX_FULL } rdof_sdr_duplex_t;

/* A modem's port on the board (firmware only): a device's port, or a split's half. */
typedef struct {
    char modem_port[8]; /* usb, uart, or sdr (the board's SDR: an RX half only) */
    int uart_tx, uart_rx, uart_num;
    long uart_speed;
    char usb_serial[32], usb_port[16];
} rdof_cfg_port_t;

typedef struct {
    rdof_cfg_mode_t mode;
    char name[24];      /* the unit's name (MQTT channel) */
    uint32_t peer_id;
    char callsign[12];
    char master_host[64];
    int master_port;
    char transport[8];  /* udp, or sctp (over UDP encapsulation, RFC 6951) */
    /* hblink.sctp_*, as MMDVMHost's SCTP* settings */
    int sctp_encap_port;     /* HBlink4's sctp_encap_port */
    int sctp_heartbeat_ms;   /* SCTPHeartbeat; 0: none */
    int sctp_max_retransmit; /* SCTPMaxRetransmit */
    int sctp_ttl_ms;         /* SCTPTTL, for DMRD only: give up on a burst after this; 0: reliable */
    bool receiver_hblink;

    /* the radio */
    char radio_type[16]; /* the device's type: radio_hotspot, mmdvm_hs, mmdvm; auto on the SDR; split */
    rdof_modem_config_t modem;
    int32_t rx_offset_hz, tx_offset_hz;
    rdof_tuner_kind_t tuner;
    uint32_t tuner_freq; /* fixed tuner; 0: the repeater's TX frequency */
    rdof_rssi_map_t rssi;
    int headers, terminators; /* -1: the kind's default */
    int settle_ms;

    /* a transceiver */
    rdof_cfg_channel_t channels[RDOF_CFG_CHANNELS];
    int nchannels;
    rdof_cfg_channel_t idle;
    double listen_s, clear_s;
    int prebuffer, csbk_preambles; /* prebuffer: the jitter buffer's fixed size, in bursts (a repeater's too) */
    bool jitter_auto;              /* jitter_buffer "auto" (the default): sized from the network, in these bounds */
    int jitter_min_ms, jitter_max_ms; /* whole bursts (60 ms) */
    double answer_s, answer_max_s, idle_after_s;
    uint32_t covered[RDOF_CFG_CHANNELS];
    int ncovered;
    bool has_scan;
    rdof_cfg_channel_t scan_channels[RDOF_CFG_CHANNELS];
    int nscan;
    double dwell_s, hang_s_scan, carrier_s;

    /* a repeater */
    uint32_t rx_freq, tx_freq;
    int cc, power;
    bool slot1, slot2;
    double hang_s, call_hang_s; /* T_MSInactiv, T_CallHt */
    bool short_lc_ts1_low;      /* interop: Act_Updt with TS1 in the low nibble */
    bool sfr;
    bool dcdm;                  /* mode dcdm: dual capacity direct mode on freq (rx_freq = tx_freq) */
    int inbound_slot;
    rdof_slot_timing_t slot_timing; /* slot_timing: the app's or frequency's own over the box's (rdof/slotgrid.h) */

    char cwid[41];
    double cwid_every_s;
    bool has_parrot; /* parrot: send back what's heard (rdof/parrot.h) */
    uint32_t parrot; /* only what's addressed to this id; 0: everything */

    bool has_location;
    rdof_location_t location;

    bool has_mqtt;
    char mqtt_host[64];
    int mqtt_port;
    char mqtt_user[32], mqtt_pass[64];
    char mqtt_root[16], mqtt_app[16];
    bool mqtt_tls;

    bool has_oled;
    int oled_address;
    bool oled_sh1106, oled_rotate180;
    double oled_refresh_s;
    int oled_sda, oled_scl;
    int oled_channel; /* this radio's channel number in the plan, 0 if none */

    /* firmware only: the device's port */
    char modem_port[8]; /* usb / uart (a path is the host's: usb); sdr for a unit on the SDR; split */
    rdof_sdr_duplex_t sdr_duplex; /* a unit on the SDR */
    double tx_power_dbm;        /* and its power at the antenna (the SDR's tx.power_dbm, 10) */
    int uart_tx, uart_rx;
    int uart_num;       /* 1 or 2 (port.uart) */
    long uart_speed;    /* port.baud, 115200 */
    char usb_serial[32]; /* port.serial: the modem's USB serial number, for two of the same kind ("" any) */
    char usb_port[16];  /* port.usb_port: where it's plugged in, hub ports from the board down ("2", "1.3"; "" any) */
    /* a split repeater (two devices; modem_port "split"): the TX and RX devices' ports ([0] TX, [1] RX) and
     * SET_ROLE's parameters (the app's split {power_cap, ...}; 0: the half's default). usb_serial / usb_port above
     * are the USB half's, uart_* the UART half's (the TX half's if both are). */
    rdof_cfg_port_t split[2];
    int split_power_cap, split_prebuffer, split_lease_s, split_cooldown_s;
    double split_max_key_s;
    rdof_txmap_t tx_map; /* the transmitter's RF levels in dBm (rdof/txpower.h): channels' and power are its levels */
} rdof_config_t;

/* The first unit that transmits in the settings (version 3); false with `error` set when they're refused ("apps.R1:
   ...", the place in the settings). */
bool rdof_config_parse(const char *text, size_t len, rdof_config_t *cfg, char *error, size_t error_cap);
/* The same for the unit called name (NULL: the first). */
bool rdof_config_parse_radio(const char *text, size_t len, const char *name, rdof_config_t *cfg, char *error,
                             size_t error_cap);
/* The same from a plan (rdof/siteplan.h); the text forms above open one each. */
struct rdof_plan;
bool rdof_plan_radio(const struct rdof_plan *p, const char *name, rdof_config_t *cfg, char *error, size_t error_cap);
/* The units a board runs can't share a port: one device a UART, and two USB devices of the same VID / PID (all
   radio_hotspot radios are) each need port.serial or port.usb_port. false with `error` set. */
bool rdof_config_check_ports(const rdof_config_t *const *cfgs, int n, char *error, size_t error_cap);
/* The board's ports a unit uses: one, or a split's two halves. Returns how many (up to 2). */
int rdof_config_ports(const rdof_config_t *cfg, rdof_cfg_port_t out[2]);
/* A unit on USB (its own modem, or a split half). */
bool rdof_config_uses_usb(const rdof_config_t *cfg);
/* What a board runs: every unit the settings make (rdof/siteplan.h). */
typedef struct {
    char box[32];   /* the box's name ("radiofoundry" if none) */
    bool has_radio, has_sdr;
    char radio[24]; /* the first unit that transmits */
    int nradios;    /* units that transmit: up to the platform's (a board RDOF_MAX_RADIOS, a host RDOF_ROLES_MAX) */
    bool usb_hub;   /* "usb_hub": true: the USB port has a hub, and every unit on it runs at once */
    char radios[RDOF_ROLES_MAX][24];
    char sdr[32];   /* the SDR's name: its receiver's, else the device's */
} rdof_roles_t;

/* The units a board runs: false with `error` set when the settings are refused, or make more than a board runs
   (RDOF_MAX_RADIOS units that transmit, one SDR) or nothing. */
bool rdof_box_roles(const char *text, size_t len, rdof_roles_t *roles, char *error, size_t error_cap);
/* A stable one-line description of everything resolved (for the tests and the web page). */
size_t rdof_config_describe(const rdof_config_t *cfg, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
