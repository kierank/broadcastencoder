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

void *start_filter( void *ptr )
{
    obe_raw_frame_t *raw_frame, *split_raw_frame;
    obe_t *h = filter_params->h;

    while( 1 )
    {
        pthread_mutex_lock( &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            goto end;
        }

        if( !filter->queue.size )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            goto end;
        }

        raw_frame = filter->queue.queue[0];
        pthread_mutex_unlock( &filter->queue.mutex );

        /* ignore the video track */
        for( int i = 1; i < h->num_encoders; i++ )
        {
            split_raw_frame = new_raw_frame();
            if( !split_raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            memcpy( split_raw_frame, raw_frame, sizeof(*split_raw_frame) );
            split_raw_frame->audio_frame.audio_data = split_raw_frame->audio_frame.linesize = NULL;
            split_raw_frame->num_channels = 0;
            split_raw_frame->channel_layout = AV_CH_LAYOUT_STEREO;

            if( av_samples_alloc( split_raw_frame->audio_frame.audio_data, split_raw_frame->audio_frame.linesize, 2,
                                  split_raw_frame->audio_frame.num_samples, (AVSampleFormat)split_raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            /* TODO: offset the channel pointers by the user's request */
            av_samples_copy( split_raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.audio_data[0], 0, 0,
                             split_raw_frame->audio_frame.num_samples, 2, split_raw_frame->audio_frame.sample_fmt );

            add_to_encode_queue( h, split_raw_frame, 1 ); // FIXME
        }

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        raw_frame = NULL;
    }

    return NULL;
}

const obe_aud_filter_func_t audio_filter = { start_filter };
