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

#include "../src/conv.h"
#include "../src/clt/adv.h"
#include "../src/clt/msg.h"
#include "../src/srv/db.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <errno.h>
#include <error.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#define SD_LISTEN_FDS_START 3
#define KEYD_BIN "../src/srv/tang-keyd"
#define KEY_GEN_BIN "../src/srv/tang-key-gen"

#define test(cond, cmd) \
    if (!(cond)) { \
        fprintf(stderr, "Error: %s:%d: %s\n", \
                __FILE__, __LINE__, strerror(errno)); \
        cmd; \
    }
#define testg(cond, label) test(cond, goto label)
#define teste(cond) test(cond, exit(EXIT_FAILURE))
#define testb(cond) test(cond, return false)

static char tempdir[] = "/tmp/tangXXXXXX";
static pid_t pid;

static EC_KEY *
keygen(const char *grpname, const char *name, db_use_t use, bool adv)
{
    char path[PATH_MAX] = {};
    EC_GROUP *grp = NULL;
    EC_KEY *key = NULL;
    FILE *file = NULL;
    int nid;

    nid = OBJ_txt2nid(grpname);
    if (nid == NID_undef)
        goto error;

    grp = EC_GROUP_new_by_curve_name(nid);
    if (!grp)
        goto error;

    EC_GROUP_set_asn1_flag(grp, OPENSSL_EC_NAMED_CURVE);
    EC_GROUP_set_point_conversion_form(grp, POINT_CONVERSION_COMPRESSED);

    key = EC_KEY_new();
    if (!key)
        goto error;

    if (EC_KEY_set_group(key, grp) <= 0)
        goto error;

    if (EC_KEY_generate_key(key) <= 0)
        goto error;

    if (strlen(tempdir) + strlen(name) + 7 > sizeof(path))
        goto error;

    strcpy(path, tempdir);
    strcat(path, "/");

    if (!adv)
        strcat(path, ".");

    strcat(path, name);

    switch (use) {
    case db_use_sig: strcat(path, ".sig"); break;
    case db_use_rec: strcat(path, ".rec"); break;
    default: goto error;
    }

    file = fopen(path, "wx");
    if (!file)
        goto error;

    if (PEM_write_ECPKParameters(file, grp) <= 0)
        goto error;

    if (PEM_write_ECPrivateKey(file, key, NULL, NULL, 0, NULL, NULL) <= 0)
        goto error;

    EC_GROUP_free(grp);
    fclose(file);
    return key;

error:
    EC_GROUP_free(grp);
    EC_KEY_free(key);
    fclose(file);
    return NULL;
}

static void
onexit(void)
{
    const char *cmd = "rm -rf ";
    char tmp[strlen(cmd) + strlen(tempdir) + 1];
    __attribute__((unused)) int r = 0;

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    strcpy(tmp, cmd);
    strcat(tmp, tempdir);
    r = system(tmp);
}

static int
makebind(int af, const msg_t *p)
{
    const struct addrinfo hnt = { .ai_family = af, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *addrs = NULL;
    int sock = -1;

    if (getaddrinfo(p->hostname, p->service, &hnt, &addrs) != 0)
        return -1;

    sock = socket(af, SOCK_DGRAM, 0);
    if (sock < 0) {
        freeaddrinfo(addrs);
        return -1;
    }

    if (bind(sock, addrs->ai_addr, addrs->ai_addrlen) != 0) {
        freeaddrinfo(addrs);
        close(sock);
        return -1;
    }

    freeaddrinfo(addrs);
    return sock;
}

static TANG_MSG *
adv(const msg_t *params, BN_CTX *ctx, ...)
{
    TANG_KEY *tkey = NULL;
    TANG_MSG *rep = NULL;
    TANG_MSG req = {};
    va_list ap;

    req.type = TANG_MSG_TYPE_ADV_REQ;
    req.val.adv.req = TANG_MSG_ADV_REQ_new();
    testg(req.val.adv.req, error);

    va_start(ap, ctx);
    for (EC_KEY *key = va_arg(ap, EC_KEY *); key; key = va_arg(ap, EC_KEY *)) {
        tkey = TANG_KEY_new();
        testg(tkey, error);
        testg(conv_eckey2tkey(key, tkey, ctx) == 0, error);
        testg(SKM_sk_push(TANG_KEY, req.val.adv.req->keys, tkey) > 0, error);
        tkey = NULL;
    }
    va_end(ap);

    rep = msg_rqst(params, &req);

error:
    TANG_MSG_ADV_REQ_free(req.val.adv.req);
    TANG_KEY_free(tkey);
    return rep;
}

static bool
key_exists(const STACK_OF(TANG_KEY) *keys, const EC_KEY *key, BN_CTX *ctx)
{
    TANG_KEY *tkey = NULL;
    bool found = false;
    int num = 0;

    tkey = TANG_KEY_new();
    testg(tkey, egress);

    testg(conv_eckey2tkey(key, tkey, ctx) == 0, egress);

    num = SKM_sk_num(TANG_KEY, keys);
    for (int i = 0; !found && i < num; i++) {
        TANG_KEY *t = SKM_sk_value(TANG_KEY, keys, i);
        found |= TANG_KEY_equals(tkey, t);
    }

egress:
    TANG_KEY_free(tkey);
    return found;
}

static bool
adv_verify(TANG_MSG *rep, BN_CTX *ctx, ...)
{
    TANG_KEY *tkey = NULL;
    bool success = false;
    int cnt = 0;
    va_list ap;

    testg(rep, egress);
    testg(rep->type == TANG_MSG_TYPE_ADV_REP, egress);

    va_start(ap, ctx);
    cnt = 0;
    for (EC_KEY *key = va_arg(ap, EC_KEY *); key; key = va_arg(ap, EC_KEY *)) {
        testg(key_exists(rep->val.adv.rep->body->sigs, key, ctx), egress);
        cnt++;
    }
    testg(SKM_sk_num(TANG_KEY, rep->val.adv.rep->body->sigs) == cnt, egress);

    cnt = 0;
    for (EC_KEY *key = va_arg(ap, EC_KEY *); key; key = va_arg(ap, EC_KEY *)) {
        testg(key_exists(rep->val.adv.rep->body->recs, key, ctx), egress);
        cnt++;
    }
    testg(SKM_sk_num(TANG_KEY, rep->val.adv.rep->body->recs) == cnt, egress);

    cnt = 0;
    for (EC_KEY *key = va_arg(ap, EC_KEY *); key; key = va_arg(ap, EC_KEY *)) {
        testg(adv_signed_by(rep->val.adv.rep, key, ctx), egress);
        cnt++;
    }
    testg(SKM_sk_num(TANG_SIG, rep->val.adv.rep->sigs) == cnt, egress);
    va_end(ap);


    success = true;

egress:
    TANG_KEY_free(tkey);
    TANG_MSG_free(rep);
    return success;
}

static TANG_MSG *
rec(const msg_t *params, BN_CTX *ctx, const EC_KEY *key)
{
    TANG_MSG_REC_REQ *req = NULL;
    const EC_GROUP *grp = NULL;
    TANG_MSG *rep = NULL;

    grp = EC_KEY_get0_group(key);
    testg(grp, error);

    req = TANG_MSG_REC_REQ_new();
    testg(req, error);

    testg(conv_eckey2tkey(key, req->key, ctx) == 0, error);
    testg(conv_point2os(grp, EC_GROUP_get0_generator(grp),
                        req->x, ctx) == 0, error);

    rep = msg_rqst(params, &(TANG_MSG) {
                               .type = TANG_MSG_TYPE_REC_REQ,
                               .val.rec.req = req
                           });

error:
    TANG_MSG_REC_REQ_free(req);
    return rep;
}

static bool
rec_verify(TANG_MSG *rep, const EC_KEY *key, BN_CTX *ctx)
{
    const EC_GROUP *grp = NULL;
    bool success = false;
    EC_POINT *p = NULL;

    testg(rep, error);
    testg(rep->type == TANG_MSG_TYPE_REC_REP, error);

    grp = EC_KEY_get0_group(key);
    testg(grp, error);

    p = EC_POINT_new(grp);
    testg(p, error);

    testg(conv_os2point(grp, rep->val.rec.rep->y, p, ctx) == 0, error);

    success = EC_POINT_cmp(grp, EC_KEY_get0_public_key(key), p, ctx) == 0;

error:
    TANG_MSG_free(rep);
    EC_POINT_free(p);
    return success;
}

/* Before any keys exist on the server. */
static bool
stage0(const msg_t *params, BN_CTX *ctx)
{
    TANG_MSG *rep = NULL;

    /* Make sure we get an empty advertisement when no keys exist. */
    rep = adv(params, ctx, NULL);
    testb(adv_verify(rep, ctx, NULL, NULL, NULL));

    return true;
}

/* When only unadvertised keys exist on the server. */
static bool
stage1(const msg_t *params, EC_KEY *reca, EC_KEY *siga, BN_CTX *ctx)
{
    TANG_MSG *rep = NULL;

    /* Make sure the unadvertised keys aren't advertised. */
    rep = adv(params, ctx, NULL);
    testb(adv_verify(rep, ctx, NULL, NULL, NULL));

    /* Make sure the server won't sign with recovery keys. */
    rep = adv(params, ctx, reca, NULL);
    testb(adv_verify(rep, ctx, NULL, NULL, NULL));

    /* Request signature with a valid, unadvertised key. */
    rep = adv(params, ctx, siga, NULL);
    testb(adv_verify(rep, ctx, NULL, NULL, siga, NULL));

    /* Test recovery of an unadvertised key. */
    rep = rec(params, ctx, reca);
    testb(rec_verify(rep, reca, ctx));

    /* Test recovery using an unadvertised signature key. */
    rep = rec(params, ctx, siga);
    testb(rep);
    testb(rep->type == TANG_MSG_TYPE_ERR);
    testb(ASN1_ENUMERATED_get(rep->val.err) == TANG_MSG_ERR_NOTFOUND_KEY);
    TANG_MSG_free(rep);

    return true;
}

/* When advertised and unadvertised keys exist on the server. */
static bool
stage2(const msg_t *params, EC_KEY *reca, EC_KEY *siga,
       EC_KEY *recA, EC_KEY *sigA, BN_CTX *ctx)
{
    TANG_MSG *rep = NULL;

    /* Make sure the unadvertised keys aren't advertised. */
    rep = adv(params, ctx, NULL);
    testb(adv_verify(rep, ctx, sigA, NULL, recA, NULL, sigA, NULL));

    /* Make sure the server won't sign with recovery keys. */
    rep = adv(params, ctx, reca, NULL);
    testb(adv_verify(rep, ctx, sigA, NULL, recA, NULL, sigA, NULL));
    rep = adv(params, ctx, recA, NULL);
    testb(adv_verify(rep, ctx, sigA, NULL, recA, NULL, sigA, NULL));

    /* Request signature with a valid, unadvertised key. */
    rep = adv(params, ctx, siga, NULL);
    testb(adv_verify(rep, ctx, sigA, NULL, recA, NULL, sigA, siga, NULL));

    /* Request signature with a valid, advertised key. */
    rep = adv(params, ctx, sigA, NULL);
    testb(adv_verify(rep, ctx, sigA, NULL, recA, NULL, sigA, NULL));

    /* Test recovery of an unadvertised key. */
    rep = rec(params, ctx, reca);
    testb(rec_verify(rep, reca, ctx));

    /* Test recovery of an advertised key. */
    rep = rec(params, ctx, recA);
    testb(rec_verify(rep, recA, ctx));

    /* Test recovery using an unadvertised signature key. */
    rep = rec(params, ctx, siga);
    testb(rep);
    testb(rep->type == TANG_MSG_TYPE_ERR);
    testb(ASN1_ENUMERATED_get(rep->val.err) == TANG_MSG_ERR_NOTFOUND_KEY);
    TANG_MSG_free(rep);

    /* Test recovery using an advertised signature key. */
    rep = rec(params, ctx, sigA);
    testb(rep);
    testb(rep->type == TANG_MSG_TYPE_ERR);
    testb(ASN1_ENUMERATED_get(rep->val.err) == TANG_MSG_ERR_NOTFOUND_KEY);
    TANG_MSG_free(rep);

    return true;
}

int
main(int argc, char *argv[])
{
    msg_t ipv4 = { .hostname = "127.0.0.1", .service = "5700", .timeout = 1 };
    msg_t ipv6 = { .hostname = "::1", .service = "5700", .timeout = 1 };
    EC_KEY *reca = NULL;
    EC_KEY *recA = NULL;
    EC_KEY *siga = NULL;
    EC_KEY *sigA = NULL;
    BN_CTX *ctx = NULL;
    int sock4 = -1;
    int sock6 = -1;

    ctx = BN_CTX_new();
    teste(ctx);

    if (!mkdtemp(tempdir))
        error(EXIT_FAILURE, errno, "Error calling mkdtemp()");

    sock4 = makebind(AF_INET, &ipv4);
    teste(sock4 >= 0);

    sock6 = makebind(AF_INET6, &ipv6);
    /* Don't fail on error. This is optional. */

    pid = fork();
    if (pid < 0)
        error(EXIT_FAILURE, errno, "Error calling fork()");

    if (pid == 0) {
        const int fd = SD_LISTEN_FDS_START;

        teste(setenv("LISTEN_FDS", sock6 >= 0 ? "2" : "1", true) == 0);

        if (sock4 != fd) {
            teste(dup2(sock4, fd) == fd);
            close(sock4);
        }

        if (sock6 != fd + 1 && sock6 >= 0) {
            teste(dup2(sock6, fd + 1) == fd + 1);
            close(sock6);
        }

        execlp(KEYD_BIN, KEYD_BIN, "-d", tempdir, NULL);
        teste(false);
    }

    atexit(onexit);
    close(sock4);
    if (sock6 >= 0)
        close(sock6);

    teste(stage0(&ipv4, ctx));
    if (sock6 >= 0)
        teste(stage0(&ipv6, ctx));

    /* Make some unadvertised keys. */
    reca = keygen("secp384r1", "reca", db_use_rec, false);
    siga = keygen("secp384r1", "siga", db_use_sig, false);
    teste(reca);
    teste(siga);
    usleep(100000); /* Let the daemon have time to pick up the new files. */

    teste(stage1(&ipv4, reca, siga, ctx))
    if (sock6 >= 0)
        teste(stage1(&ipv6, reca, siga, ctx));

    /* Make some advertised keys. */
    recA = keygen("secp384r1", "recA", db_use_rec, true);
    sigA = keygen("secp384r1", "sigA", db_use_sig, true);
    teste(recA);
    teste(sigA);
    usleep(100000); /* Let the daemon have time to pick up the new files. */

    teste(stage2(&ipv4, reca, siga, recA, sigA, ctx));
    if (sock6 >= 0)
        teste(stage2(&ipv6, reca, siga, recA, sigA, ctx));

    EC_KEY_free(reca);
    EC_KEY_free(recA);
    EC_KEY_free(siga);
    EC_KEY_free(sigA);
    BN_CTX_free(ctx);
    return 0;
}
