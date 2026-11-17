// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A P25 or NXDN repeater with no network, on a duplex MMDVM modem in that mode: each frame the modem hears goes back
 * to it to send, as heard. The input's NAC or RAN, its status symbols and any bit errors pass through; nothing is
 * decoded but the P25 NID's NAC and DUID. No I/O. Rust only (rust/src/mrelay.rs): there with RDOF_RUST.
 */
#ifndef RDOF_MRELAY_H
#define RDOF_MRELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_P25_DUID_HDU 0x0
#define RDOF_P25_DUID_TDU 0x3
#define RDOF_P25_DUID_LDU1 0x5
#define RDOF_P25_DUID_TSDU 0x7
#define RDOF_P25_DUID_LDU2 0xA
#define RDOF_P25_DUID_PDU 0xC
#define RDOF_P25_DUID_TDULC 0xF

typedef struct {
    int mode;     /* RDOF_MODE_P25 or RDOF_MODE_NXDN (rdof/mmdvm.h) */
    bool in_call; /* frames relayed since the last end */
    int64_t frames, lost, calls;
} rdof_mrelay_t;

/* What one frame from the modem was. */
typedef struct {
    int duid;   /* P25: the NID's DUID; -1: none (NXDN, a loss) */
    int nac;    /* P25: the NID's NAC as heard (no error correction); -1: none */
    int rssi;   /* the modem's raw RSSI; -1: none */
    bool lost;  /* the modem lost the signal */
    bool start; /* the first frame relayed since the last end: the transmitter keys */
    bool end;   /* a terminator (P25 TDU, TDULC) or a loss ended a call */
} rdof_mrelay_heard_t;

void rdof_mrelay_init(rdof_mrelay_t *r, int mode);

/* A frame the modem heard: its command and payload (rdof_modem_t.on_frame). Writes the frame to hand back to the
 * modem, *out_cmd and out (*out_len bytes, 0: nothing to send; cap 256 is enough) and what it was. False: not a frame
 * of this relay's mode, or too short (nothing written). */
bool rdof_mrelay_heard(rdof_mrelay_t *r, uint8_t cmd, const uint8_t *p, size_t len, uint8_t *out_cmd, uint8_t *out,
                       size_t cap, size_t *out_len, rdof_mrelay_heard_t *h);

#ifdef __cplusplus
}
#endif

#endif
