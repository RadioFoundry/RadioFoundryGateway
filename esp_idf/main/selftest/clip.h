// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Generated from the reference receiver. Do not edit. */
#pragma once

/* clip.cu8: 0.6 s of simulated traffic on 5 channels, cu8 at 288 kS/s */
#define CLIP_RATE 288000.0
#define CLIP_CENTER 461122500.0
#define CLIP_T0 1760000000.0
#define CLIP_CHUNK 4096
#define CLIP_RSSI_OFFSET -55.0
#define CLIP_EXPECT_MESSAGES 53
#define CLIP_EXPECT_CRC 0x283d7439u
#define CLIP_EXPECT_BURSTS 39
#define CLIP_CHANNELS 5
static const struct {
    const char *id;
    double freq;
    int cc, slot;
} clip_channels[CLIP_CHANNELS] = {
    {"SDR13", 461037500.0, 2, 1},
    {"SDR04", 461087500.0, 2, 1},
    {"SDR06", 461137500.0, 2, 1},
    {"SDR08", 461187500.0, 2, 1},
    {"SDR10", 461237500.0, 2, 1}
};

/* The clip with a fault put in (selftest_core.c: clip_impair), and what the reference makes of each */
#define CLIP_ROT_PERIOD 72
static const short clip_rot[CLIP_ROT_PERIOD][2] = {
    {16384, 0}, {16322, 1428}, {16135, 2845}, {15826, 4240}, {15396, 5604}, {14849, 6924},
    {14189, 8192}, {13421, 9397}, {12551, 10531}, {11585, 11585}, {10531, 12551}, {9397, 13421},
    {8192, 14189}, {6924, 14849}, {5604, 15396}, {4240, 15826}, {2845, 16135}, {1428, 16322},
    {0, 16384}, {-1428, 16322}, {-2845, 16135}, {-4240, 15826}, {-5604, 15396}, {-6924, 14849},
    {-8192, 14189}, {-9397, 13421}, {-10531, 12551}, {-11585, 11585}, {-12551, 10531}, {-13421, 9397},
    {-14189, 8192}, {-14849, 6924}, {-15396, 5604}, {-15826, 4240}, {-16135, 2845}, {-16322, 1428},
    {-16384, 0}, {-16322, -1428}, {-16135, -2845}, {-15826, -4240}, {-15396, -5604}, {-14849, -6924},
    {-14189, -8192}, {-13421, -9397}, {-12551, -10531}, {-11585, -11585}, {-10531, -12551}, {-9397, -13421},
    {-8192, -14189}, {-6924, -14849}, {-5604, -15396}, {-4240, -15826}, {-2845, -16135}, {-1428, -16322},
    {0, -16384}, {1428, -16322}, {2845, -16135}, {4240, -15826}, {5604, -15396}, {6924, -14849},
    {8192, -14189}, {9397, -13421}, {10531, -12551}, {11585, -11585}, {12551, -10531}, {13421, -9397},
    {14189, -8192}, {14849, -6924}, {15396, -5604}, {15826, -4240}, {16135, -2845}, {16322, -1428}
};
#define CLIP_NOISE_SEED 2463534242u
#define CLIP_NOISE_GAIN 40
#define CLIP_VARIANTS 3
static const struct {
    const char *name;
    int messages;
    unsigned crc;
    long bursts;
} clip_variants[CLIP_VARIANTS] = {
    {"offset", 53, 0xce0cc52eu, 39},
    {"noise", 53, 0x67ab9510u, 39},
    {"hot", 53, 0x75002f1du, 39}
};
