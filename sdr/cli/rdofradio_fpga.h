// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's FPGA bitstream on a PlutoSDR running its own firmware (rdof/sdr/plutofpga.h): the board, and the
 * bitstream with the channelizer and the TX interpolator loaded through the FPGA manager in place of the stock
 * one, which the board's boot loaded (a reboot brings it back). Linux only; elsewhere there is no board.
 *
 * The bitstream, rfg-pluto-<board>.bit.bin, is looked for in turn:
 *   the file given (--fpga-bit), and only there
 *   /mnt/jffs2/rdof/
 *   rdof/ on every partition of an SD card or a USB stick (mmcblkNpM, sdXN): where it's mounted, else mounted
 *   read-only for the look and unmounted
 *   /lib/firmware/ (RAM: put there by hand)
 * The first found is checked against the board's Zynq and loaded: the drivers on the PL's cores unbound, the file
 * to /lib/firmware, its name to the FPGA manager, the drivers bound again. /tmp/rdof-fpga notes the one loaded (RAM,
 * like the PL: gone at the next boot), so a restart with the same file loads nothing.
 */
#ifndef RDOFRADIO_FPGA_H
#define RDOFRADIO_FPGA_H

#include <stddef.h>

#define RDOFRADIO_FPGA_LOADED 1  /* loaded now */
#define RDOFRADIO_FPGA_IN 0      /* already in (the note matches) */
#define RDOFRADIO_FPGA_NONE (-2) /* not loaded: no board known, no file for it, or not Linux; the stock design stays */
#define RDOFRADIO_FPGA_ERROR (-1) /* the load failed: the PL may hold nothing until a reboot */

/* The board's name (plutofpga.h), or NULL with why in err */
const char *rdofradio_fpga_board(char *err, size_t cap);
/* The bitstream in (path: --fpga-bit, or NULL to look). RDOFRADIO_FPGA_*, with what happened in msg (where it was
 * found, why none was) */
int rdofradio_fpga_load(const char *path, char *msg, size_t cap);

#endif
