// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's FPGA bitstream on a PlutoSDR (rdofradio_fpga.h) */
#define _DEFAULT_SOURCE
#include "rdofradio_fpga.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdof/sdr/plutofpga.h"

#define NOTE "/tmp/rdof-fpga"
#define MNT "/tmp/rdof-mnt"
#define FIRMWARE "/lib/firmware"
#define FPGA_MGR "/sys/class/fpga_manager/fpga0"
#define MOST (16 * 1024 * 1024) /* a 7020's bitstream is 4 MB */

static int read_reg(uint32_t addr, uint32_t *v)
{
    const int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0)
        return -1;
    const long pg = sysconf(_SC_PAGESIZE);
    void *p = mmap(NULL, (size_t)pg, PROT_READ, MAP_SHARED, fd, (off_t)(addr & ~(uint32_t)(pg - 1)));
    close(fd);
    if (p == MAP_FAILED)
        return -1;
    *v = *(volatile uint32_t *)((uint8_t *)p + (addr & (uint32_t)(pg - 1)));
    munmap(p, (size_t)pg);
    return 0;
}

/* a whole file, at most MOST bytes: malloc'd, or NULL */
static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = malloc(MOST);
    *n = b ? fread(b, 1, MOST, f) : 0;
    const bool whole = b && feof(f) && !ferror(f) && *n > 0;
    fclose(f);
    if (!whole) {
        free(b);
        return NULL;
    }
    return b;
}

static int write_sys(const char *path, const char *v)
{
    const int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    const ssize_t w = write(fd, v, strlen(v));
    close(fd);
    return w == (ssize_t)strlen(v) ? 0 : -1;
}

/* the DDR's size from the device tree's memory node (one or two cells each for the address and the size) */
static uint64_t mem_bytes(void)
{
    glob_t g;
    uint64_t m = 0;
    if (glob("/proc/device-tree/memory*/reg", 0, NULL, &g))
        return 0;
    size_t n;
    uint8_t *b = read_file(g.gl_pathv[0], &n);
    globfree(&g);
    if (!b)
        return 0;
    if (n == 8)
        m = (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 | (uint64_t)b[6] << 8 | b[7];
    else if (n == 16)
        for (int i = 8; i < 16; i++)
            m = m << 8 | b[i];
    free(b);
    return m;
}

static bool facts(rdof_plutofpga_facts_t *f, char *err, size_t cap)
{
    if (read_reg(RDOF_PLUTOFPGA_IDCODE, &f->idcode) || read_reg(RDOF_PLUTOFPGA_DDRC, &f->ddrc) ||
        read_reg(RDOF_PLUTOFPGA_MIO16, &f->mio16)) {
        snprintf(err, cap, "/dev/mem: %s", strerror(errno));
        return false;
    }
    f->mem = mem_bytes();
    glob_t g;
    f->lvds = !glob("/sys/bus/spi/devices/*/of_node/adi,lvds-mode-enable", 0, NULL, &g);
    if (f->lvds)
        globfree(&g);
    return true;
}

const char *rdofradio_fpga_board(char *err, size_t cap)
{
    rdof_plutofpga_facts_t f;
    if (!facts(&f, err, cap))
        return NULL;
    const char *b = rdof_plutofpga_board(&f);
    if (!b)
        snprintf(err, cap, "not a board with a bitstream (IDCODE 0x%08x, DDRC 0x%08x, MIO16 0x%08x, %llu MB, %s)",
                 f.idcode, f.ddrc, f.mio16, (unsigned long long)(f.mem >> 20), f.lvds ? "LVDS" : "CMOS");
    return b;
}

/* rdof/<file> on the partition dev (its /dev name): where it's mounted, else mounted read-only for the look */
static uint8_t *from_partition(const char *dev, const char *file, size_t *n, char *where, size_t cap)
{
    char path[300], line[512];
    FILE *m = fopen("/proc/mounts", "r");
    while (m && fgets(line, sizeof line, m)) {
        char d[128], mp[256];
        if (sscanf(line, "%127s %255s", d, mp) == 2 && !strncmp(d, "/dev/", 5) && !strcmp(d + 5, dev)) {
            fclose(m);
            snprintf(path, sizeof path, "%s/rdof/%s", mp, file);
            uint8_t *b = read_file(path, n);
            if (b)
                snprintf(where, cap, "%s", path);
            return b;
        }
    }
    if (m)
        fclose(m);
    static const char *fs[] = {"vfat", "ext4", "ext2"};
    snprintf(path, sizeof path, "/dev/%s", dev);
    mkdir(MNT, 0700);
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++)
        if (!mount(path, MNT, fs[i], MS_RDONLY | MS_NOATIME, NULL)) {
            char p[300];
            snprintf(p, sizeof p, MNT "/rdof/%s", file);
            uint8_t *b = read_file(p, n);
            umount2(MNT, 0);
            rmdir(MNT);
            if (b)
                snprintf(where, cap, "rdof/%s on %s", file, path);
            return b;
        }
    rmdir(MNT);
    return NULL;
}

/* The bitstream for this board where it's found first (rdofradio_fpga.h): malloc'd, or NULL */
static uint8_t *find(const char *file, size_t *n, char *where, size_t cap)
{
    char path[300];
    snprintf(path, sizeof path, "/mnt/jffs2/rdof/%s", file);
    uint8_t *b = read_file(path, n);
    if (b) {
        snprintf(where, cap, "%s", path);
        return b;
    }
    FILE *p = fopen("/proc/partitions", "r");
    char line[256];
    while (!b && p && fgets(line, sizeof line, p)) {
        char dev[64];
        int ma, mi;
        unsigned long long blocks;
        if (sscanf(line, "%d %d %llu %63s", &ma, &mi, &blocks, dev) != 4)
            continue;
        const size_t l = strlen(dev);
        const bool mmc = !strncmp(dev, "mmcblk", 6) && strchr(dev, 'p');
        const bool usb = l >= 4 && !strncmp(dev, "sd", 2) && dev[2] >= 'a' && dev[2] <= 'z' && dev[3] >= '0' &&
                         dev[3] <= '9';
        if (mmc || usb)
            b = from_partition(dev, file, n, where, cap);
    }
    if (p)
        fclose(p);
    if (b)
        return b;
    snprintf(path, sizeof path, FIRMWARE "/%s", file);
    if ((b = read_file(path, n)))
        snprintf(where, cap, "%s", path);
    return b;
}

/* The drivers on the PL's cores (platform devices at 0x40000000 to 0xBFFFFFFF) and the AD936x's (its SPI and
   reset come through the PL): unbound before a load, bound again after, the IIO cores before the AD936x and after it */
typedef struct {
    char drv[160], dev[64];
    int order;
} bound_t;
#define MAX_BOUND 32

static int bound(bound_t *t)
{
    int n = 0;
    DIR *d = opendir("/sys/bus/platform/drivers");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char dir[160];
        snprintf(dir, sizeof dir, "/sys/bus/platform/drivers/%s", e->d_name);
        DIR *dd = opendir(dir);
        struct dirent *x;
        while (dd && (x = readdir(dd)) && n < MAX_BOUND) {
            char *dot;
            const unsigned long a = strtoul(x->d_name, &dot, 16);
            if (*dot != '.' || dot == x->d_name || a < 0x40000000ul || a >= 0xC0000000ul)
                continue;
            snprintf(t[n].drv, sizeof t[n].drv, "%s", dir);
            snprintf(t[n].dev, sizeof t[n].dev, "%s", x->d_name);
            t[n].order = !strcmp(e->d_name, "cf_axi_adc") ? 0 : !strcmp(e->d_name, "cf_axi_dds") ? 1 : 3;
            n++;
        }
        if (dd)
            closedir(dd);
    }
    if (d)
        closedir(d);
    d = opendir("/sys/bus/spi/drivers/ad9361");
    while (d && (e = readdir(d)) && n < MAX_BOUND)
        if (!strncmp(e->d_name, "spi", 3)) {
            snprintf(t[n].drv, sizeof t[n].drv, "/sys/bus/spi/drivers/ad9361");
            snprintf(t[n].dev, sizeof t[n].dev, "%s", e->d_name);
            t[n++].order = 2;
        }
    if (d)
        closedir(d);
    for (int i = 1; i < n; i++) /* by order, few */
        for (int j = i; j > 0 && t[j].order < t[j - 1].order; j--) {
            const bound_t s = t[j];
            t[j] = t[j - 1], t[j - 1] = s;
        }
    return n;
}

static void drivers(const bound_t *t, int n, bool on)
{
    char p[200];
    for (int k = 0; k < n; k++) {
        const bound_t *b = &t[on ? n - 1 - k : k];
        snprintf(p, sizeof p, "%s/%s", b->drv, on ? "bind" : "unbind");
        write_sys(p, b->dev);
    }
}

int rdofradio_fpga_load(const char *path, char *msg, size_t cap)
{
    char err[200], file[64], where[320] = "";
    const char *board = rdofradio_fpga_board(err, sizeof err);
    if (!board) {
        snprintf(msg, cap, "%s", err);
        return RDOFRADIO_FPGA_NONE;
    }
    rdof_plutofpga_file(board, file, sizeof file);
    size_t n = 0;
    uint8_t *b = NULL;
    if (path) {
        b = read_file(path, &n);
        snprintf(where, sizeof where, "%s", path);
    } else {
        b = find(file, &n, where, sizeof where);
    }
    char note[160] = "", want[160];
    FILE *f = fopen(NOTE, "r");
    if (f) {
        if (!fgets(note, sizeof note, f))
            note[0] = 0;
        fclose(f);
    }
    if (!b) {
        if (path) {
            snprintf(msg, cap, "%s: %s", path, strerror(errno));
            return RDOFRADIO_FPGA_NONE;
        }
        if (note[0]) {
            note[strcspn(note, "\n")] = 0;
            snprintf(msg, cap, "%s, loaded since boot (no %s found now)", note, file);
            return RDOFRADIO_FPGA_IN;
        }
        snprintf(msg, cap, "%s: not in /mnt/jffs2/rdof/, rdof/ on an SD card or USB stick, nor %s/", file, FIRMWARE);
        return RDOFRADIO_FPGA_NONE;
    }
    snprintf(want, sizeof want, "%s %zu %016llx\n", file, n, (unsigned long long)rdof_plutofpga_hash(b, n));
    if (!strcmp(note, want)) {
        free(b);
        snprintf(msg, cap, "%s (from %s), loaded since boot", file, where);
        return RDOFRADIO_FPGA_IN;
    }
    rdof_plutofpga_facts_t fa;
    facts(&fa, err, sizeof err);
    if (rdof_plutofpga_check(b, n, fa.idcode, err, sizeof err)) {
        free(b);
        snprintf(msg, cap, "%s: %s", where, err);
        return RDOFRADIO_FPGA_NONE;
    }
    char fw[200];
    snprintf(fw, sizeof fw, FIRMWARE "/%s", file);
    if (path || strcmp(where, fw)) { /* the FPGA manager takes a name under /lib/firmware */
        FILE *o = fopen(fw, "wb");
        bool ok = o && fwrite(b, 1, n, o) == n;
        if (o && fclose(o))
            ok = false;
        if (!ok) {
            free(b);
            snprintf(msg, cap, "%s: %s", fw, strerror(errno));
            return RDOFRADIO_FPGA_NONE;
        }
    }
    free(b);
    bound_t t[MAX_BOUND];
    const int nb = bound(t);
    drivers(t, nb, false);
    unlink(NOTE);
    const int w = write_sys(FPGA_MGR "/flags", "0") || write_sys(FPGA_MGR "/firmware", file) ? errno : 0;
    char state[64] = "";
    if ((f = fopen(FPGA_MGR "/state", "r"))) {
        if (!fgets(state, sizeof state, f))
            state[0] = 0;
        fclose(f);
    }
    state[strcspn(state, "\n")] = 0;
    drivers(t, nb, true);
    if (w || strcmp(state, "operating")) {
        snprintf(msg, cap, "%s (from %s): the FPGA manager: %s, state %s; the PL may hold nothing until a reboot",
                 file, where, w ? strerror(w) : "taken", state);
        return RDOFRADIO_FPGA_ERROR;
    }
    if ((f = fopen(NOTE, "w"))) {
        fputs(want, f);
        fclose(f);
    }
    snprintf(msg, cap, "%s loaded from %s (%zu bytes, %d drivers bound again)", file, where, n, nb);
    return RDOFRADIO_FPGA_LOADED;
}

#else

const char *rdofradio_fpga_board(char *err, size_t cap)
{
    snprintf(err, cap, "not Linux: no PlutoSDR board here");
    return NULL;
}

int rdofradio_fpga_load(const char *path, char *msg, size_t cap)
{
    (void)path;
    snprintf(msg, cap, "not Linux: no FPGA to load");
    return RDOFRADIO_FPGA_NONE;
}

#endif
