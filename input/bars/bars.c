/*****************************************************************************
 * bars.c: SMPTE bars input
 *****************************************************************************
 * Copyright (C) 2014 Open Broadcast Systems Ltd.
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
#include <libavutil/samplefmt.h>
#include "bars_common.h"

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
                fprintf( stderr, "[bars] Unsupported video format\n" );
                return NULL;
            }

            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->video_format = user_opts->video_format;
            streams[i]->width  = video_format_tab[j].width;
            streams[i]->height = video_format_tab[j].height;
            streams[i]->timebase_num = video_format_tab[j].timebase_num;
            streams[i]->timebase_den = video_format_tab[j].timebase_den;
            streams[i]->csp    = PIX_FMT_YUV422P10;
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
    device->device_type = INPUT_DEVICE_BARS;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );

    /* add device */
    memcpy( &h->device, device, sizeof(*device) );
    free( device );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_input_t *user_opts = &h->device.user_opts;
    hnd_t bars_handle = NULL;
    obe_bars_opts_t obe_bars_opts = {0};
    obe_bars_opts.video_format = user_opts->video_format;

    if( obe_bars_opts.video_format == INPUT_VIDEO_FORMAT_AUTODETECT )
        obe_bars_opts.video_format = INPUT_VIDEO_FORMAT_PAL;

    obe_bars_opts.bars_line1 = user_opts->bars_line1;
    obe_bars_opts.bars_line2 = user_opts->bars_line2;
    obe_bars_opts.bars_line3 = user_opts->bars_line3;
    obe_bars_opts.bars_line4 = user_opts->bars_line4;

    if( open_bars( &bars_handle, &obe_bars_opts ) < 0 )
    {
        fprintf( stderr, "Could not open bars \n" );
        return NULL;
    }

    obe_raw_frame_t **raw_frames;
    raw_frames = malloc( 2 * sizeof(*raw_frames ) );
    if( !raw_frames )
    {
        fprintf( stderr, "malloc failed \n" );
        return NULL;
    }

    h->device.active = 1;

    int64_t start_time = 0;

    while( 1 )
    {
        get_bars( bars_handle, raw_frames );
        if( start_time == 0 )
            start_time = get_wallclock_in_mpeg_ticks();
        else
            sleep_mpeg_ticks( start_time + raw_frames[0]->pts );

        obe_clock_tick( h, raw_frames[0]->pts );

        if( add_to_filter_queue( h, raw_frames[0] ) < 0 )
            return NULL;

        for( int i = 0; i < h->device.num_input_streams; i++ )
        {
            if( h->device.streams[i]->stream_format == AUDIO_PCM )
                raw_frames[1]->input_stream_id = h->device.streams[i]->input_stream_id;
        }

        if( add_to_filter_queue( h, raw_frames[1] ) < 0 )
            return NULL;
    }

    close_bars( bars_handle );
    // FIXME upon thread kill free text strings

    return NULL;
}

const obe_input_func_t bars_input = { autoconf_input, autoconf_input, open_input };
