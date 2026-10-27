// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests of the Linux HAL's opening (hal/linux.c) without the hardware: each failure gives its error and a
// message, and leaves no descriptor open. Linux only.
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_hal_linux.h"

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

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

static void fails_cleanly(const char *spidev, const char *gpiochip, int busy, int nreset, lr2021_err_t want,
                          const char *says)
{
    lr2021_linux_cfg_t c = {.spidev = spidev, .gpiochip = gpiochip, .busy = busy, .nreset = nreset, .irq = -1,
                            .pa_en = -1};
    lr2021_hal_t hal;
    memset(&hal, 0, sizeof hal);
    char err[160] = "";
    int before = open_fds();
    lr2021_err_t e = lr2021_hal_linux_open(&c, &hal, err, sizeof err);
    CHECK(e == want, "%s: error 0x%x, not 0x%x (%s)", spidev, (unsigned)e, (unsigned)want, err);
    CHECK(strstr(err, says) != NULL, "%s: message \"%s\" lacks \"%s\"", spidev, err, says);
    CHECK(hal.ctx == NULL && hal.xfer == NULL, "%s: hal filled on a failure", spidev);
    CHECK(open_fds() == before, "%s: %d descriptors before, %d after", spidev, before, open_fds());
    /* no room for the message: still the error, nothing written */
    e = lr2021_hal_linux_open(&c, &hal, NULL, 0);
    CHECK(e == want, "%s, no message: error 0x%x", spidev, (unsigned)e);
    CHECK(open_fds() == before, "%s, no message: descriptors left open", spidev);
}

int main(void)
{
    fails_cleanly("/dev/spidev0.0", "/dev/gpiochip0", -1, 25, LR2021_ERR_INVALID_ARG, "busy and nreset");
    fails_cleanly("/dev/spidev0.0", "/dev/gpiochip0", 24, -1, LR2021_ERR_INVALID_ARG, "busy and nreset");
    fails_cleanly("/nonexistent/spidev9.9", "/dev/gpiochip0", 24, 25, LR2021_ERR_NOT_FOUND, "/nonexistent/spidev9.9");
    /* a file that isn't spidev: the mode won't take */
    fails_cleanly("/dev/null", "/dev/gpiochip0", 24, 25, LR2021_FAIL, "/dev/null: mode 0");
    printf("lr2021 Linux HAL: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
