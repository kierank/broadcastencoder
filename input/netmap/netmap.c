/*****************************************************************************
 * netmap.c: netmap input
 *****************************************************************************
 * Copyright (C) 2017 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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
#include "input/input.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <upipe/uprobe.h>
#include <upipe/uprobe_helper_uprobe.h>
#include <upipe/uprobe_helper_alloc.h>
#include <upipe/uprobe_stdio_color.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/uprobe_uclock.h>
#include <upipe/uprobe_dejitter.h>
#include <upipe/umem.h>
#include <upipe/umem_pool.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/uref.h>
#include <upipe/uref_std.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_sound.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/ubuf.h>
#include <upipe/uclock.h>
#include <upipe/uclock_std.h>
#include <upipe/upipe.h>
#include <upipe/upump.h>
#include <upipe/upipe_dump.h>
#include <upipe-pthread/upipe_pthread_transfer.h>
#include <upipe-pthread/uprobe_pthread_upump_mgr.h>
#include <upump-ev/upump_ev.h>
#include <upipe-modules/upipe_transfer.h>
#include <upipe-modules/upipe_htons.h>
#include <upipe-modules/upipe_worker_source.h>
#include <upipe-modules/upipe_worker_linear.h>
#include <upipe-modules/upipe_worker_sink.h>
#include <upipe-modules/upipe_null.h>
#include <upipe-modules/upipe_probe_uref.h>
#include <upipe-hbrmt/upipe_sdi_dec.h>
#include <upipe-netmap/upipe_netmap_source.h>
#include <upipe/uref_dump.h>

#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#define UMEM_POOL               512
#define UDICT_POOL_DEPTH        500
#define UREF_POOL_DEPTH         500
#define UBUF_POOL_DEPTH         3000
#define UBUF_SHARED_POOL_DEPTH  50
#define UPUMP_POOL              10
#define UPUMP_BLOCKER_POOL      10
#define XFER_QUEUE              255
#define XFER_POOL               20

static enum uprobe_log_level loglevel = UPROBE_LOG_DEBUG;

typedef struct
{
    int probe;
    int video_format;

    /* Output */
    int probe_success;

    int width;
    int coded_height;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} netmap_opts_t;

typedef struct
{
    char *uri;
    struct upipe *upipe_main_src;

    /* Probe */
    int             probe_cb_cnt;

    /* Normal run */
    int             video_good;

    /* Video */
    int64_t         v_counter;
    AVRational      v_timebase;
    hnd_t           bars_hnd;
    int64_t         drop_count;
    const char *input_chroma_map[3+1];

    /* frame data for black or last-frame */
    obe_raw_frame_t stored_video_frame;
    obe_raw_frame_t stored_audio_frame;

    /* output frame pointers for bars and tone */
    obe_raw_frame_t **raw_frames;

    /* Audio */
    int64_t         a_counter;
    AVRational      a_timebase;
    const obe_audio_sample_pattern_t *sample_pattern;
    int64_t         a_errors;

    int64_t last_frame_time;

    netmap_opts_t netmap_opts;

    obe_t *h;
} netmap_ctx_t;

/** @This is the private context of an obe probe */
struct uprobe_obe {
    struct uprobe probe;
    void *data;
};

UPROBE_HELPER_UPROBE(uprobe_obe, probe);

static struct uprobe *uprobe_obe_init(struct uprobe_obe *probe_obe,
                                      struct uprobe *next, uprobe_throw_func catch, void *data)
{
    struct uprobe *probe = uprobe_obe_to_uprobe(probe_obe);
    uprobe_init(probe, catch, next);
    probe_obe->data = data;
    return probe;
}

static void uprobe_obe_clean(struct uprobe_obe *probe_obe)
{
    uprobe_clean(uprobe_obe_to_uprobe(probe_obe));
}

#define ARGS_DECL struct uprobe *next, uprobe_throw_func catch, void *data
#define ARGS next, catch, data
UPROBE_HELPER_ALLOC(uprobe_obe)
#undef ARGS
#undef ARGS_DECL

static int catch_video(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uref *flow_def;
    const char *def;

    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

    if (event == UPROBE_NEW_FLOW_DEF) {
        flow_def = va_arg(args, struct uref *);
        uint64_t hsize = 0, vsize = 0;
        struct urational fps = {0, 0};
        uref_pic_flow_get_hsize(flow_def, &hsize);
        uref_pic_flow_get_vsize(flow_def, &vsize);
        uref_pic_flow_get_fps(flow_def, &fps);

        netmap_opts->width = hsize;
        netmap_opts->height = netmap_opts->coded_height = vsize;
        netmap_opts->timebase_num = fps.den;
        netmap_opts->timebase_den = fps.num;
        netmap_opts->interlaced = !ubase_check(uref_pic_get_progressive(flow_def));
        netmap_opts->tff = ubase_check(uref_pic_get_tff(flow_def));
        /* FIXME: probe video_format!! */

        if( netmap_opts->probe )
        {

        }
        else {
            /* check this matches the configured format */
            int j = 0;
            for( ; video_format_tab[j].obe_name != -1; j++ )
            {
                if( netmap_opts->video_format == video_format_tab[j].obe_name )
                    break;
            }

            if( video_format_tab[j].width == netmap_opts->width &&
                video_format_tab[j].height == netmap_opts->height &&
                video_format_tab[j].timebase_num == netmap_opts->timebase_num &&
                video_format_tab[j].timebase_den == netmap_opts->timebase_den &&
                video_format_tab[j].interlaced == netmap_opts->interlaced )
            {
                netmap_ctx->video_good = 1;
            }
            else
            {
                netmap_ctx->video_good = 0;
            }
        }


    }
    else if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        struct upump **upump = va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);

        *drop = true;

        if(netmap_opts->probe) {
            netmap_opts->probe_success = 1;
        }
        else if(netmap_ctx->video_good) {
            obe_raw_frame_t *raw_frame = NULL;
            int64_t pts = -1;
            obe_t *h = netmap_ctx->h;
            uref = uref_dup(uref);

            pts = av_rescale_q( netmap_ctx->v_counter, netmap_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
            /* use SDI ticks as clock source */
            obe_clock_tick( h, pts );

            if( netmap_ctx->last_frame_time == -1 )
                netmap_ctx->last_frame_time = obe_mdate();

            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            pts = raw_frame->pts = av_rescale_q( netmap_ctx->v_counter++, netmap_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );

            for (int i = 0; i < 3 && netmap_ctx->input_chroma_map[i] != NULL; i++)
            {
                const uint8_t *data;
                size_t stride;
                if (unlikely(!ubase_check(uref_pic_plane_read(uref, netmap_ctx->input_chroma_map[i], 0, 0, -1, -1, &data)) ||
                             !ubase_check(uref_pic_plane_size(uref, netmap_ctx->input_chroma_map[i], &stride, NULL, NULL, NULL)))) {
                    syslog(LOG_ERR, "invalid buffer received");
                    uref_free(uref);
                    goto end;
                }

                raw_frame->alloc_img.plane[i] = (uint8_t *)data;
                raw_frame->alloc_img.stride[i] = stride;
            }

            raw_frame->alloc_img.width = netmap_opts->width;
            raw_frame->alloc_img.height = netmap_opts->height;
            raw_frame->alloc_img.csp = AV_PIX_FMT_YUV422P10;
            raw_frame->alloc_img.planes = av_pix_fmt_count_planes(raw_frame->alloc_img.csp);
            raw_frame->alloc_img.format = netmap_opts->video_format;

            memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );
            raw_frame->sar_width = raw_frame->sar_height = 1;

            for( int i = 0; i < h->device.num_input_streams; i++ )
            {
                if( h->device.streams[i]->stream_format == VIDEO_UNCOMPRESSED )
                    raw_frame->input_stream_id = h->device.streams[i]->input_stream_id;
            }

            raw_frame->uref = uref;
            raw_frame->release_data = obe_release_video_uref;
            raw_frame->release_frame = obe_release_frame;

            if( add_to_filter_queue( h, raw_frame ) < 0 )
                goto end;
        }

end:
        return UBASE_ERR_NONE;
    }

    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    return UBASE_ERR_NONE;
}

static int catch_audio(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uref *flow_def;
    const char *def;

    if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        struct upump **upump = va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);
        *drop = true;

        struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
        netmap_ctx_t *netmap_ctx = uprobe_obe->data;
        netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

        if(netmap_opts->probe) {

        }
        else if(netmap_ctx->video_good) {
            obe_raw_frame_t *raw_frame = NULL;
            obe_t *h = netmap_ctx->h;
            const int32_t *src;

            size_t size = 0;
            uint8_t sample_size = 0;
            uref_sound_size(uref, &size, &sample_size);

            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            raw_frame->audio_frame.num_samples = size;
            raw_frame->audio_frame.num_channels = 16;
            raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;

            if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, raw_frame->audio_frame.num_channels,
                                  raw_frame->audio_frame.num_samples, raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            uref_sound_read_int32_t(uref, 0, -1, &src, 1);

            for( int i = 0; i < size; i++)
                for( int j = 0; j < 16; j++ ) 
                {
                    int32_t *audio = (int32_t*)raw_frame->audio_frame.audio_data[j];
                    audio[i] = src[16*i + j];
                }


            uref_sound_unmap(uref, 0, -1, 1);

            raw_frame->pts = av_rescale_q( netmap_ctx->a_counter, netmap_ctx->a_timebase, (AVRational){1, OBE_CLOCK} );
#if 0
            if( 0 ) // FIXME
            {
                raw_frame->video_pts = pts;
                raw_frame->video_duration = av_rescale_q( 1, netmap_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
            }
#endif
            netmap_ctx->a_counter += raw_frame->audio_frame.num_samples;
            raw_frame->release_data = obe_release_audio_data;
            raw_frame->release_frame = obe_release_frame;
            for( int i = 0; i < h->device.num_input_streams; i++ )
            {
                if( h->device.streams[i]->stream_format == AUDIO_PCM )
                    raw_frame->input_stream_id = h->device.streams[i]->input_stream_id;
            }

            if( add_to_filter_queue( netmap_ctx->h, raw_frame ) < 0 )
                goto end;
        }

end:
        return UBASE_ERR_NONE;
    }


    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    return UBASE_ERR_NONE;
}

static int catch_vanc(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uref *flow_def;
    const char *def;

    if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        struct upump **upump = va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);

        /* FIXME handle VANC */
        *drop = true;

        return UBASE_ERR_NONE;
    }


    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    return UBASE_ERR_NONE;
}

static void upipe_event_timer(struct upump *upump)
{
    netmap_ctx_t *netmap_ctx = upump_get_opaque(upump, netmap_ctx_t *);
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;
    obe_t *h = netmap_ctx->h;

    if( netmap_opts->probe )
    {
        netmap_ctx->probe_cb_cnt++;

        if( netmap_opts->probe_success || netmap_ctx->probe_cb_cnt > 10 )
        {
            upump_stop(upump);
            upump_free(upump);

            upipe_release(netmap_ctx->upipe_main_src);
        }
    }
    else
    {
        int stop;

        pthread_mutex_lock( &h->device.device_mutex );
        stop = h->device.stop;
        pthread_mutex_unlock( &h->device.device_mutex);

        if( stop )
        {
            upump_stop(upump);
            upump_free(upump);

            upipe_release(netmap_ctx->upipe_main_src);
        }
    }
}

static int open_netmap( netmap_ctx_t *netmap_ctx )
{
    char *uri = netmap_ctx->uri;

    netmap_ctx->input_chroma_map[0] = "y10l";
    netmap_ctx->input_chroma_map[1] = "u10l";
    netmap_ctx->input_chroma_map[2] = "v10l";
    netmap_ctx->input_chroma_map[3] = NULL;

    /* upump manager for the main thread */
    struct upump_mgr *main_upump_mgr = upump_ev_mgr_alloc_default(UPUMP_POOL, UPUMP_BLOCKER_POOL);
    assert(main_upump_mgr);
    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    udict_mgr_release(udict_mgr);

    /* probes */
    /* main (thread-safe) probe, whose first element is uprobe_pthread_upump_mgr */
    struct uprobe *uprobe_main = uprobe_stdio_color_alloc(NULL, stdout, loglevel);
    assert(uprobe_main);
    uprobe_main = uprobe_uref_mgr_alloc(uprobe_main, uref_mgr);
    assert(uprobe_main);
    uprobe_main = uprobe_ubuf_mem_alloc(uprobe_main, umem_mgr, UBUF_POOL_DEPTH,
                                        UBUF_SHARED_POOL_DEPTH);
    uprobe_main = uprobe_pthread_upump_mgr_alloc(uprobe_main);
    assert(uprobe_main);

    uref_mgr_release(uref_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_pthread_upump_mgr_set(uprobe_main, main_upump_mgr);

    struct uprobe *uprobe_main_pthread = uprobe_main;
    struct uclock *uclock = uclock_std_alloc(0);
    assert(uclock);
    uprobe_main = uprobe_uclock_alloc(uprobe_main, uclock);
    uclock_release(uclock);
    assert(uprobe_main);

    struct uprobe *uprobe_dejitter =
        uprobe_dejitter_alloc(uprobe_use(uprobe_main), true, 1);

    uprobe_throw(uprobe_main, NULL, UPROBE_FREEZE_UPUMP_MGR);

    /* netmap source */
    struct upipe_mgr *upipe_netmap_source_mgr = upipe_netmap_source_mgr_alloc();
    netmap_ctx->upipe_main_src = upipe_void_alloc(upipe_netmap_source_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main),
                loglevel, "netmap source"));
    upipe_attach_uclock(netmap_ctx->upipe_main_src);
    if (!ubase_check(upipe_set_uri(netmap_ctx->upipe_main_src, uri))) {
        return 2;
    }

    uprobe_throw(uprobe_main, NULL, UPROBE_THAW_UPUMP_MGR);

    struct upipe_mgr *xfer_mgr =  upipe_pthread_xfer_mgr_alloc(XFER_QUEUE,
            XFER_POOL, uprobe_use(uprobe_main_pthread), upump_ev_mgr_alloc_loop,
            UPUMP_POOL, UPUMP_BLOCKER_POOL, NULL, NULL, NULL);
    assert(xfer_mgr != NULL);
    // FIXME set thread priority

    struct upipe_mgr *wsrc_mgr = upipe_wsrc_mgr_alloc(xfer_mgr);
    upipe_mgr_release(xfer_mgr);

    /* deport to the source thread */
    netmap_ctx->upipe_main_src = upipe_wsrc_alloc(wsrc_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main),
                             loglevel, "wsrc"),
            netmap_ctx->upipe_main_src,
            uprobe_pfx_alloc(uprobe_use(uprobe_main),
                             loglevel, "wsrc_x"),
            255);

    upipe_mgr_release(wsrc_mgr);

    /* sdi dec to y10 */
    struct upipe_mgr *upipe_sdi_dec_mgr = upipe_sdi_dec_mgr_alloc();
    struct uref *uref = uref_alloc(uref_mgr);
    uref_flow_set_def(uref, "pic.");
    uref_pic_flow_set_macropixel(uref, 1);
    uref_pic_flow_add_plane(uref, 1, 1, 1, "y10l");
    uref_pic_flow_add_plane(uref, 2, 2, 1, "u10l");
    uref_pic_flow_add_plane(uref, 2, 2, 1, "v10l");

    struct upipe *sdi_dec = upipe_sdi_dec_alloc_output(netmap_ctx->upipe_main_src,
        upipe_sdi_dec_mgr,
        uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec"),
        uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec vanc"),
        uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec vbi"),
        uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec audio"),
        uref);
    uref_free(uref);
    //upipe_set_option(sdi_dec, "debug", "1");
    upipe_mgr_release(upipe_sdi_dec_mgr);

    /* video callback */
    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    struct upipe *probe_uref_video = upipe_void_chain_output(sdi_dec,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_video, netmap_ctx),
            loglevel, "probe_uref_video"));
    upipe_release(probe_uref_video);

    /* audio */
    struct upipe *audio = NULL;
    if (!ubase_check(upipe_sdi_dec_get_audio_sub(sdi_dec, &audio))) {
        printf("NO AUDIO\n");
        return 1;
    }
    else {
        upipe_release(audio);
    }

    /* audio callback */
    struct upipe *probe_uref_audio = upipe_void_alloc_output(audio,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_audio, netmap_ctx),
            loglevel, "audio probe_uref"));
    upipe_release(probe_uref_audio);

    /* vanc */
    struct upipe *vanc = NULL;
    if (!ubase_check(upipe_sdi_dec_get_vanc_sub(sdi_dec, &vanc))) {
        printf("NO vanc\n");
        return 1;
    }
    else {
        upipe_release(vanc);
    }

    /* vanc callback */
    struct upipe *probe_uref_vanc = upipe_void_alloc_output(vanc,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_vanc, netmap_ctx),
            loglevel, "vanc probe_uref"));
    upipe_release(probe_uref_vanc);

    upipe_mgr_release(upipe_probe_uref_mgr);

    /* vbi */
    struct upipe *vbi = NULL;
    if (!ubase_check(upipe_sdi_dec_get_vbi_sub(sdi_dec, &vbi))) {
        printf("NO vbi\n");
        return 1;
    }
    else {
        upipe_release(vbi);
    }

    static struct upump *event_upump;
    /* stop timer */
    event_upump = upump_alloc_timer(main_upump_mgr, upipe_event_timer, netmap_ctx, NULL,
                                    UCLOCK_FREQ, UCLOCK_FREQ);
    assert(event_upump != NULL);
    upump_start(event_upump);

    /* main loop */
    upump_mgr_run(main_upump_mgr, NULL);

    /* Wait on all upumps */
    upump_mgr_release(main_upump_mgr);
    uprobe_release(uprobe_main);	
    uprobe_release(uprobe_dejitter);

    return 0;

}

static void *autoconf_input( void *ptr )
{
    obe_int_input_stream_t *streams[MAX_STREAMS];
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    int cur_input_stream_id = 0;

    for( int i = 0; i < 2; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            return NULL;

        streams[i]->input_stream_id = cur_input_stream_id++;

        if( i == 0 )
        {
            int j;
            for( j = 0; video_format_tab[j].obe_name != -1; j++ )
            {
                if( video_format_tab[j].obe_name == user_opts->video_format )
                    break;
            }

            if( video_format_tab[j].obe_name == -1 )
            {
                fprintf( stderr, "[netmap] Unsupported video format\n" );
                return NULL;
            }

            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->video_format = user_opts->video_format;
            streams[i]->width  = video_format_tab[j].width;
            streams[i]->height = video_format_tab[j].height;
            streams[i]->timebase_num = video_format_tab[j].timebase_num;
            streams[i]->timebase_den = video_format_tab[j].timebase_den;
            streams[i]->csp    = AV_PIX_FMT_YUV422P10;
            streams[i]->interlaced = video_format_tab[j].interlaced;
            streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels  = 16;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[i]->sample_rate = 48000;
        }
    }

    device = new_device();

    if( !device )
        return NULL;

    device->num_input_streams = 2;
    memcpy( device->streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**) );
    device->device_type = INPUT_DEVICE_NETMAP;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );
    pthread_mutex_destroy( &h->device.device_mutex );

    /* add device */
    memcpy( &h->device, device, sizeof(*device) );
    free( device );

    return NULL;
}

static void *probe_input( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;

    netmap_ctx_t netmap_ctx = {0};
    netmap_ctx.uri = user_opts->netmap_uri;
    netmap_ctx.h = h;
    netmap_opts_t *netmap_opts = &netmap_ctx.netmap_opts;
    netmap_opts->probe = 1;

    open_netmap( &netmap_ctx );

    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int cur_stream = 2, cur_input_stream_id = 0;

    if( !netmap_opts->probe_success )
    {
        fprintf( stderr, "[netmap] No valid frames received - check connection and input format\n" );
        goto finish;
    }

    for( int i = 0; i < 2; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            goto finish;

        streams[i]->input_stream_id = cur_input_stream_id++;

        if( i == 0 )
        {
            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->video_format = netmap_opts->video_format; /* FIXME this isn't set currently */
            streams[i]->width  = netmap_opts->width;
            streams[i]->height = netmap_opts->height;
            streams[i]->timebase_num = netmap_opts->timebase_num;
            streams[i]->timebase_den = netmap_opts->timebase_den;
            streams[i]->csp    = AV_PIX_FMT_YUV422P10;
            streams[i]->interlaced = netmap_opts->interlaced;
            streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels  = 16;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[i]->sample_rate = 48000;
        }
    }

    /* TODO: VBI/VANC */

    device = new_device();

    if( !device )
        goto finish;

    device->num_input_streams = cur_stream;
    memcpy( device->streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**) );
    device->device_type = INPUT_DEVICE_NETMAP;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );
    pthread_mutex_destroy( &h->device.device_mutex );

    /* add device */
    memcpy( &h->device, device, sizeof(*device) );
    free( device );

finish:
    free( probe_ctx );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_input_t *user_opts = &h->device.user_opts;
    hnd_t netmap = NULL;

    netmap_ctx_t netmap_ctx = {0};
    netmap_opts_t *netmap_opts = &netmap_ctx.netmap_opts;
    netmap_opts->video_format = user_opts->video_format;

    netmap_ctx.uri = user_opts->netmap_uri;
    netmap_ctx.h = h;

    int j = 0;
    for( ; video_format_tab[j].obe_name != -1; j++ )
    {
        if( netmap_opts->video_format == video_format_tab[j].obe_name )
            break;
    }

    netmap_ctx.v_timebase.num = video_format_tab[j].timebase_num;
    netmap_ctx.v_timebase.den = video_format_tab[j].timebase_den;    

    netmap_ctx.a_timebase.num = 1;
    netmap_ctx.a_timebase.den = 48000;

    open_netmap( &netmap_ctx );



    //pthread_cleanup_push( close_thread, (void*)&status );


    //pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_input_func_t netmap_input = { probe_input, autoconf_input, open_input };
