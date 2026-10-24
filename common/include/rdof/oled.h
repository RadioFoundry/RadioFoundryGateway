// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A 128x64 SSD1306 / SH1106 frame and its panel commands with
 * no I/O (a 5x7 font), and a simple status screen for a board running one
 * roamer or repeater: the board's own state.
 */
#ifndef RDOF_OLED_INCLUDED
#define RDOF_OLED_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_OLED_W 128
#define RDOF_OLED_H 64
#define RDOF_OLED_PAGES 8

typedef struct {
    uint8_t buf[RDOF_OLED_W * RDOF_OLED_PAGES];
} rdof_frame_buf_t;

void rdof_oled_clear(rdof_frame_buf_t *f);
void rdof_oled_pixel(rdof_frame_buf_t *f, int x, int y, bool on);
bool rdof_oled_get(const rdof_frame_buf_t *f, int x, int y); /* false off the panel */
void rdof_oled_hline(rdof_frame_buf_t *f, int x0, int x1, int y, bool on);
void rdof_oled_rect(rdof_frame_buf_t *f, int x, int y, int w, int h, bool fill, bool on);
/* 5x7 text in 6-pixel cells at (x, y) top left; returns the x after it. tight: punctuation takes one column. */
int rdof_oled_text(rdof_frame_buf_t *f, int x, int y, const char *s, bool invert, bool tight);
int rdof_oled_text_width(const char *s, bool tight);

/* Panel.open's command bytes (each sent as 0x00, cmd). Returns the count. */
size_t rdof_oled_init_cmds(bool rotate180, int contrast, uint8_t *out, size_t cap);
/* The command bytes that address one page before its 128 data bytes (0x40, ...). */
size_t rdof_oled_page_cmds(bool sh1106, int page, uint8_t *out, size_t cap);

typedef struct {
    const char *name;     /* receiver name */
    const char *ip;       /* "" when not connected */
    bool wifi, mqtt, hblink, radio;
    bool repeater;
    uint32_t freq, tx_freq;
    int cc;
    const char *state;    /* idle / listening / on air / scanning ... */
    const char *rx_line;  /* the call being heard ("" none), e.g. "T201<9990123" */
    bool rx_active;
    const char *tx_line;  /* what it's sending ("" none) */
    bool tx_active;
    const char *clock;    /* "23:41:07" or "" */
} rdof_oled_status_t;

void rdof_oled_render(rdof_frame_buf_t *f, const rdof_oled_status_t *s);

#ifdef __cplusplus
}
#endif

#endif
