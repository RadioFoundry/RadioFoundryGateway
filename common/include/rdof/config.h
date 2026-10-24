// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The firmware's settings: a box config in JSON (docs/spec/fw/config.md),
 * for the one roamer or repeater the board runs, resolved: channel plan
 * references, defaults, validation. JSON with // and block comments and
 * trailing commas is taken.
 *
 * Box keys used: name, location, mqtt, hblink, cc, channels (the plan),
 * radios (the roamer or repeater "run" names, else the first that isn't an
 * SDR, runs here; an SDR's channels are `covered`), oled. Firmware-only keys:
 * run (the radios this board runs: a name or a list of names, one roamer or
 * repeater and one SDR at most), modem ("usb" / "uart": where the radio is),
 * uart {tx, rx}, oled {sda, scl}. The firmware's wifi and ntp keys
 * aren't read here: the board's Wi-Fi and clock components keep those.
 *
 * "${VAR}" values come out empty (no environment on the board); a radio's
 * serial_port, speed, protocol, modem_address, rigctld and command tuners
 * aren't used.
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

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_CFG_CHANNELS 32
/* Roamers or repeaters one board runs ("run" names up to this many). */
#ifndef RDOF_MAX_RADIOS
#define RDOF_MAX_RADIOS 2
#endif

typedef struct {
    uint32_t freq;
    int cc, power;
} rdof_cfg_channel_t;

typedef enum { RDOF_CFG_ROAMER = 1, RDOF_CFG_REPEATER } rdof_cfg_mode_t;
/* An sdr_transceiver's duplex: none (another radio), half (a roamer), full (a duplex repeater). */
typedef enum { RDOF_SDR_DUPLEX_NONE = 0, RDOF_SDR_DUPLEX_HALF, RDOF_SDR_DUPLEX_FULL } rdof_sdr_duplex_t;

/* A modem's port on the board (firmware only): a split's half, as a radio's own modem / uart / usb keys. */
typedef struct {
    char modem_port[8]; /* usb, uart, or sdr (the board's SDR: an RX half only) */
    int uart_tx, uart_rx, uart_num;
    long uart_speed;
    char usb_serial[32], usb_port[16];
} rdof_cfg_port_t;

typedef struct {
    rdof_cfg_mode_t mode;
    char name[24];      /* the receiver name (MQTT channel) */
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
    char radio_type[16]; /* auto, radio_hotspot (opengd77 in a config), mmdvm_hs, mmdvm */
    rdof_modem_config_t modem;
    int32_t rx_offset_hz, tx_offset_hz;
    rdof_tuner_kind_t tuner;
    uint32_t tuner_freq; /* fixed tuner; 0: the repeater's TX frequency */
    rdof_rssi_map_t rssi;
    int headers, terminators; /* -1: the kind's default */
    int settle_ms;

    /* a roamer */
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
    rdof_slot_timing_t slot_timing; /* slot_timing: its own over the box's (rdof/slotgrid.h) */

    char cwid[41];
    double cwid_every_s;

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

    /* firmware only: the radio's own keys, else the box's (modem, uart) */
    char modem_port[8]; /* usb / uart; sdr for an sdr_transceiver */
    rdof_sdr_duplex_t sdr_duplex; /* an sdr_transceiver's (type sdr_transceiver, duplex half or full) */
    double tx_power_dbm;        /* and its power (tx_power_dbm, 10) */
    int uart_tx, uart_rx;
    int uart_num;       /* 1 or 2 (uart.num) */
    long uart_speed;    /* the radio's speed, 115200 */
    char usb_serial[32]; /* usb.serial: the radio's USB serial number, for two of the same kind ("" any) */
    char usb_port[16];  /* usb.port: where it's plugged in, hub ports from the board down ("2", "1.3"; "" any) */
    /* radio type split (a repeater; modem_port "split"): the TX and RX halves' ports (radio.tx, radio.rx; [0] TX,
     * [1] RX) and SET_ROLE's parameters (radio.power_cap ...; 0: the half's default). usb_serial / usb_port above
     * are the USB half's, uart_* the UART half's (the TX half's if both are). */
    rdof_cfg_port_t split[2];
    int split_power_cap, split_prebuffer, split_lease_s, split_cooldown_s;
    double split_max_key_s;
} rdof_config_t;

/* Parse a settings document; false with `error` set when it's refused. The roamer or repeater is the
   first one "run" names (else the first in radios). */
bool rdof_config_parse(const char *text, size_t len, rdof_config_t *cfg, char *error, size_t error_cap);
/* The same for the roamer or repeater called name, which "run" must name (NULL: the first). */
bool rdof_config_parse_radio(const char *text, size_t len, const char *name, rdof_config_t *cfg, char *error,
                           size_t error_cap);
/* The radios a board runs can't share a port: one radio a UART, and two USB radios of the same VID / PID (all
   radio_hotspot radios are) each need usb.serial or usb.port. false with `error` set. */
bool rdof_config_check_ports(const rdof_config_t *const *cfgs, int n, char *error, size_t error_cap);
/* The board's ports a radio uses: one, or a split's two halves. Returns how many (up to 2). */
int rdof_config_ports(const rdof_config_t *cfg, rdof_cfg_port_t out[2]);
/* A radio on USB (its own modem, or a split half). */
bool rdof_config_uses_usb(const rdof_config_t *cfg);
/* Which radios a board runs from these settings. */
typedef struct {
    char box[32];   /* the box's name ("radiofoundry" if none); a plain SDR config's name */
    bool has_radio, has_sdr;
    char radio[24]; /* the (first) roamer's or repeater's name */
    int nradios;    /* roamers and repeaters: up to RDOF_MAX_RADIOS with "run", else one */
    bool usb_hub;   /* "usb_hub": true: the USB port has a hub, and every role on it runs at once */
    char radios[RDOF_MAX_RADIOS][24];
    char sdr[32];   /* the SDR's name */
    bool sdr_plain; /* the settings are a plain SDR config on its own (no radios), not a box config */
} rdof_roles_t;

/* "run" if it's there (up to RDOF_MAX_RADIOS roamers or repeaters and one SDR); else the first roamer or repeater,
   or failing that the first SDR. false with `error` set
   when the settings don't say (bad JSON, no radios, a type or a name in run that isn't known). */
bool rdof_box_roles(const char *text, size_t len, rdof_roles_t *roles, char *error, size_t error_cap);
/* A stable one-line description of everything resolved (for the tests and the web page). */
size_t rdof_config_describe(const rdof_config_t *cfg, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
