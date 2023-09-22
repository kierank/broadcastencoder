/*****************************************************************************
 * srt.c: rtp retransmission
 *****************************************************************************
 * Copyright (C) 2018 Open Broadcast Systems Ltd.
 *
 * Authors: Rafaël Carré <funman@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/


#include "common/common.h"
#include "output/output.h"
#include "common/bitstream.h"

#include <upipe/uprobe.h>
#include <upipe/uprobe_helper_uprobe.h>
#include <upipe/uprobe_helper_alloc.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/uprobe_uclock.h>
#include <upipe/uprobe_dejitter.h>
#include <upipe/umem.h>
#include <upipe/umem_pool.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/uref.h>
#include <upipe/uref_std.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/ubuf_block_mem.h>
#include <upipe/uclock.h>
#include <upipe/uclock_std.h>
#include <upipe/upipe.h>
#include <upipe/uqueue.h>
#include <upipe/ueventfd.h>
#include <upipe/upump.h>

#include <upump-ev/upump_ev.h>

#include <upipe-modules/upipe_setflowdef.h>
#include <upipe-modules/upipe_probe_uref.h>
#include <upipe-modules/upipe_udp_source.h>
#include <upipe-modules/upipe_udp_sink.h>
#include <upipe-modules/upipe_dup.h>

#include <upipe-srt/upipe_srt_sender.h>
#include <upipe-srt/upipe_srt_handshake.h>


#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "srt.h"
#include "udp.h"

#define UMEM_POOL               512
#define UDICT_POOL_DEPTH        500
#define UREF_POOL_DEPTH         500
#define UBUF_POOL_DEPTH         3000
#define UBUF_SHARED_POOL_DEPTH  50
#define UPUMP_POOL              10
#define UPUMP_BLOCKER_POOL      10
#define XFER_QUEUE              255
#define XFER_POOL               20

static const enum uprobe_log_level loglevel = UPROBE_LOG_DEBUG;

static void catch_event(struct upump *upump)
{
    struct srt_ctx *ctx = upump->opaque;
    ueventfd_read(ctx->event);
    pthread_mutex_lock(&ctx->mutex);
    bool end = ctx->end;
    pthread_mutex_unlock(&ctx->mutex);

    if (end) {
        upipe_release(ctx->upipe_udpsrc_srt);
        upipe_release(ctx->upipe_setflowdef);

        upump_stop(upump);
        return;
    }

    for (;;) {
        struct uref *uref = uqueue_pop(ctx->queue, struct uref *);
        if (!uref)
            break;

        upipe_input(ctx->upipe_setflowdef, uref, &upump);
    }
}

static void addr_to_str(const struct sockaddr *s, char uri[INET6_ADDRSTRLEN+6])
{
    uint16_t port = 0;
    switch(s->sa_family) {
    case AF_INET: {
        struct sockaddr_in *in = (struct sockaddr_in *)s;
        inet_ntop(AF_INET, &in->sin_addr, uri, INET6_ADDRSTRLEN);
        port = ntohs(in->sin_port);
        break;
    }
    case AF_INET6: {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)s;
        inet_ntop(AF_INET6, &in6->sin6_addr, uri, INET6_ADDRSTRLEN);
        port = ntohs(in6->sin6_port);
        break;
    }
    default:
        uri[0] = '\0';
    }

    size_t uri_len = strlen(uri);
    sprintf(&uri[uri_len], ":%hu", port);
}

static int start(struct srt_ctx *ctx)
{
    bool listener = false; //dirpath && *dirpath == '@';

    struct upipe_mgr *upipe_udpsrc_mgr = upipe_udpsrc_mgr_alloc();


    struct upipe_mgr *upipe_setflowdef_mgr = upipe_setflowdef_mgr_alloc();
    struct upipe *upipe_setflowdef = upipe_void_alloc(upipe_setflowdef_mgr,
            uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "setflowdef"));
    upipe_mgr_release(upipe_setflowdef_mgr);

    struct uref *flow_def = uref_block_flow_alloc_def(ctx->uref_ctx->uref_mgr, "");
    upipe_set_flow_def(upipe_setflowdef, flow_def);
    upipe_setflowdef_set_dict(upipe_setflowdef, flow_def);
    uref_free(flow_def);

    ctx->upipe_setflowdef = upipe_setflowdef;

    /* send through srt sender */
    struct upipe_mgr *upipe_srt_sender_mgr = upipe_srt_sender_mgr_alloc();
    struct upipe *upipe_srt_sender = upipe_void_alloc_output(upipe_setflowdef, upipe_srt_sender_mgr,
            uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "srt sender"));
    upipe_mgr_release(upipe_srt_sender_mgr);

    char lat[12];
    snprintf(lat, sizeof(lat) - 1, "%u", ctx->latency);
    lat[sizeof(lat)-1] = '\0';
    if (!ubase_check(upipe_set_option(upipe_srt_sender, "latency", lat)))
        return EXIT_FAILURE;

    ctx->upipe_udpsrc_srt = upipe_void_alloc(upipe_udpsrc_mgr, uprobe_use(ctx->uprobe_udp_srt));
    upipe_attach_uclock(ctx->upipe_udpsrc_srt);

    struct upipe_mgr *upipe_srt_handshake_mgr = upipe_srt_handshake_mgr_alloc();
    struct upipe *upipe_srt_handshake = upipe_void_alloc_output(ctx->upipe_udpsrc_srt, upipe_srt_handshake_mgr,
            uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "srt handshake"));
    upipe_set_option(upipe_srt_handshake, "listener", listener ? "1" : "0");
    if (ctx->password)
        upipe_srt_handshake_set_password(upipe_srt_handshake, ctx->password);

    upipe_mgr_release(upipe_srt_handshake_mgr);

    upipe_mgr_release(upipe_udpsrc_mgr);

    struct upipe *upipe_srt_handshake_sub = upipe_void_alloc_sub(upipe_srt_handshake,
        uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "srt handshake sub"));
    assert(upipe_srt_handshake_sub);

    struct upipe *upipe_srt_sender_sub = upipe_void_chain_output_sub(upipe_srt_handshake,
        upipe_srt_sender,
        uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "srt sender sub"));
    assert(upipe_srt_sender_sub);
    upipe_release(upipe_srt_sender_sub);

    /* send to udp */
    struct upipe_mgr *upipe_udpsink_mgr = upipe_udpsink_mgr_alloc();
    ctx->upipe_udpsink = upipe_void_chain_output(upipe_srt_sender, upipe_udpsink_mgr,
            uprobe_pfx_alloc(uprobe_use(ctx->logger), loglevel, "udp sink"));
    upipe_release(ctx->upipe_udpsink);

    upipe_set_output(upipe_srt_handshake_sub, ctx->upipe_udpsink);

    if (listener) {
        // TODO
        #if 0
        if (!ubase_check(upipe_set_uri(ctx->upipe_udpsrc_srt, dirpath))) {
            return EXIT_FAILURE;
        }
        ubase_assert(upipe_udpsrc_get_fd(ctx->upipe_udpsrc_srt, &udp_fd));
        #endif
    } else {
        ubase_assert(upipe_udpsink_set_fd(ctx->upipe_udpsink, ctx->fd));

        int flags = fcntl(ctx->fd, F_GETFL);
        flags |= O_NONBLOCK;
        if (fcntl(ctx->fd, F_SETFL, flags) < 0)
            upipe_err(ctx->upipe_udpsink, "Could not set flags");;

        ubase_assert(upipe_udpsrc_set_fd(ctx->upipe_udpsrc_srt, ctx->fd));
        ubase_assert(upipe_udpsink_set_peer(ctx->upipe_udpsink,
                    (const struct sockaddr*)&ctx->dest_addr, ctx->dest_addr_len));
    }

    struct sockaddr_storage ad;
    socklen_t peer_len = sizeof(ad);
    struct sockaddr *peer = (struct sockaddr*) &ad;

    if (!getsockname(ctx->fd, peer, &peer_len)) {
        char uri[INET6_ADDRSTRLEN+6];
        addr_to_str(peer, uri);
        upipe_warn_va(upipe_srt_handshake, "Local %s", uri); // XXX: INADDR_ANY when listening
        upipe_srt_handshake_set_peer(upipe_srt_handshake, peer, peer_len);
    }

    return 0;
}

static void stop(struct upump *upump)
{
    struct srt_ctx *ctx = upump_get_opaque(upump, struct srt_ctx*);

    if (upump) {
        upump_stop(upump);
        upump_free(upump);
    }

    upipe_release(ctx->upipe_udpsrc_srt);
    upipe_release(ctx->upipe_setflowdef);

    if (ctx->restart) {
        ctx->restart = false;
        start(ctx);
    }
}

/** @This is the private context of an obe probe */
struct uprobe_srt {
    struct uprobe probe;
    void *data;
};

UPROBE_HELPER_UPROBE(uprobe_srt, probe);

static struct uprobe *uprobe_srt_init(struct uprobe_srt *probe_srt,
                                      struct uprobe *next, uprobe_throw_func catch, void *data)
{
    struct uprobe *probe = uprobe_srt_to_uprobe(probe_srt);
    uprobe_init(probe, catch, next);
    probe_srt->data = data;
    return probe;
}

static void uprobe_srt_clean(struct uprobe_srt *probe_srt)
{
    uprobe_clean(uprobe_srt_to_uprobe(probe_srt));
}

#define ARGS_DECL struct uprobe *next, uprobe_throw_func catch, void *data
#define ARGS next, catch, data
UPROBE_HELPER_ALLOC(uprobe_srt)
#undef ARGS
#undef ARGS_DECL

static int catch_udp(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    struct uprobe_srt *uprobe_srt = uprobe_srt_from_uprobe(uprobe);
    struct srt_ctx *ctx = uprobe_srt->data; // XXX

    switch (event) {
    case UPROBE_SOURCE_END:
        upipe_warn(upipe, "Remote end not listening, can't receive SRT");
        ctx->restart = true;
        struct upump *u = upump_alloc_timer(ctx->upump_mgr, stop, ctx,
                NULL, UCLOCK_FREQ, 0);
        upump_start(u);
        return uprobe_throw_next(uprobe, upipe, event, args);
    case UPROBE_UDPSRC_NEW_PEER: {
        int udp_fd;
        int sig = va_arg(args, int);
        if (sig != UPIPE_UDPSRC_SIGNATURE)
            break;

        const struct sockaddr *s = va_arg(args, struct sockaddr*);
        const socklen_t *len = va_arg(args, socklen_t *);

        char uri[INET6_ADDRSTRLEN+6];
        addr_to_str(s, uri);
        upipe_warn_va(upipe, "Remote %s", uri);

        ubase_assert(upipe_udpsrc_get_fd(ctx->upipe_udpsrc_srt, &udp_fd));
        ubase_assert(upipe_udpsink_set_fd(ctx->upipe_udpsink, dup(udp_fd)));

        ubase_assert(upipe_udpsink_set_peer(ctx->upipe_udpsink, s, *len));

        return UBASE_ERR_NONE;
    }
    }

    return uprobe_throw_next(uprobe, upipe, event, args);
}

static int catch_srt(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    struct uref *uref = NULL;
    struct uprobe_srt *uprobe_srt = uprobe_srt_from_uprobe(uprobe);
    struct srt_ctx *ctx = uprobe_srt->data;

    switch (event) {
    case UPROBE_SOURCE_END:
        upipe_release(upipe);
        break;

    case UPROBE_PROBE_UREF: {
        int sig = va_arg(args, int);
        if (sig != UPIPE_PROBE_UREF_SIGNATURE)
            return UBASE_ERR_INVALID;
        uref = va_arg(args, struct uref *);
        va_arg(args, struct upump **);
        va_arg(args, bool *);

        const uint8_t *buf;
        int s = -1;
        if (!ubase_check(uref_block_read(uref, 0, &s, &buf)))
            return UBASE_ERR_INVALID;

        uint64_t cr_sys;
        if (!ubase_check(uref_clock_get_cr_sys(uref, &cr_sys)))
            cr_sys = UINT64_MAX;

        // TODO
        //parse_rtcp(ctx, upipe, buf, s, cr_sys, uref->ubuf->mgr);

        uref_block_unmap(uref, 0);
        break;
    }
    default:
        return uprobe_throw_next(uprobe, upipe, event, args);
    }
    return UBASE_ERR_NONE;
}

void srt_write(struct srt_ctx *ctx, struct uref *uref)
{
    if (!uqueue_push(ctx->queue, uref)) {
        printf("full\n");
        uref_free(uref);
    } else {
        ueventfd_write(ctx->event);
    }
}

static void *srt_thread(void *arg)
{
    struct srt_ctx *ctx = arg;

    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    ctx->uref_ctx->uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    ctx->upump_mgr = upump_ev_mgr_alloc_loop(UPUMP_POOL,
                                                     UPUMP_BLOCKER_POOL);
    ctx->uref_ctx->uclock = uclock_std_alloc(UCLOCK_FLAG_REALTIME);
    struct uprobe *uprobe = uprobe_srt_alloc(NULL, catch_srt, ctx);
    struct uprobe *logger = uprobe_stdio_alloc(uprobe_use(uprobe), stdout, loglevel);
    assert(logger != NULL);
    struct uprobe *uprobe_dejitter = uprobe_dejitter_alloc(logger, true, 0);
    assert(uprobe_dejitter != NULL);

    logger = uprobe_uref_mgr_alloc(uprobe_dejitter, ctx->uref_ctx->uref_mgr);

    assert(logger != NULL);
    logger = uprobe_upump_mgr_alloc(logger, ctx->upump_mgr);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH,
                                   UBUF_POOL_DEPTH);
    assert(logger != NULL);

    logger = uprobe_uclock_alloc(logger, ctx->uref_ctx->uclock);
    assert(logger != NULL);

    ctx->logger = logger;
    ctx->uprobe_udp_srt = uprobe_srt_alloc(uprobe_pfx_alloc(uprobe_use(logger), loglevel, "udp source srt"), catch_udp, ctx);

    start(ctx);

    /* */

    struct upump *event = ueventfd_upump_alloc(ctx->event, ctx->upump_mgr,
            catch_event, ctx, NULL);
    upump_start(event);

    ctx->uref_ctx->ubuf_mgr = ubuf_block_mem_mgr_alloc(255, 255, umem_mgr, 0, 0, 0, 0);
    assert(ctx->uref_ctx->ubuf_mgr);

    /* fire loop ! */
    upump_mgr_run(ctx->upump_mgr, NULL);

    upump_free(event);

    uprobe_release(ctx->logger);
    uprobe_release(uprobe);
    uprobe_release(ctx->uprobe_udp_srt);

    upump_mgr_release(ctx->upump_mgr);
    uref_mgr_release(ctx->uref_ctx->uref_mgr);
    ubuf_mgr_release(ctx->uref_ctx->ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uclock_release(ctx->uref_ctx->uclock);

    return NULL;
}

struct srt_ctx *open_srt(obe_udp_ctx *p_udp, unsigned latency)
{
    struct srt_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd = p_udp->udp_fd;
    ctx->dest_addr = p_udp->dest_addr;
    ctx->dest_addr_len = p_udp->dest_addr_len;
    ctx->latency = latency;
    ctx->end = false;
    ctx->queue = malloc(sizeof(*ctx->queue));
    if (!ctx->queue) {
        free(ctx);
        return NULL;
    }

    static const uint8_t length = 255;
    ctx->queue_extra = malloc(uqueue_sizeof(length));
    if (!ctx->queue_extra) {
        goto error;
    }
    uqueue_init(ctx->queue, length, ctx->queue_extra);

    ctx->event = malloc(sizeof(*ctx->event));
    if (!ctx->event || !ueventfd_init(ctx->event, false/*?*/))
        goto error;

    pthread_mutex_init(&ctx->mutex, NULL);

    if (pthread_create(&ctx->thread, NULL, srt_thread, ctx) < 0) {
        ueventfd_clean(ctx->event);
        pthread_mutex_destroy(&ctx->mutex);
        goto error;
    }

    return ctx;

error:
    free(ctx->event);
    if(ctx->queue_extra)
        uqueue_clean(ctx->queue);
    free(ctx->queue);
    free(ctx->queue_extra);
    free(ctx);
    return NULL;
}

void close_srt(struct srt_ctx *ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->end = true;
    pthread_mutex_unlock(&ctx->mutex);

    ueventfd_write(ctx->event);
    pthread_join(ctx->thread, NULL);

    ueventfd_clean(ctx->event);
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx->event);
    uqueue_clean(ctx->queue);
    free(ctx->queue);
    free(ctx->queue_extra);
    free(ctx);
}

int srt_bidirectional(struct srt_ctx *ctx)
{
    uint64_t now = uclock_now(ctx->uref_ctx->uclock);
    uint64_t last_rr_cr = 0;

    pthread_mutex_lock(&ctx->mutex);
    last_rr_cr = now+UCLOCK_FREQ;//ctx->last_rr_cr;
    pthread_mutex_unlock(&ctx->mutex);

    // TODO
    if (now - last_rr_cr < UCLOCK_FREQ)
        return 1;

    return 0;
}
