/* vim: set tabstop=8 shiftwidth=4 softtabstop=4 expandtab smarttab colorcolumn=80: */
/*
 * Copyright (c) 2015 Red Hat, Inc.
 * Author: Nathaniel McCallum <npmccallum@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "srv.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <argp.h>
#include <signal.h>
#include <sysexits.h>
#include <unistd.h>

#define LISTEN_FD_START 3

struct options {
    const char *dbdir;
};

struct addr {
    union {
        struct sockaddr_storage store;
        struct sockaddr addr;
    };
    socklen_t size;
};

static int fds;

static void
onsig(int sig)
{
    for (int i = 0; i < fds; i++)
        close(i + LISTEN_FD_START);
}

static int
req(int sock, TANG_MSG **msg, void *misc)
{
    struct addr *addr = misc;
    pkt_t pkt = {};

    addr->size = sizeof(addr->store);
    pkt.size = recvfrom(sock, pkt.data, sizeof(pkt.data), 0,
                        &addr->addr, &addr->size);
    if (pkt.size < 0)
        return EAGAIN;

    *msg = d2i_TANG_MSG(NULL, &(const uint8_t *) { pkt.data }, pkt.size);
    return *msg ? 0 : EAGAIN;
}

static int
rep(int sock, const pkt_t *pkt, void *misc)
{
    struct addr *addr = misc;
    sendto(sock, pkt->data, pkt->size, 0, &addr->addr, addr->size);
    return 0;
}

static error_t
parser(int key, char* arg, struct argp_state* state)
{
    struct options *opts = state->input;

    switch (key) {
    case 'd':
        opts->dbdir = arg;
        return 0;

    case ARGP_KEY_END:
        if (!opts->dbdir)
            opts->dbdir = TANG_DB;
        return 0;

    default:
        return ARGP_ERR_UNKNOWN;
    }
}

static int
epoll_add(int epoll, int fd)
{
    return epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &(struct epoll_event) {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
        .data.fd = fd
    });
}

static int
setup(int epoll, int family, struct sockaddr *addr, socklen_t addrlen)
{
    int sock = 0;

    sock = socket(family, SOCK_DGRAM, 0);
    if (sock < 0)
        goto error;

    if (bind(sock, addr, addrlen) != 0)
        goto error;

    if (epoll_add(epoll, sock) != 0)
        goto error;

    return sock;

error:
    close(sock);
    return -errno;
}

int
main(int argc, char *argv[])
{
    struct options opts = {};
    const struct argp argp = {
        .options = (const struct argp_option[]) {
            { "dbdir", 'd', "dir", .doc = "database directory" },
            {}
        },
        .parser = parser,
    };
    const char *lfds = NULL;
    struct addr addr = {};
    int epoll = -1;
    int sock4 = -1;
    int sock6 = -1;
    int r;

    if (argp_parse(&argp, argc, argv, 0, NULL, &opts) != 0)
        return EX_OSERR;

    epoll = epoll_create(1024);
    if (epoll < 0)
        return EX_OSERR;

    /* Setup listening sockets. */
    lfds = getenv("LISTEN_FDS");
    if (lfds) {
        errno = 0;
        fds = strtol(lfds, NULL, 10);
        if (errno != 0 || fds == 0) {
            fprintf(stderr, "Invalid LISTEN_FDS: %s\n", lfds);
            close(epoll);
            return EX_CONFIG;
        }

        for (int i = 0; i < fds; i++) {
            if (epoll_add(epoll, i + LISTEN_FD_START) != 0) {
                close(epoll);
                return EX_OSERR;
            }
        }
    } else {
        sock6 = setup(epoll, AF_INET6,
                      (struct sockaddr *) &(struct sockaddr_in6) {
                          .sin6_family = AF_INET6,
                          .sin6_port = htons(TANG_PORT),
                          .sin6_addr = IN6ADDR_ANY_INIT
                      }, sizeof(struct sockaddr_in6));
        sock4 = setup(epoll, AF_INET,
                      (struct sockaddr *) &(struct sockaddr_in) {
                          .sin_family = AF_INET,
                          .sin_port = htons(TANG_PORT),
                          .sin_addr = { .s_addr = htonl(INADDR_ANY) }
                      }, sizeof(struct sockaddr_in));
        if (sock4 < 0 && sock6 < 0) {
            close(sock4);
            close(sock6);
            return EX_OSERR;
        }
    }

    signal(SIGTERM, onsig);
    signal(SIGINT, onsig);

    r = srv_main(opts.dbdir, epoll, req, rep, &addr, -1);
    close(epoll);
    close(sock4);
    close(sock6);
    return r == 0 ? 0 : EX_IOERR;
}
