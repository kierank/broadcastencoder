/*****************************************************************************
 * audio.c: basic audio filtering system
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd
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
 */

#include "common/common.h"
#include "audio.h"

static void *start_filter( void *ptr )
{
    obe_raw_frame_t *raw_frame, *split_raw_frame;
    obe_aud_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_output_stream_t *output_stream;
    int num_channels;

    while( 1 )
    {
        pthread_mutex_lock( &filter->queue.mutex );

        while( !filter->queue.size && !filter->cancel_thread )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            break;
        }

        raw_frame = filter->queue.queue[0];
        pthread_mutex_unlock( &filter->queue.mutex );

        /* ignore the video track */
        for( int i = 1; i < h->num_encoders; i++ )
        {
            output_stream = get_output_stream( h, h->encoders[i]->output_stream_id );
            num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

            split_raw_frame = new_raw_frame();
            if( !split_raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }
            memcpy( split_raw_frame, raw_frame, sizeof(*split_raw_frame) );
            memset( split_raw_frame->audio_frame.audio_data, 0, sizeof(split_raw_frame->audio_frame.audio_data) );
            split_raw_frame->audio_frame.linesize = split_raw_frame->audio_frame.num_channels = 0;
            split_raw_frame->audio_frame.channel_layout = output_stream->channel_layout;

            if( av_samples_alloc( split_raw_frame->audio_frame.audio_data, &split_raw_frame->audio_frame.linesize, num_channels,
                                  split_raw_frame->audio_frame.num_samples, split_raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }

            /* TODO: offset the channel pointers by the user's request */
            av_samples_copy( split_raw_frame->audio_frame.audio_data,
                             &raw_frame->audio_frame.audio_data[((output_stream->sdi_audio_pair-1)<<1)+output_stream->mono_channel], 0, 0,
                             split_raw_frame->audio_frame.num_samples, num_channels, split_raw_frame->audio_frame.sample_fmt );

            split_raw_frame->pts += (int64_t)output_stream->audio_offset * OBE_CLOCK/1000;

            add_to_encode_queue( h, split_raw_frame, h->encoders[i]->output_stream_id );
        }

        remove_from_queue( &filter->queue );
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        raw_frame = NULL;
    }

    free( filter_params );

    return NULL;
}

const obe_aud_filter_func_t audio_filter = { start_filter };
