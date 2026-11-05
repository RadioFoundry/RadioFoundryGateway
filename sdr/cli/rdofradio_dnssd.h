// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DNS-SD for the host tools: a service listed over mDNS by the host's own responder, through its command-line
 * publisher (avahi-publish-service where avahi runs, as on a PlutoSDR; dns-sd on macOS), which runs while the
 * service does. None of them: nothing listed, quietly. The bench line's is what `rfgbench find` looks for.
 */
#ifndef RDOFRADIO_DNSSD_H
#define RDOFRADIO_DNSSD_H

/* name: the instance ("pluto"); type: "_rfgbench._tcp"; txt: "key=value" items, NULL-terminated (or NULL). */
void rdofdnssd_publish(const char *name, const char *type, int port, const char *const *txt);
/* Every service this process listed, taken down (also at exit, and when the process dies on Linux). */
void rdofdnssd_stop(void);

#endif
