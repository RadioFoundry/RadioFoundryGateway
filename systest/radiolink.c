// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "radiolink.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (n < cap && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
        out[n++] = (uint8_t)((hexval(s[0]) << 4) | hexval(s[1]));
        s += 2;
    }
    return n;
}

bool radiolink_start(radiolink_t *l, const char *path, const char *description)
{
    int in[2], out[2];
    if (pipe(in) || pipe(out))
        return false;
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        if (description)
            execl(path, path, description, (char *)NULL);
        else
            execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    l->pid = pid;
    l->to = in[1];
    l->from = out[0];
    l->have = 0;
    l->now = 1000;
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void radiolink_stop(radiolink_t *l)
{
    if (l->pid <= 0)
        return;
    if (write(l->to, "q\n", 2) < 0) {
    }
    close(l->to);
    close(l->from);
    waitpid(l->pid, NULL, 0);
    l->pid = 0;
}

static void take_line(radiolink_t *l, char *s)
{
    if (s[0] == 'o' && l->on_frame) {
        char *p;
        uint32_t ms = (uint32_t)strtoul(s + 2, &p, 10);
        uint8_t f[600];
        size_t n = unhex(p + 1, f, sizeof f);
        l->on_frame(l->ctx, ms, f, n);
    } else if (s[0] == 'a' && l->on_air) {
        radiolink_air_t a;
        char emb[16] = {0}, pay[64] = {0};
        unsigned ms, slot;
        memset(&a, 0, sizeof a);
        if (sscanf(s + 2, "%u %u %d %d %d %d %15s %63s", &ms, &slot, &a.kind, &a.index, &a.sync, &a.lcss, emb, pay) ==
            8) {
            a.ms = ms;
            a.slot = slot;
            unhex(emb, a.emb, sizeof a.emb);
            a.len = unhex(pay, a.payload, sizeof a.payload);
            l->on_air(l->ctx, &a);
        }
    } else if (s[0] == 'e' && l->on_event) {
        char *p;
        uint32_t ms = (uint32_t)strtoul(s + 2, &p, 10);
        l->on_event(l->ctx, ms, p + 1);
    }
}

bool radiolink_request(radiolink_t *l, const char *line)
{
    size_t n = strlen(line);
    if (write(l->to, line, n) != (ssize_t)n || write(l->to, "\n", 1) != 1)
        return false;
    for (;;) {
        char *nl;
        while ((nl = memchr(l->buf, '\n', l->have)) != NULL) {
            *nl = '\0';
            bool done = l->buf[0] == '.' && l->buf[1] == '\0';
            if (!done)
                take_line(l, l->buf);
            size_t used = (size_t)(nl - l->buf) + 1;
            memmove(l->buf, nl + 1, l->have - used);
            l->have -= used;
            if (done)
                return true;
        }
        if (l->have == sizeof l->buf)
            return false; /* a line longer than the buffer */
        ssize_t got = read(l->from, l->buf + l->have, sizeof l->buf - l->have);
        if (got <= 0)
            return false;
        l->have += (size_t)got;
    }
}

bool radiolink_host(radiolink_t *l, const uint8_t *data, size_t len)
{
    char line[2 + 2 * 512 + 1];
    if (len > 512)
        return false;
    line[0] = 'h';
    line[1] = ' ';
    for (size_t i = 0; i < len; i++)
        snprintf(line + 2 + 2 * i, 3, "%02X", data[i]);
    line[2 + 2 * len] = '\0';
    return radiolink_request(l, line);
}

bool radiolink_advance(radiolink_t *l, uint32_t ms)
{
    char line[32];
    l->now += ms;
    snprintf(line, sizeof line, "t %u", (unsigned)l->now);
    return radiolink_request(l, line);
}
