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
#include <upipe-pciesdi/upipe_pciesdi_source.h>
#include <upipe-hbrmt/upipe_pciesdi_source_framer.h>
#include <upipe-filters/upipe_filter_vanc.h>
#include <upipe/uref_dump.h>

#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#include <bitstream/dvb/vbi.h>

#define UMEM_POOL               512
#define UDICT_POOL_DEPTH        500
#define UREF_POOL_DEPTH         500
#define UBUF_POOL_DEPTH         3000
#define UBUF_SHARED_POOL_DEPTH  50
#define UPUMP_POOL              10
#define UPUMP_BLOCKER_POOL      10
#define XFER_QUEUE              255
#define XFER_POOL               20

typedef struct
{
    int probe;
    int video_format;
    int picture_on_loss;
    obe_bars_opts_t obe_bars_opts;

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
    int64_t         a_counter;
    AVRational      a_timebase;
    const obe_audio_sample_pattern_t *sample_pattern;
    int64_t         a_errors;
    uint8_t         channels;

    int64_t last_frame_time;

    netmap_opts_t netmap_opts;

    /* Upipe events */
    struct upump_mgr *upump_mgr;

    struct upump *no_video_upump;

    int stop;

    obe_sdi_non_display_data_t non_display_parser;

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

static void setup_picture_on_signal_loss_timer(netmap_ctx_t *netmap_ctx)
{
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
        netmap_opts->height = netmap_opts->coded_height = vsize;
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

                goto end;
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

            if (netmap_ctx->no_video_upump) {
                upump_stop(netmap_ctx->no_video_upump);
                upump_free(netmap_ctx->no_video_upump);
            }
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

static int catch_audio(struct uprobe *uprobe, struct upipe *upipe,
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

            bool discontinuity = ubase_check(uref_flow_get_discontinuity(uref));
            if (discontinuity)
                goto end;

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

static int catch_vanc(struct uprobe *uprobe, struct upipe *upipe,
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
            upump_stop(upump);
            upump_free(upump);

            if (netmap_ctx->no_video_upump) {
                upump_stop(netmap_ctx->no_video_upump);
                upump_free(netmap_ctx->no_video_upump);
            }

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
        }
    }
}

static int open_netmap( netmap_ctx_t *netmap_ctx )
{
    char *uri = netmap_ctx->uri;

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
    struct uclock *uclock = uclock_std_alloc(0);
    assert(uclock);
    uprobe_main = uprobe_uclock_alloc(uprobe_main, uclock);
    uclock_release(uclock);
    assert(uprobe_main);

    struct uprobe *uprobe_dejitter =
        uprobe_dejitter_alloc(uprobe_use(uprobe_main), true, 1);

    uprobe_throw(uprobe_main, NULL, UPROBE_FREEZE_UPUMP_MGR);

    /* netmap source */
    bool pciesdi = *uri == '/';
    struct upipe_mgr *src_mgr = pciesdi ? upipe_pciesdi_src_mgr_alloc()
        : upipe_netmap_source_mgr_alloc();
    netmap_ctx->upipe_main_src = upipe_void_alloc(src_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_main),
                loglevel, "netmap source"));
    upipe_mgr_release(src_mgr);
    upipe_attach_uclock(netmap_ctx->upipe_main_src);
    if (!ubase_check(upipe_set_uri(netmap_ctx->upipe_main_src, uri))) {
        return 2;
    }

    if (pciesdi) {
        struct upipe_mgr *upipe_mgr = upipe_pciesdi_source_framer_mgr_alloc();
        struct upipe *pipe = upipe_void_alloc_output(netmap_ctx->upipe_main_src, upipe_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main), loglevel, "pciesdi_source_framer"));
        assert(pipe);
        upipe_mgr_release(upipe_mgr);
        upipe_release(pipe);
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

    upipe_mgr_release(upipe_probe_uref_mgr);

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
    struct upipe *probe_vanc = upipe_vanc_alloc_output(vanc,
            upipe_filter_vanc_mgr,
            uprobe_pfx_alloc(uprobe_use(uprobe_dejitter), loglevel, "vanc filter"),
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_vanc, netmap_ctx),
               UPROBE_LOG_DEBUG, "afd"),
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_vanc, netmap_ctx),
               UPROBE_LOG_DEBUG, "scte104"),
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_ttx, netmap_ctx),
               UPROBE_LOG_DEBUG, "op47"),
            uprobe_pfx_alloc(uprobe_obe_alloc(uprobe_use(uprobe_dejitter), catch_vanc, netmap_ctx),
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

    static struct upump *event_upump;
    /* stop timer */
    event_upump = upump_alloc_timer(main_upump_mgr, upipe_event_timer, netmap_ctx, NULL,
                                    UCLOCK_FREQ/2, UCLOCK_FREQ/2);
    assert(event_upump != NULL);
    upump_start(event_upump);

    setup_picture_on_signal_loss_timer(netmap_ctx);

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
    netmap_ctx.h = h;
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

    return NULL;
}

const obe_input_func_t netmap_input = { probe_input, autoconf_input, open_input };
