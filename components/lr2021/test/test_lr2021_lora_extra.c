// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for lr2021_lora_extra.h: the 10-bit sync word frames and the ranging delay table.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_lora_extra.h"

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

static void frame(const uint8_t *got, size_t n, const uint8_t *want, size_t wn, const char *what)
{
    bool same = n == wn && !memcmp(got, want, n);
    char g[96] = "";
    for (size_t i = 0, o = 0; i < n && o + 4 < sizeof g; i++)
        o += (size_t)snprintf(g + o, sizeof g - o, "%02X ", got[i]);
    CHECK(same, "%s: got %s(%zu bytes)", what, g, n);
}

#define FRAME(call, ...)                           \
    do {                                           \
        uint8_t b_[LR2021_LR_FHSS_FRAME_MAX];      \
        static const uint8_t w_[] = {__VA_ARGS__}; \
        size_t n_ = call;                          \
        frame(b_, n_, w_, sizeof w_, #call);       \
    } while (0)

int main(void)
{
    // the 10-bit form: public (6, 8), private (2, 4), negatives as 5-bit two's complement
    FRAME(lr2021_lora_syncword_ext(b_, 6, 8), 0x02, 0x2D, 0x06, 0x08);
    FRAME(lr2021_lora_syncword_ext(b_, -2, -16), 0x02, 0x2D, 0x1E, 0x10);
    int8_t s1, s2;
    lr2021_lora_syncword_split(0x34, &s1, &s2);
    CHECK(s1 == 6 && s2 == 8, "0x34 splits %d %d", s1, s2);
    lr2021_lora_syncword_split(0x12, &s1, &s2);
    CHECK(s1 == 2 && s2 == 4, "0x12 splits %d %d", s1, s2);
    lr2021_lora_syncword_split(0xF8, &s1, &s2);
    CHECK(s1 == -2 && s2 == -16, "0xF8 splits %d %d", s1, s2);
    const int8_t pairs[3][2] = {{2, 4}, {6, 8}, {-2, -4}};
    FRAME(lr2021_lora_side_det_syncword_ext(b_, pairs), 0x02, 0x2E, 0x02, 0x04, 0x06, 0x08, 0x1E, 0x1C);

    CHECK(lr2021_rttof_base_delay(5, LR2021_LORA_BW_1000) == 21711 && lr2021_rttof_base_delay(12, LR2021_LORA_BW_125) == 14191 &&
              lr2021_rttof_base_delay(7, LR2021_LORA_BW_203) == 24089,
          "base delay table");
    CHECK(lr2021_rttof_base_delay(12, LR2021_LORA_BW_62) == 12400 && lr2021_rttof_base_delay(7, LR2021_LORA_BW_62) == 17825 &&
              lr2021_rttof_base_delay(4, LR2021_LORA_BW_125) == 0,
          "base delay estimate");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
