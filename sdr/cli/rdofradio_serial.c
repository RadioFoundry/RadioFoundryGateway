// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio_serial.h: a raw serial port through termios.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE /* macOS: the speeds over 38400 */
#define _DEFAULT_SOURCE  /* glibc: the same */
#include "rdofradio_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static speed_t speed_of(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return 0;
    }
}

int rdofradio_serial_open(const char *path, int baud, char *err, size_t cap)
{
    const speed_t sp = speed_of(baud);
    if (!sp) {
        snprintf(err, cap, "%s: %d baud isn't one this takes (9600 to 230400, 460800, 921600)", path, baud);
        return -1;
    }
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, cap, "%s: %s", path, strerror(errno));
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t)) {
        snprintf(err, cap, "%s: not a serial port (%s)", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* raw, as cfmakeraw (not in POSIX) */
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB);
    t.c_cflag |= CS8 | CREAD | CLOCAL;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t)) {
        snprintf(err, cap, "%s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int rdofradio_serial_read(int fd, uint8_t *buf, size_t cap)
{
    const ssize_t n = read(fd, buf, cap);
    if (n > 0)
        return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return n == 0 ? 0 : -1;
}

bool rdofradio_serial_write(int fd, const uint8_t *data, size_t len)
{
    int waits = 0;
    while (len) {
        const ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += n, len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return false;
        struct pollfd p = {fd, POLLOUT, 0};
        if (++waits > 10 || poll(&p, 1, 20) < 0)
            return false;
    }
    return true;
}
