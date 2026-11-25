// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "rdofradio_tun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_tun.h>
#include <net/route.h>
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *g_owner;
static int g_tun = -1, g_rip = -1;
static char g_name[IFNAMSIZ];
static rdof_ipgw_net_t g_net;
static double g_next_rip;

static double mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

#ifdef __linux__
static int open_tun(const rdof_ipgw_net_t *net)
{
    int t = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (t < 0)
        return -1;
    struct ifreq ifr = {0};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "rdof%s", "%d");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = s >= 0 && ioctl(t, TUNSETIFF, &ifr) == 0;
    int e = errno;
    if (ok) {
        snprintf(g_name, sizeof g_name, "%s", ifr.ifr_name);
        ok = ioctl(s, SIOCGIFFLAGS, &ifr) == 0;
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ok = ok && ioctl(s, SIOCSIFFLAGS, &ifr) == 0;
        e = errno;
    }
    if (ok) {
        struct rtentry rt = {0};
        struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst, *m = (struct sockaddr_in *)&rt.rt_genmask;
        d->sin_family = m->sin_family = AF_INET;
        d->sin_addr.s_addr = htonl(net->net);
        m->sin_addr.s_addr = htonl(net->mask);
        rt.rt_flags = RTF_UP;
        rt.rt_dev = g_name;
        ok = ioctl(s, SIOCADDRT, &rt) == 0 || errno == EEXIST;
        e = errno;
    }
    if (s >= 0)
        close(s);
    if (!ok) {
        close(t);
        errno = e;
        return -1;
    }
    fcntl(t, F_SETFL, fcntl(t, F_GETFL, 0) | O_NONBLOCK);
    FILE *f = fopen("/proc/sys/net/ipv4/ip_forward", "r");
    int fwd = 0;
    if (f) {
        if (fscanf(f, "%d", &fwd) != 1)
            fwd = 0;
        fclose(f);
    }
    if (!fwd)
        fprintf(stderr, "routed: net.ipv4.ip_forward is 0: only this host reaches the radios (sysctl -w "
                        "net.ipv4.ip_forward=1 for the LAN)\n");
    return t;
}
#else
static int open_tun(const rdof_ipgw_net_t *net)
{
    (void)net;
    errno = ENOTSUP;
    return -1;
}
#endif

bool rtun_open(const rdof_ipgw_net_t *net, const char *who, bool rip, char *why, size_t cap)
{
    pthread_mutex_lock(&g_mu);
    bool ok = false;
    if (g_owner) {
        snprintf(why, cap, "the %s routes radio_net", g_owner);
    } else if ((g_tun = open_tun(net)) < 0) {
        snprintf(why, cap, "no TUN device (%s; Linux, root or CAP_NET_ADMIN)", strerror(errno));
    } else {
        g_owner = who;
        g_net = *net;
        g_next_rip = 0;
        if (rip) {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            if (s >= 0)
                fcntl(s, F_SETFD, FD_CLOEXEC); /* not the DNS-SD publisher's, across an exec */
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(RDOF_IPGW_RIP_PORT),
                                    .sin_addr.s_addr = htonl(INADDR_ANY)};
            unsigned char ttl = 1;
            if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0) {
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
                g_rip = s;
            } else {
                if (s >= 0)
                    close(s);
                fprintf(stderr, "routed: no socket on port %d (root or CAP_NET_BIND_SERVICE): no RIP\n",
                        RDOF_IPGW_RIP_PORT);
            }
        }
        fprintf(stderr, "routed: radio_net on %s for the %s%s\n", g_name, who, g_rip >= 0 ? ", announced by RIP" : "");
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void rtun_close(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_tun >= 0)
        close(g_tun); /* the device and its route go with it */
    if (g_rip >= 0)
        close(g_rip);
    g_tun = g_rip = -1;
    g_owner = NULL;
    g_name[0] = 0;
    pthread_mutex_unlock(&g_mu);
}

size_t rtun_recv(uint8_t *buf, size_t cap)
{
    if (g_rip >= 0) {
        const double now = mono();
        if (now >= g_next_rip) {
            uint8_t m[24];
            const rdof_ipgw_config_t c = {.radio_net = g_net};
            const size_t n = rdof_ipgw_rip(&c, m, sizeof m);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(RDOF_IPGW_RIP_PORT),
                                    .sin_addr.s_addr = htonl(RDOF_IPGW_RIP_GROUP)};
            if (n)
                sendto(g_rip, m, n, 0, (struct sockaddr *)&a, sizeof a);
            g_next_rip = now + RDOF_IPGW_RIP_S;
        }
    }
    const ssize_t n = g_tun >= 0 ? read(g_tun, buf, cap) : -1;
    return n > 0 ? (size_t)n : 0;
}

bool rtun_send(const uint8_t *data, size_t len)
{
    return g_tun >= 0 && write(g_tun, data, len) == (ssize_t)len;
}

bool rtun_lan(rdof_ipgw_net_t *out)
{
    struct ifaddrs *all = NULL, *i;
    bool found = false;
    if (getifaddrs(&all) != 0)
        return false;
    for (i = all; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK) || (g_name[0] && !strcmp(i->ifa_name, g_name)))
            continue;
        const uint32_t ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        out->mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        out->net = ip & out->mask;
        found = true;
    }
    freeifaddrs(all);
    return found;
}
