/*****************************************************************************
 * arq.c: rtp retransmission
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
#include <upipe-modules/upipe_rtcp.h>
#include <upipe-modules/upipe_dup.h>

#include <upipe-filters/upipe_rtcp_fb_receiver.h>

#include <bitstream/ietf/rtp.h>
#include <bitstream/ietf/rtcp.h>
#include <bitstream/ietf/rtcp_sr.h>
#include <bitstream/ietf/rtcp_rr.h>
#include <bitstream/ietf/rtcp3611.h>

#include <unistd.h>
#include <fcntl.h>

#include "arq.h"
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

static void catch_event(struct upump *upump)
{
    struct arq_ctx *ctx = upump->opaque;
    ueventfd_read(ctx->event);
    pthread_mutex_lock(&ctx->mutex);
    bool end = ctx->end;
    pthread_mutex_unlock(&ctx->mutex);

    if (end) {
        upipe_release(ctx->upipe_udpsrc);
        upipe_release(ctx->upipe_rtcp_sub);
        upipe_release(ctx->upipe_rtcpfb);
        upipe_release(ctx->upipe_row_udpsink);
        upipe_release(ctx->upipe_col_udpsink);
        upump_stop(upump);
        return;
    }

    for (;;) {
        struct uref *uref = uqueue_pop(ctx->queue, struct uref *);
        if (!uref)
            break;
        struct uref *row_uref = NULL, *col_uref = NULL;
        struct uchain *next = uref->uchain.next;
        for (int i = 0; i < 2; i++) {
            if (!next)
                break;

            struct uref *uref_chain = uref_from_uchain(next);
            if (uref_chain->priv == 1)
                row_uref = uref_chain;
            else
                col_uref = uref_chain;
            next = uref_chain->uchain.next;
        }

        upipe_input(ctx->upipe_rtcpfb, uref, &upump);
        if (row_uref)
            upipe_input(ctx->upipe_row_udpsink, row_uref, &upump);
        if (col_uref)
            upipe_input(ctx->upipe_col_udpsink, col_uref, &upump);
    }
}

static int catch_udp(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    const char *uri;

    switch (event) {
    case UPROBE_SOURCE_END:
        upipe_warn(upipe, "Remote end not listening, can't receive RTCP");
        /* This control can not fail, and will trigger restart of upump */
        upipe_get_uri(upipe, &uri);
        return UBASE_ERR_NONE;
    case UPROBE_UDPSRC_NEW_PEER:
        return UBASE_ERR_NONE;
    default:
        return uprobe_throw_next(uprobe, upipe, event, args);
    }
}

/** @This is the private context of an obe probe */
struct uprobe_arq {
    struct uprobe probe;
    void *data;
};

UPROBE_HELPER_UPROBE(uprobe_arq, probe);

static struct uprobe *uprobe_arq_init(struct uprobe_arq *probe_arq,
                                      struct uprobe *next, uprobe_throw_func catch, void *data)
{
    struct uprobe *probe = uprobe_arq_to_uprobe(probe_arq);
    uprobe_init(probe, catch, next);
    probe_arq->data = data;
    return probe;
}

static void uprobe_arq_clean(struct uprobe_arq *probe_arq)
{
    uprobe_clean(uprobe_arq_to_uprobe(probe_arq));
}

#define ARGS_DECL struct uprobe *next, uprobe_throw_func catch, void *data
#define ARGS next, catch, data
UPROBE_HELPER_ALLOC(uprobe_arq)
#undef ARGS
#undef ARGS_DECL

static void parse_rtcp(struct arq_ctx *ctx, struct upipe *upipe,
        const uint8_t *rtp, int s, uint64_t cr_sys, struct ubuf_mgr *ubuf_mgr)
{
    while (s > 0) {
        if (s < 4 || !rtp_check_hdr(rtp)) {
            upipe_warn_va(upipe, "Received invalid RTP packet");
            break;
        }

        size_t len = 4 + 4 * rtcp_get_length(rtp);
        if (len > s) {
           break;
        }

        switch (rtcp_get_pt(rtp)) {
        case RTCP_PT_SR:
            if (s < RTCP_SR_SIZE)
                break;
            uint32_t ntp_msw = rtcp_sr_get_ntp_time_msw(rtp);
            uint32_t ntp_lsw = rtcp_sr_get_ntp_time_lsw(rtp);
            if (cr_sys != UINT64_MAX)
                ctx->last_sr_cr = cr_sys;
            ctx->last_sr_ntp = ((uint64_t)ntp_msw << 32) | ntp_lsw;
            upipe_verbose_va(upipe, "RTCP SR, CR %"PRIu64" NTP %"PRIu64, ctx->last_sr_cr,
                    ctx->last_sr_ntp);
            break;
        case RTCP_PT_RR:
            if (s < RTCP_RR_SIZE)
                break;
            if (rtcp_get_rc(rtp) < 1)
                break;

            uint32_t delay = rtcp_rr_get_delay_since_last_sr(rtp);
            uint32_t last_sr = rtcp_rr_get_last_sr(rtp);
            if (last_sr != ((ctx->last_sr_ntp >> 16) & 0xffffffff))
                break;

            if (cr_sys != UINT64_MAX) {
                cr_sys -= ctx->last_sr_cr;
                cr_sys -= delay * UCLOCK_FREQ / 65536;
                upipe_verbose_va(upipe, "RTCP RR: RTT %f", (float) cr_sys / UCLOCK_FREQ);
            }
            break;
        case RTCP_PT_XR:
            if (s < RTCP_XR_HEADER_SIZE + RTCP_XR_RRTP_SIZE)
                break;

            uint8_t ssrc[4];
            rtcp_xr_get_ssrc_sender(rtp, ssrc);
            rtp += RTCP_XR_HEADER_SIZE;

            if (rtcp_xr_get_bt(rtp) != RTCP_XR_RRTP_BT)
                break;
            if ((rtcp_xr_get_length(rtp) + 1) * 4 != RTCP_XR_RRTP_SIZE)
                break;

            uint64_t ntp = rtcp_xr_rrtp_get_ntp(rtp);

            struct uref *xr = uref_alloc(ctx->uref_mgr);
            if (!xr)
                break;

            if (cr_sys != UINT64_MAX)
                uref_clock_set_cr_sys(xr, cr_sys);

            const size_t xr_len = RTCP_XR_HEADER_SIZE + RTCP_XR_DLRR_SIZE;
            struct ubuf *ubuf = ubuf_block_alloc(ubuf_mgr, xr_len);
            if (!ubuf) {
                uref_free(xr);
                break;
            }

            uref_attach_ubuf(xr, ubuf);

            uint8_t *buf_xr;
            s = 0;
            uref_block_write(xr, 0, &s, &buf_xr);

            rtcp_set_rtp_version(buf_xr);
            rtcp_set_pt(buf_xr, RTCP_PT_XR);
            rtcp_set_length(buf_xr, xr_len / 4 - 1);

            static const uint8_t pi_ssrc[4] = { 0, 0, 0, 0 };
            rtcp_xr_set_ssrc_sender(buf_xr, pi_ssrc);

            buf_xr += RTCP_XR_HEADER_SIZE;
            rtcp_xr_set_bt(buf_xr, RTCP_XR_DLRR_BT);
            rtcp_xr_dlrr_set_reserved(buf_xr);
            rtcp_xr_set_length(buf_xr, RTCP_XR_DLRR_SIZE / 4 - 1);
            rtcp_xr_dlrr_set_ssrc_receiver(buf_xr, ssrc);

            ntp >>= 16;
            rtcp_xr_dlrr_set_lrr(buf_xr, (uint32_t)ntp);

            rtcp_xr_dlrr_set_dlrr(buf_xr, 0); // delay = 0, we answer immediately

            uref_block_unmap(xr, 0);

            upipe_dbg_va(upipe, "sending XR");
            upipe_input(ctx->upipe_udpsink_rtcp, xr, NULL);
            break;
        default:
            break;
        }

        s -= len;
        rtp += len;
    }
}

static int catch_arq(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    struct uref *uref = NULL;
    struct uprobe_arq *uprobe_arq = uprobe_arq_from_uprobe(uprobe);
    struct arq_ctx *ctx = uprobe_arq->data;

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

        parse_rtcp(ctx, upipe, buf, s, cr_sys, uref->ubuf->mgr);

        uref_block_unmap(uref, 0);
        break;
    }
    default:
        return uprobe_throw_next(uprobe, upipe, event, args);
    }
    return UBASE_ERR_NONE;
}

struct uref *make_uref(struct arq_ctx *ctx, uint8_t *buf, size_t len,
        int64_t timestamp)
{
    struct uref *uref = uref_alloc(ctx->uref_mgr);;
    if (!uref)
        return NULL;

    struct ubuf *ubuf = ubuf_block_alloc_from_opaque(ctx->ubuf_mgr, buf, len);
    if (!ubuf) {
        uref_free(uref);
        return NULL;
    }
    uref_attach_ubuf(uref, ubuf);
    uref_clock_set_pts_prog(uref, timestamp);
    uref_clock_set_cr_sys(uref, uclock_now(ctx->uclock));

    return uref;
}

void arq_write(struct arq_ctx *ctx, struct uref *uref)
{
    if (!uqueue_push(ctx->queue, uref)) {
        printf("full\n");
        uref_free(uref);
    } else {
        ueventfd_write(ctx->event);
    }
}

static void *arq_thread(void *arg)
{
    struct arq_ctx *ctx = arg;

    enum uprobe_log_level loglevel = UPROBE_LOG_DEBUG;

    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    ctx->uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    struct upump_mgr *upump_mgr = upump_ev_mgr_alloc_loop(UPUMP_POOL,
                                                     UPUMP_BLOCKER_POOL);
    ctx->uclock = uclock_std_alloc(UCLOCK_FLAG_REALTIME);
    struct uprobe *uprobe = uprobe_arq_alloc(NULL, catch_arq, ctx);
    struct uprobe *logger = uprobe_stdio_alloc(uprobe_use(uprobe), stdout, loglevel);
    assert(logger != NULL);
    struct uprobe *uprobe_dejitter = uprobe_dejitter_alloc(logger, true, 0);
    assert(uprobe_dejitter != NULL);

    logger = uprobe_uref_mgr_alloc(uprobe_dejitter, ctx->uref_mgr);

    assert(logger != NULL);
    logger = uprobe_upump_mgr_alloc(logger, upump_mgr);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH,
                                   UBUF_POOL_DEPTH);
    assert(logger != NULL);

    logger = uprobe_uclock_alloc(logger, ctx->uclock);
    assert(logger != NULL);

    /* rtp source */
    struct upipe_mgr *upipe_udpsrc_mgr = upipe_udpsrc_mgr_alloc();

    /* send through rtcp fb receiver */
    struct upipe_mgr *upipe_setflowdef_mgr = upipe_setflowdef_mgr_alloc();
    struct upipe *upipe_setflowdef = upipe_void_alloc(upipe_setflowdef_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), loglevel, "rtcp fb setflowdef"));
    upipe_mgr_release(upipe_setflowdef_mgr);

    struct uref *flow_def = uref_block_flow_alloc_def(ctx->uref_mgr, "");
    upipe_set_flow_def(upipe_setflowdef, flow_def);
    upipe_setflowdef_set_dict(upipe_setflowdef, flow_def);
    uref_free(flow_def);

    ctx->upipe_rtcpfb = upipe_setflowdef;

    struct upipe_mgr *upipe_rtcpfb_mgr = upipe_rtcpfb_mgr_alloc();
    struct upipe *upipe_rtcpfb = upipe_void_alloc_output(upipe_setflowdef, upipe_rtcpfb_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), loglevel, "rtcp fb"));
    upipe_mgr_release(upipe_rtcpfb_mgr);

    char slatency[20];
    snprintf(slatency, sizeof(slatency), "%d", ctx->latency);
    ubase_assert(upipe_set_option(upipe_rtcpfb, "latency", slatency));

    struct uprobe uprobe_udp_rtcp;
    uprobe_init(&uprobe_udp_rtcp, catch_udp, uprobe_pfx_alloc(uprobe_use(logger),
                             loglevel, "udp source rtcp"));
    ctx->upipe_udpsrc = upipe_void_alloc(upipe_udpsrc_mgr, &uprobe_udp_rtcp);
    upipe_attach_uclock(ctx->upipe_udpsrc);

    upipe_mgr_release(upipe_udpsrc_mgr);

    /* catch RTCP XR/NACK messages before they're output to rtcp_fb */
    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    struct upipe *upipe_probe_uref = upipe_void_alloc_output(ctx->upipe_udpsrc,
            upipe_probe_uref_mgr, uprobe_pfx_alloc(uprobe_use(logger), loglevel,
                "probe uref"));
    assert(upipe_probe_uref);
    upipe_mgr_release(upipe_probe_uref_mgr);

    ctx->upipe_rtcp_sub = upipe_void_chain_output_sub(upipe_probe_uref,
        upipe_rtcpfb,
        uprobe_pfx_alloc(uprobe_use(logger), loglevel, "rtcp fb sub"));
    assert(ctx->upipe_rtcp_sub);

    struct upipe_mgr *dup_mgr = upipe_dup_mgr_alloc();
    struct upipe *dup = upipe_void_chain_output(upipe_rtcpfb, dup_mgr,
            uprobe_pfx_alloc(uprobe_use(logger),
                             loglevel, "dup"));
    upipe_mgr_release(dup_mgr);

    struct upipe *upipe_rtcpfb_dup = upipe_void_alloc_sub(dup,
            uprobe_pfx_alloc(uprobe_use(logger),
                             loglevel, "dup 1"));

    struct upipe *upipe_rtcpfb_dup2 = upipe_void_alloc_sub(dup,
            uprobe_pfx_alloc(uprobe_use(logger),
                             loglevel, "dup 2"));

    upipe_release(dup);

    struct upipe_mgr *rtcp_mgr = upipe_rtcp_mgr_alloc();
    struct upipe *rtcp = upipe_void_alloc_output(upipe_rtcpfb_dup2, rtcp_mgr,
            uprobe_pfx_alloc(uprobe_use(logger),
                             loglevel, "rtcp"));
    upipe_mgr_release(rtcp_mgr);

    /* catch RTCP SR messages before they're output */
    upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    rtcp = upipe_void_chain_output(rtcp, upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), loglevel, "probe uref"));
    assert(rtcp);
    upipe_mgr_release(upipe_probe_uref_mgr);

    /* send to udp */
    struct upipe_mgr *upipe_udpsink_mgr = upipe_udpsink_mgr_alloc();
    ctx->upipe_udpsink = upipe_void_alloc_output(upipe_rtcpfb_dup, upipe_udpsink_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), loglevel, "udp sink"));

    int flags = fcntl(ctx->fd, F_GETFL);
    flags |= O_NONBLOCK;
    if (fcntl(ctx->fd, F_SETFL, flags) < 0)
        upipe_err(ctx->upipe_udpsink, "Could not set flags");;

    ubase_assert(upipe_udpsink_set_fd(ctx->upipe_udpsink, ctx->fd));
    ubase_assert(upipe_udpsink_set_peer(ctx->upipe_udpsink,
                (const struct sockaddr*)&ctx->dest_addr, ctx->dest_addr_len));

    ubase_assert(upipe_udpsrc_set_fd(ctx->upipe_udpsrc, ctx->rtcp_fd));

    upipe_release(ctx->upipe_udpsink);

    /* RTCP */
    ctx->upipe_udpsink_rtcp = upipe_void_chain_output(rtcp, upipe_udpsink_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), loglevel, "udp sink rtcp"));
    ubase_assert(upipe_udpsink_set_fd(ctx->upipe_udpsink_rtcp, ctx->rtcp_fd));
    ubase_assert(upipe_udpsink_set_peer(ctx->upipe_udpsink_rtcp,
                (const struct sockaddr*)&ctx->rtcp_dest_addr, ctx->rtcp_dest_addr_len));
    ubase_assert(upipe_set_flow_def(ctx->upipe_udpsink_rtcp, flow_def));
    upipe_release(ctx->upipe_udpsink_rtcp);

    /* fec */
    flow_def = uref_block_flow_alloc_def(ctx->uref_mgr, "");

    if (ctx->row_dest_addr_len) {
        ctx->upipe_row_udpsink = upipe_void_alloc(upipe_udpsink_mgr,
                uprobe_pfx_alloc(uprobe_use(logger), loglevel, "udp sink row fec"));

        ubase_assert(upipe_udpsink_set_fd(ctx->upipe_row_udpsink, ctx->row_fd));
        ubase_assert(upipe_udpsink_set_peer(ctx->upipe_row_udpsink,
                    (const struct sockaddr*)&ctx->row_dest_addr, ctx->row_dest_addr_len));
        ubase_assert(upipe_set_flow_def(ctx->upipe_row_udpsink, flow_def));
    }

    if (ctx->col_dest_addr_len) {
        ctx->upipe_col_udpsink = upipe_void_alloc(upipe_udpsink_mgr,
                uprobe_pfx_alloc(uprobe_use(logger), loglevel, "udp sink col fec"));

        ubase_assert(upipe_udpsink_set_fd(ctx->upipe_col_udpsink, ctx->col_fd));
        ubase_assert(upipe_udpsink_set_peer(ctx->upipe_col_udpsink,
                    (const struct sockaddr*)&ctx->col_dest_addr, ctx->col_dest_addr_len));
        ubase_assert(upipe_set_flow_def(ctx->upipe_col_udpsink, flow_def));
    }

    uref_free(flow_def);

    upipe_mgr_release(upipe_udpsink_mgr);

    /* */

    struct upump *event = ueventfd_upump_alloc(ctx->event, upump_mgr,
            catch_event, ctx, NULL);
    upump_start(event);

    ctx->ubuf_mgr = ubuf_block_mem_mgr_alloc(255, 255, umem_mgr, 0, 0, 0, 0);
    assert(ctx->ubuf_mgr);

    /* fire loop ! */
    upump_mgr_run(upump_mgr, NULL);

    upump_free(event);

    uprobe_clean(&uprobe_udp_rtcp);
    uprobe_release(logger);
    uprobe_release(uprobe);

    upump_mgr_release(upump_mgr);
    uref_mgr_release(ctx->uref_mgr);
    ubuf_mgr_release(ctx->ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uclock_release(ctx->uclock);

    return NULL;
}

struct arq_ctx *open_arq(obe_udp_ctx *p_udp, obe_udp_ctx *p_row,
        obe_udp_ctx *p_col, obe_udp_ctx *p_rtcp, unsigned latency)
{
    struct arq_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd = p_udp->udp_fd;
    ctx->dest_addr = p_udp->dest_addr;
    ctx->dest_addr_len = p_udp->dest_addr_len;
    if (p_row) {
        ctx->row_fd = p_row->udp_fd;
        ctx->row_dest_addr = p_row->dest_addr;
        ctx->row_dest_addr_len = p_row->dest_addr_len;
    }
    if (p_col) {
        ctx->col_fd = p_col->udp_fd;
        ctx->col_dest_addr = p_col->dest_addr;
        ctx->col_dest_addr_len = p_col->dest_addr_len;
    }
    if (p_rtcp) {
        ctx->rtcp_fd = p_rtcp->udp_fd;
        ctx->rtcp_dest_addr = p_rtcp->dest_addr;
        ctx->rtcp_dest_addr_len = p_rtcp->dest_addr_len;
    }
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

    if (pthread_create(&ctx->thread, NULL, arq_thread, ctx) < 0) {
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

void close_arq(struct arq_ctx *ctx)
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
