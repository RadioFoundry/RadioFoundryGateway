// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdofradio_dnssd.h"

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define DNSSD_MAX 4

static pid_t kids[DNSSD_MAX];
static int nkids;

static bool on_path(const char *prog, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s:/usr/bin:/usr/sbin:/bin", path ? path : "");
    for (char *save = NULL, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(out, cap, "%s/%s", d, prog);
        if (access(out, X_OK) == 0)
            return true;
    }
    return false;
}

void rdofdnssd_stop(void)
{
    for (int i = 0; i < nkids; i++) {
        kill(kids[i], SIGTERM);
        waitpid(kids[i], NULL, 0);
    }
    nkids = 0;
}

void rdofdnssd_publish(const char *name, const char *type, int port, const char *const *txt)
{
    if (nkids == DNSSD_MAX || port <= 0)
        return;
    char prog[512], ports[16];
    const char *argv[24];
    int n = 0;
    snprintf(ports, sizeof ports, "%d", port);
    if (on_path("avahi-publish-service", prog, sizeof prog)) {
        argv[n++] = prog, argv[n++] = name, argv[n++] = type, argv[n++] = ports;
    } else if (on_path("dns-sd", prog, sizeof prog)) { /* macOS: dns-sd -R NAME TYPE DOMAIN PORT [TXT...] */
        argv[n++] = prog, argv[n++] = "-R", argv[n++] = name, argv[n++] = type, argv[n++] = "local",
        argv[n++] = ports;
    } else {
        return;
    }
    for (int i = 0; txt && txt[i] && n < 23; i++)
        argv[n++] = txt[i];
    argv[n] = NULL;
    static bool hooked;
    if (!hooked)
        atexit(rdofdnssd_stop), hooked = true;
    const pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* gone with us, however we end */
#endif
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) /* its messages (a daemon not running, a name taken) aren't ours to show */
            dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
        execv(prog, (char *const *)argv);
        _exit(127);
    }
    kids[nkids++] = pid;
}
