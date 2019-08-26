/*****************************************************************************
 * netmap.c: netmap input
 *****************************************************************************
 * Copyright (C) 2017-2018 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *          Rafaël Carré
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

#define _GNU_SOURCE
#include "common/common.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/vbi.h"
#include "input/bars/bars_common.h"

#include <stdlib.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <upipe/uprobe.h>
#include <upipe/uprobe_helper_uprobe.h>
#include <upipe/uprobe_helper_alloc.h>
#include <upipe/uprobe_stdio.h>
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
#include <upipe/uref_block.h>
#include <upipe/ubuf.h>
#include <upipe/uclock.h>
#include <upipe/uclock_ptp.h>
#include <upipe/upipe.h>
#include <upipe/upump.h>
#include <upipe/upipe_dump.h>
#include <upipe-pthread/upipe_pthread_transfer.h>
#include <upipe-pthread/uprobe_pthread_upump_mgr.h>
#include <upump-ev/upump_ev.h>
#include <upipe-modules/upipe_transfer.h>
#include <upipe-modules/upipe_setflowdef.h>
#include <upipe-modules/upipe_udp_source.h>
#include <upipe-modules/upipe_rtp_decaps.h>
#include <upipe-modules/upipe_rtp_reorder.h>
#include <upipe-modules/upipe_rtp_pcm_unpack.h>
#include <upipe-modules/upipe_htons.h>
#include <upipe-modules/upipe_worker_source.h>
#include <upipe-modules/upipe_worker_linear.h>
#include <upipe-modules/upipe_worker_sink.h>
#include <upipe-modules/upipe_null.h>
#include <upipe-modules/upipe_probe_uref.h>
#include <upipe-modules/upipe_sync.h>
#include <upipe-hbrmt/upipe_sdi_dec.h>
#include <upipe-netmap/upipe_netmap_source.h>
#include <upipe-pciesdi/upipe_pciesdi_source.h>
#include <upipe-filters/upipe_filter_vanc.h>
#include <upipe-filters/upipe_filter_vbi.h>
#include <upipe/uref_dump.h>

#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#include <bitstream/dvb/vbi.h>
#include <bitstream/smpte/291.h>

#define UMEM_POOL               512
#define UDICT_POOL_DEPTH        500
#define UREF_POOL_DEPTH         500
#define UBUF_POOL_DEPTH         3000
#define UBUF_SHARED_POOL_DEPTH  50
#define UPUMP_POOL              10
#define UPUMP_BLOCKER_POOL      10
#define XFER_QUEUE              255
#define XFER_POOL               20

#define RFC_LATENCY (UCLOCK_FREQ/100)

typedef struct
{
    int probe;
    int video_format;
    int picture_on_loss;
    obe_bars_opts_t obe_bars_opts;

    /* Output */
    int probe_success;

    int width;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} netmap_opts_t;

typedef struct
{
    uint8_t idx;
    uint8_t channels;

    size_t samples;

    struct upipe *src[2];
    struct upipe *rtpr;

} netmap_audio_t;

typedef struct
{
    char *uri;
    struct upipe *upipe_main_src;
    struct upipe *avsync;

    /* Probe */
    int             probe_cb_cnt;

    /* Normal run */
    int             video_good;

    char            *ptp_nic;
    bool            rfc4175;

    /* Video */
    int             detected_video_format;
    int64_t         v_counter;
    AVRational      v_timebase;
    int64_t         video_freq;
    hnd_t           bars_hnd;
    const char *input_chroma_map[3+1];

    /* frame data for black or last-frame */
    obe_raw_frame_t stored_video_frame;
    obe_raw_frame_t stored_audio_frame;

    /* output frame pointers for bars and tone */
    obe_raw_frame_t **raw_frames;

    /* Audio */
    char            *audio_uri;
    int64_t         a_counter;
    AVRational      a_timebase;
    const obe_audio_sample_pattern_t *sample_pattern;
    int64_t         a_errors;
    uint8_t         channels;

    netmap_audio_t  audio[16];

    int64_t last_frame_time;

    netmap_opts_t netmap_opts;

    /* Upipe events */
    struct upump_mgr *upump_mgr;

    struct upump *no_video_upump;

    int stop;

    obe_sdi_non_display_data_t non_display_parser;

    obe_coded_frame_t *anc_frame;

    uint16_t cc_ctr;

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

static void no_video_timer(struct upump *upump)
{
    netmap_ctx_t *netmap_ctx = upump_get_opaque(upump, netmap_ctx_t *);
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;
    obe_t *h = netmap_ctx->h;
    obe_int_input_stream_t *video_stream = get_input_stream(h, 0);
    int64_t pts = -1;

    if (!video_stream)
        return;

    syslog( LOG_ERR, "inputDropped: No input signal detected" );
    if (!netmap_opts->picture_on_loss)
        return;

    obe_raw_frame_t *video_frame = NULL, *audio_frame = NULL;
    pts = av_rescale_q( netmap_ctx->v_counter, netmap_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
    obe_clock_tick( h, pts );

    if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
        netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
    {
        video_frame = new_raw_frame();
        if( !video_frame )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            return;
        }
        memcpy( video_frame, &netmap_ctx->stored_video_frame, sizeof(*video_frame) );

        /* Handle the case where no frames received, so first frame in LASTFRAME mode is black */
        if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK || netmap_ctx->stored_video_frame.uref == NULL )
        {
            int i = 0;
            while( video_frame->buf_ref[i] != NULL )
            {
                video_frame->buf_ref[i] = av_buffer_ref( netmap_ctx->stored_video_frame.buf_ref[i] );
                i++;
            }
            video_frame->buf_ref[i] = NULL;
        }
        else if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
        {
            video_frame->uref = uref_dup(netmap_ctx->stored_video_frame.uref);
            /* Map the frame again to create another buffer reference */
            for (int i = 0; i < 3 && netmap_ctx->input_chroma_map[i] != NULL; i++)
            {
                const uint8_t *data;
                size_t stride;
                if (unlikely(!ubase_check(uref_pic_plane_read(video_frame->uref, netmap_ctx->input_chroma_map[i], 0, 0, -1, -1, &data)) ||
                            !ubase_check(uref_pic_plane_size(video_frame->uref, netmap_ctx->input_chroma_map[i], &stride, NULL, NULL, NULL)))) {
                    syslog(LOG_ERR, "invalid buffer received");
                    uref_free(video_frame->uref);
                    return;
                }

                video_frame->alloc_img.plane[i] = (uint8_t *)data;
                video_frame->alloc_img.stride[i] = stride;
            }
        }
    }
    else if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BARS )
    {
        get_bars( netmap_ctx->bars_hnd, netmap_ctx->raw_frames );

        video_frame = netmap_ctx->raw_frames[0];
        audio_frame = netmap_ctx->raw_frames[1];
    }

    video_frame->pts = pts;

    if( add_to_filter_queue( h, video_frame ) < 0 )
        return;

    if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
        netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
    {
        audio_frame = new_raw_frame();
        if( !audio_frame )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            return;
        }

        memcpy( audio_frame, &netmap_ctx->stored_audio_frame, sizeof(*audio_frame) );
        /* Assumes only one buffer reference */
        audio_frame->buf_ref[0] = av_buffer_ref( netmap_ctx->stored_audio_frame.buf_ref[0] );
        audio_frame->buf_ref[1] = NULL;

        audio_frame->audio_frame.num_samples = netmap_ctx->sample_pattern->pattern[netmap_ctx->v_counter % netmap_ctx->sample_pattern->mod];
    }

    /* Write audio frame */
    for( int i = 0; i < h->device.num_input_streams; i++ )
    {
        if( h->device.streams[i]->stream_format == AUDIO_PCM )
            audio_frame->input_stream_id = h->device.streams[i]->input_stream_id;
    }

    audio_frame->pts = av_rescale_q( netmap_ctx->a_counter, netmap_ctx->a_timebase,
                                        (AVRational){1, OBE_CLOCK} );
    netmap_ctx->a_counter += audio_frame->audio_frame.num_samples;

    if( add_to_filter_queue( h, audio_frame ) < 0 )
        return;
    /* Increase video PTS at the end so it can be used in NTSC sample size generation */
    netmap_ctx->v_counter++;
}

static void stop_no_video_timer(netmap_ctx_t *netmap_ctx)
{
    if (netmap_ctx->no_video_upump) {
        upump_stop(netmap_ctx->no_video_upump);
        upump_free(netmap_ctx->no_video_upump);
    }
}

static void setup_picture_on_signal_loss_timer(netmap_ctx_t *netmap_ctx)
{
    stop_no_video_timer(netmap_ctx);
    netmap_ctx->no_video_upump = upump_alloc_timer(netmap_ctx->upump_mgr,
            no_video_timer, netmap_ctx, NULL, UCLOCK_FREQ/4, netmap_ctx->video_freq);
    assert(netmap_ctx->no_video_upump != NULL);
    upump_start(netmap_ctx->no_video_upump);
}


static void extract_afd_bar_data(netmap_ctx_t *netmap_ctx, struct uref *uref, obe_raw_frame_t *raw_frame)
{
    obe_t *h = netmap_ctx->h;
    obe_sdi_non_display_data_t *non_display_data = &netmap_ctx->non_display_parser;

    uint8_t afd = 0;
    if (!ubase_check(uref_pic_get_afd(uref, &afd)))
		return;

    const uint8_t *bar_data = NULL;
    size_t bar_data_size = 0;
    uref_pic_get_bar_data(uref, &bar_data, &bar_data_size);
    if (bar_data_size < 5)
        return;

    /* TODO: make Bar Data optional */

    /* AFD is duplicated on the second field so skip it if we've already detected it */
    if( non_display_data->probe )
    {
        /* TODO: mention existence of second line of AFD? */
        if( check_probed_non_display_data( non_display_data, MISC_AFD ) )
            return;

		obe_int_frame_data_t *tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+2) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

		obe_int_frame_data_t *frame_data = &non_display_data->frame_data[non_display_data->num_frame_data];
        non_display_data->num_frame_data += 2;

        /* AFD */
        frame_data->type = MISC_AFD;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = 1;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        /* Bar data */
        frame_data++;
        frame_data->type = MISC_BAR_DATA;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = 1;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        return;
    }

    /* Return if user didn't select AFD */
    if( !check_user_selected_non_display_data( h, MISC_AFD, USER_DATA_LOCATION_FRAME ) )
        return;

    /* Return if AFD already exists in frame */
    if( check_active_non_display_data( raw_frame, USER_DATA_AFD ) )
        return;

    obe_user_data_t *tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+2) * sizeof(*raw_frame->user_data) );
    if( !tmp2 )
        goto fail;

    raw_frame->user_data = tmp2;
    obe_user_data_t *user_data = &raw_frame->user_data[raw_frame->num_user_data];
    raw_frame->num_user_data += 2;

    /* Read AFD */
    user_data->len = 1;
    user_data->type = USER_DATA_AFD;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len );
    if( !user_data->data )
        goto fail;

    user_data->data[0] = afd;

    user_data++;

    /* Read Bar Data */
    user_data->len = 5;
    user_data->type = USER_DATA_BAR_DATA;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len );
    if( !user_data->data )
        goto fail;

    memcpy(user_data->data, bar_data, 5);

    return;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return;
}


static void extract_op47(netmap_ctx_t *netmap_ctx, struct uref *uref)
{
    obe_sdi_non_display_data_t *non_display_data = &netmap_ctx->non_display_parser;
    obe_t *h = netmap_ctx->h;

    non_display_data->has_ttx_frame = 1;

    if( non_display_data->probe )
    {
        if( check_probed_non_display_data( non_display_data, MISC_TELETEXT ) )
            return;

        obe_int_frame_data_t *tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            return;

        non_display_data->frame_data = tmp;

        obe_int_frame_data_t *frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = MISC_TELETEXT;
        frame_data->source = VANC_OP47_SDP;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = 12;
        frame_data->location = USER_DATA_LOCATION_DVB_STREAM;

        return;
    }

    if( !get_output_stream_by_format( h, MISC_TELETEXT ) )
        return;

    int s = -1;
    const uint8_t *buf;
    if (!ubase_check(uref_block_read(uref, 0, &s, &buf)))
        return;

    buf++; // skip DVBVBI_DATA_IDENTIFIER
    s--;
    for( int i = 0; i < 5; i++ ) {
        if (s < DVBVBI_UNIT_HEADER_SIZE + DVBVBI_LENGTH)
            break;

        if (buf[0] != DVBVBI_ID_TTX_SUB && buf[0] != DVBVBI_ID_TTX_NONSUB)
            goto skip;

        if (buf[1] != DVBVBI_LENGTH)
            goto skip;

        vbi_sliced *vbi_slice = &non_display_data->vbi_slices[non_display_data->num_vbi++];
        vbi_slice->id = VBI_SLICED_TELETEXT_B;

        int line_smpte = 0;
        int field = dvbvbittx_get_field(&buf[DVBVBI_UNIT_HEADER_SIZE]) ? 1 : 2;
        int line_analogue = dvbvbittx_get_line(&buf[DVBVBI_UNIT_HEADER_SIZE]);

        obe_convert_analogue_to_smpte( INPUT_VIDEO_FORMAT_PAL, line_analogue, field, &line_smpte );

        vbi_slice->line = line_smpte;

        for( int j = 0; j < 40+2; j++ ) // MRAG x2 + data_block
            vbi_slice->data[j] = REVERSE(buf[4+j]);

skip:
        buf += DVBVBI_UNIT_HEADER_SIZE;
        s -= DVBVBI_UNIT_HEADER_SIZE;

        buf += DVBVBI_LENGTH;
        s -= DVBVBI_LENGTH;
    }

    uref_block_unmap(uref, 0);
}

static void extract_cc(netmap_ctx_t *netmap_ctx, struct uref *uref, obe_raw_frame_t *raw_frame)
{
    obe_t *h = netmap_ctx->h;
    const uint8_t *pic_data = NULL;
    size_t pic_data_size = 0;
    uref_pic_get_cea_708(uref, &pic_data, &pic_data_size);
    if (!pic_data_size)
        return;

    obe_sdi_non_display_data_t *non_display_data = &netmap_ctx->non_display_parser;

    if( non_display_data->probe )
    {
        if( check_probed_non_display_data( non_display_data, CAPTIONS_CEA_708 ) )
            return;

        obe_int_frame_data_t *tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        obe_int_frame_data_t *frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = CAPTIONS_CEA_708;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = 1;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        non_display_data->has_probed = 1;

        return;
    }

    /* Return if user didn't select CEA-708 */
    if( !check_user_selected_non_display_data( h, CAPTIONS_CEA_708, USER_DATA_LOCATION_FRAME ) ) {
        return;
    }

    /* Return if there is already CEA-708 data in the frame */
    if( check_active_non_display_data( raw_frame, USER_DATA_CEA_708_CDP ) ) {
        return;
    }

    obe_user_data_t *tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
    if( !tmp2 )
        goto fail;

    raw_frame->user_data = tmp2;
    obe_user_data_t *user_data = &raw_frame->user_data[raw_frame->num_user_data++];
    user_data->len = 9 + pic_data_size + 4;
    user_data->type = USER_DATA_CEA_708_CDP;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len);

    if( user_data->data ) {
        const uint16_t hdr_sequence_cntr = netmap_ctx->cc_ctr++;
        netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;
        const uint8_t fps = (netmap_opts->timebase_den == 1001) ? 0x4 : 0x7;

        user_data->data[0] = 0x96;
        user_data->data[1] = 0x69;
        user_data->data[2] = user_data->len;
        user_data->data[3] = (fps << 4) | 0xf;
        user_data->data[4] = (1 << 6) | (1 << 1) | 1; // ccdata_present | caption_service_active | Reserved
        user_data->data[5] = hdr_sequence_cntr >> 8;
        user_data->data[6] = hdr_sequence_cntr & 0xff;
        user_data->data[7] = 0x72;
        user_data->data[8] = (0x7 << 5) | (pic_data_size / 3);

        user_data->data[9 + pic_data_size] = 0x74;
        user_data->data[9 + pic_data_size+1] = user_data->data[5];
        user_data->data[9 + pic_data_size+2] = user_data->data[6];

        for( int i = 0; i < pic_data_size; i++ )
            user_data->data[9+i] = pic_data[i];

        uint8_t checksum = 0;
        for (int i = 0; i < user_data->len - 1; i++) // don't include checksum
            checksum += user_data->data[i];

        user_data->data[9 + pic_data_size+3] = checksum ? 256 - checksum : 0;

    }

    return;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
}

static int catch_video(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uref *flow_def;
    const char *def;

    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;
    obe_t *h = netmap_ctx->h;

    if (netmap_ctx->stop)
        return UBASE_ERR_NONE;

    if (event == UPROBE_NEW_FLOW_DEF) {
        flow_def = va_arg(args, struct uref *);
        uint64_t hsize = 0, vsize = 0;
        struct urational fps = {0, 0};
        uref_pic_flow_get_hsize(flow_def, &hsize);
        uref_pic_flow_get_vsize(flow_def, &vsize);
        uref_pic_flow_get_fps(flow_def, &fps);

        netmap_opts->width = hsize;
        netmap_opts->height = vsize;
        netmap_opts->timebase_num = fps.den;
        netmap_opts->timebase_den = fps.num;
        netmap_opts->interlaced = !ubase_check(uref_pic_get_progressive(flow_def));
        netmap_opts->tff = ubase_check(uref_pic_get_tff(flow_def));

        /* FIXME: probe video_format!! */
        if( netmap_opts->probe ) {

        }
        else {
            /* check this matches the configured format */
            int j = 0, detected_video_format = -1;
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
                detected_video_format = video_format_tab[j].obe_name;
            }
            else
            {
                netmap_ctx->video_good = 0;

                /* Find the actual format */
                j = 0;
                for( ; video_format_tab[j].obe_name != -1; j++ )
                {
                    if( video_format_tab[j].width == netmap_opts->width &&
                        video_format_tab[j].height == netmap_opts->height &&
                        video_format_tab[j].timebase_num == netmap_opts->timebase_num &&
                        video_format_tab[j].timebase_den == netmap_opts->timebase_den &&
                        video_format_tab[j].interlaced == netmap_opts->interlaced )
                    {
                        detected_video_format = video_format_tab[j].obe_name;
                        break;
                    }
                }
            }

            printf("\n detected %i \n", detected_video_format);

            if( netmap_ctx->detected_video_format != detected_video_format )
            {
                pthread_mutex_lock( &h->device_mutex );
                h->device.input_status.detected_video_format = detected_video_format;
                pthread_mutex_unlock( &h->device_mutex );
                netmap_ctx->detected_video_format = detected_video_format;
            }
        }
    }
    else if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);

        *drop = true;

        bool discontinuity = ubase_check(uref_flow_get_discontinuity(uref));

        pthread_mutex_lock( &h->device_mutex );
        h->device.input_status.active = 1;
        pthread_mutex_unlock( &h->device_mutex );

        netmap_ctx->last_frame_time = obe_mdate();

        if(netmap_opts->probe) {
            extract_cc(netmap_ctx, uref, NULL);
            extract_afd_bar_data(netmap_ctx, uref, NULL);
            extract_op47(netmap_ctx, NULL);
            netmap_opts->probe_success = 1;
        } else if(netmap_ctx->video_good == 1) {
            /* drop first video frame,
             * we already dropped first audio frame because video flow def was not yet set
             */
            netmap_ctx->video_good = 2;
        } else if(netmap_ctx->video_good) {
            obe_raw_frame_t *raw_frame = NULL;
            int64_t pts = -1;

            if( discontinuity )
            {
                pthread_mutex_lock( &h->drop_mutex );
                h->encoder_drop = h->mux_drop = 1;
                pthread_mutex_unlock( &h->drop_mutex );
            }

            pts = av_rescale_q( netmap_ctx->v_counter++, netmap_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
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

            raw_frame->pts = pts;

            uref = uref_dup(uref);
            extract_cc(netmap_ctx, uref, raw_frame);
            extract_afd_bar_data(netmap_ctx, uref, raw_frame);
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

            setup_picture_on_signal_loss_timer(netmap_ctx);

            /* Make a copy of the frame for showing the last frame */
            if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
            {
                if( netmap_ctx->stored_video_frame.release_data )
                    netmap_ctx->stored_video_frame.release_data( &netmap_ctx->stored_video_frame );

                memcpy( &netmap_ctx->stored_video_frame, raw_frame, sizeof(netmap_ctx->stored_video_frame) );

                uref = uref_dup(uref);
                /* Map the frame again to create another buffer reference */
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
                    netmap_ctx->stored_video_frame.buf_ref[i] = NULL;
                }

                netmap_ctx->stored_video_frame.release_data = obe_release_video_uref;
                netmap_ctx->stored_video_frame.num_user_data = 0;
                netmap_ctx->stored_video_frame.user_data = NULL;
                netmap_ctx->stored_video_frame.uref = uref;
            }

            if( add_to_filter_queue( h, raw_frame ) < 0 )
                goto end;

            if( send_vbi_and_ttx( h, &netmap_ctx->non_display_parser, pts ) < 0 )
                goto end;

            if (netmap_ctx->anc_frame) {
                obe_coded_frame_t *anc_frame = netmap_ctx->anc_frame;
                anc_frame->pts = pts;

                if (add_to_queue(&h->mux_queue, &anc_frame->uchain) < 0) {
                    destroy_coded_frame(anc_frame);
                }

                netmap_ctx->anc_frame = NULL;
            }


            netmap_ctx->non_display_parser.num_vbi = 0;
            netmap_ctx->non_display_parser.num_anc_vbi = 0;
        }

end:
        return UBASE_ERR_NONE;
    }

    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    return UBASE_ERR_NONE;
}

static int catch_audio_hbrmt(struct uprobe *uprobe, struct upipe *upipe,
                             int event, va_list args)
{
    struct uref *flow_def;
    const char *def;
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

    if (netmap_ctx->stop)
        return UBASE_ERR_NONE;

    if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);
        *drop = true;

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
            raw_frame->audio_frame.num_channels = netmap_ctx->channels;
            raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;

            if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, raw_frame->audio_frame.num_channels,
                                  raw_frame->audio_frame.num_samples, raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            uref_sound_read_int32_t(uref, 0, -1, &src, 1);

            for( int i = 0; i < size; i++)
                for( int j = 0; j < netmap_ctx->channels; j++ )
                {
                    int32_t *audio = (int32_t*)raw_frame->audio_frame.audio_data[j];
                    audio[i] = src[netmap_ctx->channels*i + j];
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
    } else if (event == UPROBE_NEW_FLOW_DEF) {
        flow_def = va_arg(args, struct uref *);
        if (!ubase_check(uref_sound_flow_get_channels(flow_def, &netmap_ctx->channels))) {
            netmap_ctx->channels = 0;
        }

    }

    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    return UBASE_ERR_NONE;
}

static int catch_audio_2110(struct uprobe *uprobe, struct upipe *upipe,
                           int event, va_list args)
{
    struct uref *flow_def;
    const char *def;
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

    if (netmap_ctx->stop)
        return UBASE_ERR_NONE;

    if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);
        *drop = true;

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
            raw_frame->audio_frame.num_channels = netmap_ctx->channels;
            raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;

            if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, raw_frame->audio_frame.num_channels,
                                  raw_frame->audio_frame.num_samples, raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            uref_sound_read_int32_t(uref, 0, -1, &src, 1);

            for( int i = 0; i < size; i++)
                for( int j = 0; j < netmap_ctx->channels; j++ )
                {
                    int32_t *audio = (int32_t*)raw_frame->audio_frame.audio_data[j];
                    audio[i] = src[netmap_ctx->channels*i + j];
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
    else {
        if (!uprobe_plumber(event, args, &flow_def, &def))
            return uprobe_throw_next(uprobe, upipe, event, args);
    }

    return UBASE_ERR_NONE;
}

static int catch_ttx(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    struct uref *flow_def;
    const char *def;

    if (event == UPROBE_PROBE_UREF) {
        UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
        struct uref *uref = va_arg(args, struct uref *);
        va_arg(args, struct upump **);
        bool *drop = va_arg(args, bool *);
        *drop = true;

        extract_op47(netmap_ctx, uref);

        return UBASE_ERR_NONE;
    }


    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    struct upipe *probe_uref_ttx = upipe_void_alloc_output(upipe,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe), catch_ttx, netmap_ctx),
            UPROBE_LOG_DEBUG, "probe_uref_ttx"));
    upipe_mgr_release(upipe_probe_uref_mgr);
    upipe_release(probe_uref_ttx);

    return UBASE_ERR_NONE;
}

static bool send_did_sdid(netmap_ctx_t *netmap_ctx, uint8_t did, uint8_t sdid)
{
    // TODO
    return false;
}

static int catch_sdi_dec(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

    if (event != UPROBE_SDI_DEC_HANC_PACKET)
        return uprobe_throw_next(uprobe, upipe, event, args);

    UBASE_SIGNATURE_CHECK(args, UPIPE_SDI_DEC_SIGNATURE);

    unsigned line = va_arg(args, unsigned);
    unsigned horiz_offset = va_arg(args, unsigned);
    const uint16_t *packet = va_arg(args, const uint16_t *);

    bool c_not_y = false;
    const unsigned gap = IS_SD(netmap_opts->video_format) ? 1 : 2;
    if (gap == 2) {
        c_not_y = horiz_offset & 1;
        horiz_offset /= 2;
    }

    uint16_t did = packet[gap*3];
    uint16_t sdid = packet[gap*4];
    uint16_t dc = packet[gap*5];

    if (!send_did_sdid(netmap_ctx, did & 0xff, sdid & 0xff))
        return UBASE_ERR_NONE;

    obe_t *h = netmap_ctx->h;
    obe_output_stream_t *output_stream = get_output_stream_by_format(h, ANC_RAW);
    if (!output_stream)
        return UBASE_ERR_INVALID;

    obe_coded_frame_t *anc_frame = netmap_ctx->anc_frame;
    if (!anc_frame) {
        netmap_ctx->anc_frame = anc_frame = new_coded_frame(output_stream->output_stream_id, 65536);
        if (!anc_frame)
            return UBASE_ERR_ALLOC;

        anc_frame->len = 0;
    }

    bs_t s;
    bs_init(&s, &anc_frame->data[anc_frame->len], 65536 - anc_frame->len);

    bs_write(&s, 6, 0);
    bs_write(&s, 1, c_not_y);
    bs_write(&s, 11, line);
    bs_write(&s, 12, 0xffe); /* as defined in rfc8331 */
    bs_write(&s, 10, did);
    bs_write(&s, 10, sdid);
    bs_write(&s, 10, dc);

    for (int i = 0; i < (dc & 0xff) + 1 /* CS */; i++) {
        bs_write(&s, 10, packet[gap*(6+i)]);
    }

    int pos = bs_pos(&s) & 7;
    if (pos)
        bs_write(&s, 8 - pos, 0xff);

    bs_flush(&s);

    anc_frame->len += bs_pos(&s) / 8;

    return UBASE_ERR_NONE;
}

struct sdi_line_range {
    uint16_t start;
    uint16_t end;
};

struct sdi_picture_fmt {
    bool interlaced;
    uint16_t active_height;

    /* Field 1 (interlaced) or Frame (progressive) line ranges */
    struct sdi_line_range vbi_f1_part1;
    struct sdi_line_range active_f1;
    struct sdi_line_range vbi_f1_part2;

    /* Field 2 (interlaced)  */
    struct sdi_line_range vbi_f2_part1;
    struct sdi_line_range active_f2;
    struct sdi_line_range vbi_f2_part2;
};

static const struct sdi_picture_fmt pict_fmts[] = {
    /* 1125 Interlaced (1080 active) lines */
    {true, 1080, {1, 20}, {21, 560}, {561, 563}, {564, 583}, {584, 1123}, {1124, 1125}},
    /* 1125 Progressive (1080 active) lines */
    {false, 1080, {1, 41}, {42, 1121}, {1122, 1125}, {0, 0}, {0, 0}, {0, 0}},
    /* 750 Progressive (720 active) lines */
    {false, 720, {1, 25}, {26, 745}, {746, 750}, {0, 0}, {0, 0}, {0, 0}},

    /* PAL */
    {true, 576, {1, 22}, {23, 310}, {311, 312}, {313, 335}, {336, 623}, {624, 625}},
    /* NTSC */
    {true, 486, {4, 19}, {20, 263}, {264, 265}, {266, 282}, {283, 525}, {1, 3}},
};

static int vanc_line_number(const struct sdi_picture_fmt *fmt, int line)
{
    line += fmt->vbi_f1_part1.start;

    if (line >= fmt->vbi_f1_part1.start && line <= fmt->vbi_f1_part1.end)
        return line;

    line += fmt->active_f1.end - fmt->active_f1.start + 1;

    if (line >= fmt->vbi_f1_part2.start && line <= fmt->vbi_f1_part2.end)
        return line;

    if (line >= fmt->vbi_f2_part1.start && line <= fmt->vbi_f2_part1.end)
        return line;

    line += fmt->active_f2.end - fmt->active_f2.start + 1;

    if (line >= fmt->vbi_f2_part2.start && line <= fmt->vbi_f2_part2.end)
        return line;

    line -= fmt->vbi_f2_part2.end; /* NTSC */
    return line;
}

struct sdi_offsets_fmt {
    uint16_t active_height;

    /* Number of samples (pairs) between EAV and start of active data */
    uint16_t active_offset;

    struct urational fps;
};

static const struct sdi_offsets_fmt fmts_data[] = {
    { 1080, 720, { 25, 1} },
    { 1080, 720, { 50, 1} },

    { 1080, 280, { 30000, 1001 } },
    { 1080, 280, { 60000, 1001 } },
    { 1080, 280, { 60, 1 } },

    { 1080, 830, { 24000, 1001 } },
    { 1080, 830, { 24, 1 } },

    { 720, 700, { 50, 1} },
    { 720, 370, { 60000, 1001 } },
    { 720, 370, { 60, 1 } },

    { 576, 144, { 25, 1} },
    { 486, 138, { 30000, 1001 } },
};

static int get_sav_offset(netmap_ctx_t *netmap_ctx)
{
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;

    for (size_t i = 0; i < sizeof(fmts_data)/sizeof(*fmts_data); i++) {
        const struct sdi_offsets_fmt *fmt = &fmts_data[i];
        if (netmap_opts->height != fmt->active_height)
            continue;

        if (netmap_opts->timebase_num != fmt->fps.den)
            continue;
        if (netmap_opts->timebase_den != fmt->fps.num)
            continue;

        return fmt->active_offset;
    }

    return 0;
}

static int restamp_audio_2110(struct uprobe *uprobe, struct upipe *upipe,
                              int event, va_list args)
{
    struct uref *flow_def;
    const char *def;
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_audio_t *audio = uprobe_obe->data;

    if (!audio->channels) {
        printf("[%d] no channels\n", audio->idx);
        return UBASE_ERR_NONE;
    }

    if (event != UPROBE_PROBE_UREF)
            return uprobe_throw_next(uprobe, upipe, event, args);

    UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
    struct uref *uref = va_arg(args, struct uref *);
    va_arg(args, struct upump **);
    bool *drop = va_arg(args, bool *);

    uint64_t cr_sys = 0, pts_orig = 0;
    uref_clock_get_cr_sys(uref, &cr_sys);
    uref_clock_get_pts_orig(uref, &pts_orig);
    // * 48000 / 27000000
    pts_orig = pts_orig * 2 / 1125;

    uint64_t ptp_rtp = cr_sys * 2 / 1125;
    uint32_t timestamp = pts_orig;
    uint32_t expected_timestamp = ptp_rtp;

    /* expected_timestamp > timestamp assuming timestamp is time of transmission
       FIXME: check timestamp goes forward */
    uint32_t diff = (UINT32_MAX + expected_timestamp -
                        (timestamp % UINT32_MAX)) % UINT32_MAX;

    uint64_t pts = (ptp_rtp - diff) * 1125 / 2;
    uref_clock_set_pts_sys(uref, pts);

    return UBASE_ERR_NONE;
}

static int catch_vanc(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    netmap_ctx_t *netmap_ctx = uprobe_obe->data;
    netmap_opts_t *netmap_opts = &netmap_ctx->netmap_opts;
    obe_t *h = netmap_ctx->h;

    if (event != UPROBE_PROBE_UREF)
        return uprobe_throw_next(uprobe, upipe, event, args);

    obe_output_stream_t *output_stream = get_output_stream_by_format(h, ANC_RAW);
    if (!output_stream)
        return UBASE_ERR_INVALID;

    UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
    struct uref *uref = va_arg(args, struct uref *);
    va_arg(args, struct upump **);
    va_arg(args, bool *);

    const struct sdi_picture_fmt *fmt = NULL;
    for (int i = 0; i < sizeof(pict_fmts) / sizeof(*pict_fmts); i++) {
        if (pict_fmts[i].interlaced != netmap_opts->interlaced)
            continue;
        if (pict_fmts[i].active_height != netmap_opts->height)
            continue;
        fmt = &pict_fmts[i];
        break;
    }

    if (!fmt) {
        upipe_err(upipe, "Unknown format");
        return UBASE_ERR_INVALID;
    }

    size_t hsize, vsize, stride;
    const uint8_t *r;
    if (unlikely(!ubase_check(uref_pic_size(uref, &hsize, &vsize, NULL)) ||
                 !ubase_check(uref_pic_plane_size(uref, "x10", &stride,
                                                  NULL, NULL, NULL)) ||
                 !ubase_check(uref_pic_plane_read(uref, "x10", 0, 0, -1, -1,
                                                  &r)))) {
        upipe_throw_error(upipe, UBASE_ERR_INVALID);
        return UBASE_ERR_INVALID;
    }

    for (unsigned int i = 0; i < vsize; i++) {
        r += stride;

        const uint16_t *x = (const uint16_t*)r;

        size_t h_left = hsize;
        while (h_left > S291_HEADER_SIZE + S291_FOOTER_SIZE) {
            if (x[0] != S291_ADF1 || x[1] != S291_ADF2 || x[2] != S291_ADF3) {
                break;
            }

            uint8_t did = s291_get_did(x);
            uint8_t sdid = s291_get_sdid(x);
            uint8_t dc = s291_get_dc(x);
            if (S291_HEADER_SIZE + dc + S291_FOOTER_SIZE > h_left) {
                upipe_dbg_va(upipe, "ancillary too large (%"PRIu8" > %zu) for 0x%"PRIx8"/0x%"PRIx8,
                        dc, h_left, did, sdid);
                break;
            }

            if (!s291_check_cs(x)) {
                upipe_dbg_va(upipe, "invalid CRC for 0x%"PRIx8"/0x%"PRIx8,
                        did, sdid);
                x += 3;
                h_left -= 3;
                continue;
            }

            if (!send_did_sdid(netmap_ctx, did, sdid)) {
                x += 3;
                h_left -= 3;
                continue;
            }

            obe_coded_frame_t *anc_frame = netmap_ctx->anc_frame;
            if (!anc_frame) {
                netmap_ctx->anc_frame = anc_frame = new_coded_frame(output_stream->output_stream_id, 65536);
                if (!anc_frame)
                    return UBASE_ERR_ALLOC;

                anc_frame->len = 0;
            }

            bool c_not_y = false;

            bs_t s;
            bs_init(&s, &anc_frame->data[anc_frame->len], 65536 - anc_frame->len);

            uint16_t horiz_offset = hsize - h_left;

            bs_write(&s, 6, 0);
            bs_write(&s, 1, c_not_y);
            bs_write(&s, 11, vanc_line_number(fmt, i));
            bs_write(&s, 12, horiz_offset);

            for (int j = 3; j < S291_HEADER_SIZE + (dc & 0xff) + 1 /* CS */; j++) {
                bs_write(&s, 10, x[j]);
            }

            int pos = bs_pos(&s) & 7;
            if (pos)
                bs_write(&s, 8 - pos, 0xff);

            bs_flush(&s);

            anc_frame->len += bs_pos(&s) / 8;

next:
            x += S291_HEADER_SIZE + dc + 1;
            h_left -= S291_HEADER_SIZE + dc + 1;
        }
    }

    uref_pic_plane_unmap(uref, "x10", 0, 0, -1, -1);

    return UBASE_ERR_NONE;
}

static int catch_null(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    struct uref *flow_def;
    const char *def;

    if (!uprobe_plumber(event, args, &flow_def, &def))
        return uprobe_throw_next(uprobe, upipe, event, args);

    struct upipe_mgr *null_mgr = upipe_null_mgr_alloc();
    struct upipe *pipe = upipe_void_alloc_output(upipe, null_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe), UPROBE_LOG_DEBUG, "null"));
    upipe_release(pipe);


    return UBASE_ERR_NONE;
}

static int restamp_rfc4175_video(struct uprobe *uprobe, struct upipe *upipe,
                               int event, va_list args)
{
    struct uref *flow_def;
    const char *def;
    struct uprobe_obe *uprobe_obe = uprobe_obe_from_uprobe(uprobe);
    if (event != UPROBE_PROBE_UREF)
        return uprobe_throw_next(uprobe, upipe, event, args);

    UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
    struct uref *uref = va_arg(args, struct uref *);
    va_arg(args, struct upump **);
    bool *drop = va_arg(args, bool *);

        uint64_t cr_sys = 0, pts_orig = 0;
        uref_clock_get_cr_sys(uref, &cr_sys);
        uref_clock_get_pts_orig(uref, &pts_orig);
        pts_orig = pts_orig / 300;

        uint64_t ptp_rtp = cr_sys / 300;
        uint32_t timestamp = pts_orig;
        uint32_t expected_timestamp = ptp_rtp;

        /* expected_timestamp > timestamp assuming timestamp is time of transmission
           FIXME: check timestamp goes forward */
        uint32_t diff = (UINT32_MAX + expected_timestamp -
                        (timestamp % UINT32_MAX)) % UINT32_MAX;

        /* Work in the 90kHz domain to avoid timestamp jitter */
        uint64_t vpts = (ptp_rtp - diff) * 300;
        uref_clock_set_pts_sys(uref, vpts);

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
            for (int i = 0; i < 16; i++) {
                netmap_audio_t *audio = &netmap_ctx->audio[i];
                if (audio->channels == 0)
                    break;
                upipe_release(audio->src[0]);
                upipe_release(audio->src[1]);
                upipe_release(audio->rtpr);
            }
        }
    }
    else
    {
        int active;

        pthread_mutex_lock( &h->device_mutex );
        netmap_ctx->stop = h->device.stop;
        active = h->device.input_status.active;
        pthread_mutex_unlock( &h->device_mutex);

        /* Note obe_mdate() is in microseconds */
        if ( netmap_ctx->last_frame_time > 0 && (obe_mdate() - netmap_ctx->last_frame_time) >= 1500000 &&
             active )
        {
            pthread_mutex_lock( &h->device_mutex );
            h->device.input_status.active = 0;
            pthread_mutex_unlock( &h->device_mutex);
        }

        if( netmap_ctx->stop )
        {
            for (int i = 0; i < 16; i++) {
                netmap_audio_t *audio = &netmap_ctx->audio[i];
                if (audio->channels == 0)
                    break;
                printf("audio release %u\n", i);
                upipe_release(audio->src[0]);
                upipe_release(audio->src[1]);
                upipe_release(audio->rtpr);
                audio->channels = 0;
            }

            upump_stop(upump);
            upump_free(upump);

            stop_no_video_timer(netmap_ctx);

            if( netmap_ctx->raw_frames )
               free( netmap_ctx->raw_frames );

            if( netmap_ctx->bars_hnd )
                close_bars( netmap_ctx->bars_hnd );

            /* Stored frames are not malloced */
            if( netmap_ctx->stored_video_frame.release_data )
                netmap_ctx->stored_video_frame.release_data( &netmap_ctx->stored_video_frame );

            if( netmap_ctx->stored_audio_frame.release_data )
                netmap_ctx->stored_audio_frame.release_data( &netmap_ctx->stored_audio_frame );

            upipe_release(netmap_ctx->upipe_main_src);
            //upipe_release(netmap_ctx->avsync);
        }
    }
}

static void setup_rfc_audio_channel(netmap_ctx_t *netmap_ctx, char *uri, char *uri2,
        netmap_audio_t *a, struct uprobe *uprobe_main, const int loglevel,
        struct uref *flow_def)
{
    struct upipe_mgr *rtpr_mgr = upipe_rtpr_mgr_alloc();
    struct upipe *rtpr = upipe_void_alloc(rtpr_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "rtpr"));
    assert(rtpr);
    upipe_mgr_release(rtpr_mgr);
    a->rtpr = rtpr;
    upipe_attach_uclock(rtpr);

    upipe_rtpr_set_delay(rtpr, RFC_LATENCY);

    for (int i = 0; i < 2; i++) {
        char *u = i ? uri2 : uri;
        if (!u)
            break;
        struct upipe_mgr *udpsrc_mgr = upipe_udpsrc_mgr_alloc();
        struct upipe *pcm_src = upipe_void_alloc(udpsrc_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pcm src"));
        assert(pcm_src);
        upipe_mgr_release(udpsrc_mgr);

        ubase_assert(upipe_set_uri(pcm_src, u));
        ubase_assert(upipe_attach_uclock(pcm_src));

        struct upipe *sub = upipe_void_alloc_output_sub(pcm_src, rtpr,
                uprobe_pfx_alloc_va(uprobe_use(uprobe_main),
                    loglevel, "rtpr sub"));
        upipe_release(sub);

        a->src[i] = pcm_src;
    }

    struct upipe_mgr *setflowdef_mgr = upipe_setflowdef_mgr_alloc();
    struct upipe *setflowdef = upipe_void_alloc_output(rtpr,
            setflowdef_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pcm setflowdef1"));
    assert(setflowdef);
    upipe_setflowdef_set_dict(setflowdef, flow_def);

    struct upipe_mgr *rtpd_mgr = upipe_rtpd_mgr_alloc();
    struct upipe *rtpd = upipe_void_chain_output(setflowdef, rtpd_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pcm src"));
    assert(rtpd);
    upipe_mgr_release(rtpd_mgr);

    setflowdef = upipe_void_chain_output(rtpd,
            setflowdef_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pcm setflowdef2"));
    assert(setflowdef);

    flow_def = uref_dup(flow_def);
    uref_flow_set_def(flow_def, "block.s24be.sound.");
    uref_sound_flow_set_rate(flow_def, 48000);
    uref_sound_flow_set_channels(flow_def, a->channels);
    uref_sound_flow_set_planes(flow_def, 0);
    uref_sound_flow_add_plane(flow_def, "all");
    upipe_setflowdef_set_dict(setflowdef, flow_def);
    uref_free(flow_def);

    upipe_mgr_release(setflowdef_mgr);

    struct upipe_mgr *pcm_unpack_mgr = upipe_rtp_pcm_unpack_mgr_alloc();
    struct upipe *pcm_unpack = upipe_void_chain_output(setflowdef,
            pcm_unpack_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pcm unpack"));
    assert(pcm_unpack);
    upipe_mgr_release(pcm_unpack_mgr);

    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    struct upipe *probe_uref_audio_restamp = upipe_void_chain_output(pcm_unpack,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_main),
                    restamp_audio_2110, a),
                loglevel, "audio probe_uref_restamp"));
    upipe_release(probe_uref_audio_restamp);

    struct upipe *audio = upipe_void_alloc_output_sub(probe_uref_audio_restamp, netmap_ctx->avsync,
            uprobe_pfx_alloc_va(uprobe_use(uprobe_main),
                loglevel, "sync audio %u", 0));
    upipe_release(audio);

    struct upipe *probe_uref_audio = upipe_void_alloc_output(audio,
                upipe_probe_uref_mgr,
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_main), catch_audio_2110, netmap_ctx),
                    loglevel, "audio probe_uref"));
    upipe_release(probe_uref_audio);
    upipe_mgr_release(upipe_probe_uref_mgr);
}

static int get_channels_from_uri(char *uri)
{
    char *slash = strrchr(uri, '/');
    while (slash && (slash[1] < '0' || slash[1] > '9')) {
        slash = strchr(&slash[1], '/');
    }
    if (!slash)
        return 0;
    *slash++ = '\0';
    return atoi(slash);
}

static int setup_rfc_audio(netmap_ctx_t *netmap_ctx, struct uref_mgr *uref_mgr,
    struct uprobe *uprobe_main, const int loglevel, char *audio)
{
    struct uref *flow_def = uref_alloc(uref_mgr);
    uref_sound_flow_set_rate(flow_def, 48000);

    unsigned i = 0;
    while (audio) {
        char *next = strchr(audio, ';');
        if (next)
            *next++ = '\0';

        char *path2 = strchr(audio, '|');
        if (path2)
            *path2++ = '\0';

        unsigned channels = 16; //FIXME
        if (!channels) {
            printf("audio URI missing channels\n");
            uref_free(flow_def);
            return 1;
        }

        assert((channels & 1) == 0);

        netmap_audio_t *a = &netmap_ctx->audio[i++];
        a->idx = netmap_ctx->channels/2;
        a->channels = channels;

        setup_rfc_audio_channel(netmap_ctx, audio, path2, a, uprobe_main,
                loglevel, flow_def);

        audio = next;
        netmap_ctx->channels += channels;
    }
    uref_free(flow_def);

    return 0;
}

static int open_netmap( netmap_ctx_t *netmap_ctx )
{
    char *uri = netmap_ctx->uri;
    char *ptp_nic = netmap_ctx->ptp_nic;

    netmap_ctx->detected_video_format = -1;
    netmap_ctx->input_chroma_map[0] = "y10l";
    netmap_ctx->input_chroma_map[1] = "u10l";
    netmap_ctx->input_chroma_map[2] = "v10l";
    netmap_ctx->input_chroma_map[3] = NULL;

    /* upump manager for the main thread */
    struct upump_mgr *main_upump_mgr = upump_ev_mgr_alloc_loop(UPUMP_POOL, UPUMP_BLOCKER_POOL);
    assert(main_upump_mgr);
    netmap_ctx->upump_mgr = main_upump_mgr;
    netmap_ctx->no_video_upump = NULL;
    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    udict_mgr_release(udict_mgr);

    enum uprobe_log_level loglevel = UPROBE_LOG_WARNING;

    /* probes */
    /* main (thread-safe) probe, whose first element is uprobe_pthread_upump_mgr */
    struct uprobe *uprobe_main = uprobe_stdio_alloc(NULL, stdout, loglevel);
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
    const char *intf[2] = { NULL, NULL };
    char *next = strchr( ptp_nic, '|' );
    if( next )
    {
        *next++ = '\0';
        intf[1] = next;
    }
    intf[0] = ptp_nic;

    struct uclock *uclock = uclock_ptp_alloc(uprobe_main, intf);
    assert(uclock);
    uprobe_main = uprobe_uclock_alloc(uprobe_main, uclock);
    uclock_release(uclock);
    assert(uprobe_main);

    struct uprobe *uprobe_dejitter =
        uprobe_dejitter_alloc(uprobe_use(uprobe_main), !netmap_ctx->rfc4175, 1);

    uprobe_throw(uprobe_main, NULL, UPROBE_FREEZE_UPUMP_MGR);

    /* netmap source */
    bool pciesdi = *uri == '/';
    struct upipe_mgr *src_mgr = pciesdi ? upipe_pciesdi_src_mgr_alloc()
        : upipe_netmap_source_mgr_alloc();
    if (netmap_ctx->rfc4175) {
        struct uref *uref = uref_alloc(uref_mgr);
        uref_flow_set_def(uref, "pic.");
        obe_int_input_stream_t *video_stream = get_input_stream(netmap_ctx->h, 0);
        uref_pic_flow_set_vsize(uref, video_stream->height);
        uref_pic_flow_set_hsize(uref, video_stream->width);
        uref_pic_flow_set_macropixel(uref, 1);
        uref_pic_flow_add_plane(uref, 1, 1, 2, "y10l");
        uref_pic_flow_add_plane(uref, 2, 1, 2, "u10l");
        uref_pic_flow_add_plane(uref, 2, 1, 2, "v10l");

        struct urational fps = {netmap_ctx->v_timebase.den, netmap_ctx->v_timebase.num};
        uref_pic_flow_set_fps(uref, fps);
        if (!video_stream->interlaced)
            uref_pic_set_progressive(uref);

        netmap_ctx->upipe_main_src = upipe_flow_alloc(src_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main),
                    loglevel, "netmap source"), uref);
        uref_free(uref);
    } else {
        netmap_ctx->upipe_main_src = upipe_void_alloc(src_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main),
                    loglevel, "netmap source"));
    }
    upipe_mgr_release(src_mgr);

    assert(netmap_ctx->upipe_main_src);
    upipe_attach_uclock(netmap_ctx->upipe_main_src);
    if (!ubase_check(upipe_set_uri(netmap_ctx->upipe_main_src, uri))) {
        return 2;
    }

    uprobe_throw(uprobe_main, NULL, UPROBE_THAW_UPUMP_MGR);

    pthread_attr_t *thread_attribs_ptr = NULL;
    pthread_attr_t thread_attribs;
    struct sched_param params;

    thread_attribs_ptr = &thread_attribs;
    pthread_attr_init(&thread_attribs);
    pthread_attr_setschedpolicy(&thread_attribs, SCHED_FIFO);
    pthread_attr_setinheritsched(&thread_attribs, PTHREAD_EXPLICIT_SCHED);

    pthread_attr_getschedparam (&thread_attribs, &params);
    params.sched_priority = sched_get_priority_max(SCHED_FIFO);;
    int ret = pthread_attr_setschedparam(&thread_attribs, &params);
    if (ret < 0)
        perror("setschedparam");

    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(encoder_id + 1, &cs);

    if (pthread_attr_setaffinity_np(&thread_attribs, sizeof(cs), &cs)) {
        perror("pthread_attr_setaffinity_np");
    }

    struct upipe_mgr *xfer_mgr =  upipe_pthread_xfer_mgr_alloc(XFER_QUEUE,
            XFER_POOL, uprobe_use(uprobe_main_pthread), upump_ev_mgr_alloc_loop,
            UPUMP_POOL, UPUMP_BLOCKER_POOL, NULL, NULL, thread_attribs_ptr);
    assert(xfer_mgr != NULL);

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

    /* in rfc4175 mode, netmap_src outputs pictures */
    struct upipe *sdi_dec;
    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    if (!netmap_ctx->rfc4175) {
        /* sdi dec to y10 */
        struct upipe_mgr *upipe_sdi_dec_mgr = upipe_sdi_dec_mgr_alloc();
        struct uref *uref = uref_alloc(uref_mgr);
        uref_flow_set_def(uref, "pic.");
        uref_pic_flow_set_macropixel(uref, 1);
        uref_pic_flow_add_plane(uref, 1, 1, 2, "y10l");
        uref_pic_flow_add_plane(uref, 2, 1, 2, "u10l");
        uref_pic_flow_add_plane(uref, 2, 1, 2, "v10l");

        sdi_dec = upipe_sdi_dec_alloc_output(netmap_ctx->upipe_main_src,
                upipe_sdi_dec_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec"),
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec vanc"),
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec vbi"),
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "sdi_dec audio"),
                uref);
        uref_free(uref);
        //upipe_set_option(sdi_dec, "debug", "1");
        upipe_mgr_release(upipe_sdi_dec_mgr);
    } else {
        sdi_dec = upipe_void_alloc_output(netmap_ctx->upipe_main_src,
                upipe_probe_uref_mgr,
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), restamp_rfc4175_video, netmap_ctx),
                loglevel, "probe_uref_rfc4175_video"));

        struct upipe_mgr *upipe_sync_mgr = upipe_sync_mgr_alloc();
        netmap_ctx->avsync = upipe_void_chain_output(sdi_dec,
                upipe_sync_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "avsync"));
        assert(netmap_ctx->avsync);
        upipe_attach_uclock(netmap_ctx->avsync);
        upipe_mgr_release(upipe_sync_mgr);
        sdi_dec = netmap_ctx->avsync;
    }

    /* video callback */
    struct upipe *probe_uref_video = upipe_void_chain_output(sdi_dec,
            upipe_probe_uref_mgr,
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_main), catch_video, netmap_ctx),
            loglevel, "probe_uref_video"));
    upipe_release(probe_uref_video);

    if (!netmap_ctx->rfc4175) {
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
        netmap_ctx->channels = 16;
        struct upipe *probe_uref_audio = upipe_void_alloc_output(audio,
                upipe_probe_uref_mgr,
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_audio_hbrmt, netmap_ctx),
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

        /* vanc filter */
        struct upipe_mgr *upipe_filter_vanc_mgr = upipe_vanc_mgr_alloc();

        struct upipe *probe_vanc = upipe_void_alloc_output(vanc,
                upipe_probe_uref_mgr,
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_vanc, netmap_ctx),
                UPROBE_LOG_DEBUG, "probe_uref_ttx"));

        probe_vanc = upipe_vanc_chain_output(probe_vanc,
                upipe_filter_vanc_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "vanc filter"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_null, netmap_ctx),
                UPROBE_LOG_DEBUG, "afd"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_null, netmap_ctx),
                UPROBE_LOG_DEBUG, "scte104"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_ttx, netmap_ctx),
                UPROBE_LOG_DEBUG, "op47"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_null, netmap_ctx),
                    UPROBE_LOG_DEBUG, "cea708"));
        upipe_release(probe_vanc);
        upipe_mgr_release(upipe_filter_vanc_mgr);

        /* vbi */
        struct upipe *vbi = NULL;
        if (!ubase_check(upipe_sdi_dec_get_vbi_sub(sdi_dec, &vbi))) {
            printf("NO vbi\n");
            return 1;
        }
        else {
            upipe_release(vbi);
        }

        /* vbi filter */
        struct upipe_mgr *upipe_filter_vbi_mgr = upipe_vbi_mgr_alloc();
        struct upipe *probe_vbi = upipe_vbi_alloc_output(vbi,
                upipe_filter_vbi_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "vbi filter"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_ttx, netmap_ctx),
                UPROBE_LOG_DEBUG, "vbi_ttx"),
                uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_null, netmap_ctx),
                    UPROBE_LOG_DEBUG, "vbi_cea708"));
        upipe_release(probe_vbi);
        upipe_mgr_release(upipe_filter_vbi_mgr);
    }
    else
    {
        char *audio = netmap_ctx->audio_uri;
        if (!audio) {
            printf("Missing audio URI\n");
            return 1;
        }

        if (setup_rfc_audio(netmap_ctx, uref_mgr, uprobe_main, loglevel, audio))
            return 1;
    }
    upipe_mgr_release(upipe_probe_uref_mgr);

    static struct upump *event_upump;
    /* stop timer */
    event_upump = upump_alloc_timer(main_upump_mgr, upipe_event_timer, netmap_ctx, NULL,
                                    UCLOCK_FREQ/2, UCLOCK_FREQ/2);
    assert(event_upump != NULL);
    upump_start(event_upump);

    netmap_ctx->no_video_upump = NULL;
    setup_picture_on_signal_loss_timer(netmap_ctx);

    /* main loop */
    upump_mgr_run(main_upump_mgr, NULL);

    /* Wait on all upumps */
    upump_mgr_release(main_upump_mgr);
    uprobe_release(uprobe_main);
    uprobe_release(uprobe_dejitter);

    if (netmap_ctx->anc_frame)
        destroy_coded_frame(netmap_ctx->anc_frame);

    return 0;

}

static void *autoconf_input( void *ptr )
{
    obe_int_input_stream_t *streams[MAX_STREAMS];
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
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

    h->device.num_input_streams = 2;
    memcpy( h->device.streams, streams, h->device.num_input_streams * sizeof(obe_int_input_stream_t**) );
    h->device.device_type = INPUT_DEVICE_NETMAP;
    memcpy( &h->device.user_opts, user_opts, sizeof(*user_opts) );

    return NULL;
}

static void *probe_input( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;

    netmap_ctx_t netmap_ctx = {0};
    netmap_ctx.uri = user_opts->netmap_uri;
    netmap_ctx.audio_uri = user_opts->netmap_audio;
    netmap_ctx.rfc4175 = user_opts->netmap_mode && !strcmp(user_opts->netmap_mode, "rfc4175");
    netmap_ctx.h = h;
    netmap_ctx.ptp_nic = user_opts->ptp_nic;
    netmap_opts_t *netmap_opts = &netmap_ctx.netmap_opts;
    netmap_opts->probe = netmap_ctx.non_display_parser.probe = 1;

    open_netmap( &netmap_ctx );

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

            add_non_display_services( &netmap_ctx.non_display_parser, streams[i], USER_DATA_LOCATION_FRAME );
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

    if( netmap_ctx.non_display_parser.has_ttx_frame )
    {
        streams[cur_stream] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[cur_stream]) );
        if( !streams[cur_stream] )
            goto finish;

        streams[cur_stream]->input_stream_id = cur_input_stream_id++;

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = MISC_TELETEXT;
        if( add_teletext_service( &netmap_ctx.non_display_parser, streams[cur_stream] ) < 0 )
            goto finish;
        cur_stream++;
    }

    streams[cur_stream] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[cur_stream]) );
    if( !streams[cur_stream] )
        goto finish;

    streams[cur_stream]->input_stream_id = cur_input_stream_id++;

    streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
    streams[cur_stream]->stream_format = ANC_RAW;
    cur_stream++;

    free( netmap_ctx.non_display_parser.frame_data );

    init_device(&h->device);
    h->device.num_input_streams = cur_stream;
    memcpy( h->device.streams, streams, h->device.num_input_streams * sizeof(obe_int_input_stream_t**) );
    h->device.device_type = INPUT_DEVICE_NETMAP;
    memcpy( &h->device.user_opts, user_opts, sizeof(*user_opts) );

finish:

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_input_t *user_opts = &h->device.user_opts;

    netmap_ctx_t netmap_ctx = {0};
    netmap_opts_t *netmap_opts = &netmap_ctx.netmap_opts;
    netmap_opts->video_format = user_opts->video_format;
    netmap_opts->picture_on_loss = user_opts->picture_on_loss;
    //netmap_opts->downscale = user_opts->downscale;

    netmap_opts->obe_bars_opts.video_format = user_opts->video_format;
    netmap_opts->obe_bars_opts.bars_line1 = user_opts->bars_line1;
    netmap_opts->obe_bars_opts.bars_line2 = user_opts->bars_line2;
    netmap_opts->obe_bars_opts.bars_line3 = user_opts->bars_line3;
    netmap_opts->obe_bars_opts.bars_line4 = user_opts->bars_line4;
    netmap_opts->obe_bars_opts.no_signal = 1;

    netmap_ctx.uri = user_opts->netmap_uri;
    netmap_ctx.audio_uri = user_opts->netmap_audio;
    netmap_ctx.rfc4175 = user_opts->netmap_mode && !strcmp(user_opts->netmap_mode, "rfc4175");
    netmap_ctx.ptp_nic = user_opts->ptp_nic;
    netmap_ctx.h = h;

    int j = 0;
    for( ; video_format_tab[j].obe_name != -1; j++ )
    {
        if( netmap_opts->video_format == video_format_tab[j].obe_name )
            break;
    }

    netmap_ctx.v_timebase.num = video_format_tab[j].timebase_num;
    netmap_ctx.v_timebase.den = video_format_tab[j].timebase_den;
    netmap_ctx.video_freq = av_rescale_q( 1, netmap_ctx.v_timebase, (AVRational){1, OBE_CLOCK} );

    netmap_ctx.a_timebase.num = 1;
    netmap_ctx.a_timebase.den = 48000;

    if( netmap_opts->picture_on_loss )
    {
        netmap_ctx.sample_pattern = get_sample_pattern( netmap_opts->video_format );
        if( !netmap_ctx.sample_pattern )
        {
            fprintf( stderr, "[netmap] Invalid sample pattern" );
            return NULL;
        }

        /* Setup Picture on Loss */
        if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
            netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
        {
            setup_stored_video_frame( &netmap_ctx.stored_video_frame, video_format_tab[j].width,
                                       video_format_tab[j].height );
            blank_yuv422p_frame( &netmap_ctx.stored_video_frame );
        }
        else if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BARS )
        {
            netmap_ctx.raw_frames = (obe_raw_frame_t**)calloc( 2, sizeof(*netmap_ctx.raw_frames) );
            if( !netmap_ctx.raw_frames )
            {
                fprintf( stderr, "[netmap] Malloc failed\n" );
                return NULL;
            }

            netmap_opts->obe_bars_opts.video_format = netmap_opts->video_format;
            /* Setup bars later if we don't know the video format */
            if( open_bars( &netmap_ctx.bars_hnd, &netmap_opts->obe_bars_opts ) < 0 )
            {
                fprintf( stderr, "[netmap] Could not open bars\n" );
                return NULL;
            }
        }

        /* Setup stored audio frame */
        if( netmap_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
            netmap_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
        {
            setup_stored_audio_frame( &netmap_ctx.stored_audio_frame, netmap_ctx.sample_pattern->max );
        }
    }

    open_netmap( &netmap_ctx );

    free(ptr);

    return NULL;
}

const obe_input_func_t netmap_input = { probe_input, autoconf_input, open_input };
