/*****************************************************************************
 * smoothing.c : Video encoder output smoothing
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
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
#include "smoothing.h"

static void *start_smoothing( void *ptr )
{
    obe_t *h = ptr;
    int num_smoothing_frames = 0, ready = 0, buffer_frames = 0;
    int64_t start_mpeg_time = 0, start_dts_time = 0, last_clock = -1, last_dts = -1;
    obe_coded_frame_t *coded_frame = NULL;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->is_video )
        {
            pthread_mutex_lock( &h->encoders[i]->encoder_mutex );
            if( !h->encoders[i]->is_ready )
                pthread_cond_wait( &h->encoders[i]->encoder_cv, &h->encoders[i]->encoder_mutex );
            x264_param_t *params = h->encoders[i]->encoder_params;
            buffer_frames = params->sc.i_buffer_size;
            pthread_mutex_unlock( &h->encoders[i]->encoder_mutex );
            break;
        }
    }

    int64_t send_delta = 0;

    while( 1 )
    {
        pthread_mutex_lock( &h->smoothing_mutex );

        if( h->cancel_smoothing_thread )
        {
            pthread_mutex_unlock( &h->smoothing_mutex );
            break;
        }

        if( h->num_smoothing_frames == num_smoothing_frames )
        {
            if( ready )
                printf("\n smoothing wait underflow \n" );
            pthread_cond_wait( &h->smoothing_cv, &h->smoothing_mutex );
        }

        if( h->cancel_smoothing_thread )
        {
            pthread_mutex_unlock( &h->smoothing_mutex );
            break;
        }

        num_smoothing_frames = h->num_smoothing_frames;

        /* Refill the buffer after a drop */
        pthread_mutex_lock( &h->drop_mutex );
        if( h->smoothing_drop )
        {
            syslog( LOG_INFO, "Smoothing buffer reset\n" );
            ready = h->smoothing_drop = 0;
            last_clock = -1;
        }
        pthread_mutex_unlock( &h->drop_mutex );

        if( !ready )
        {
            if( num_smoothing_frames >= buffer_frames )
                ready = 1;
            else
            {
                pthread_mutex_unlock( &h->smoothing_mutex );
                continue;
            }
        }

        printf("\n smoothed frames %i \n", num_smoothing_frames );

        coded_frame = h->smoothing_frames[0];

        pthread_mutex_unlock( &h->smoothing_mutex );

        if( last_clock != -1 )
        {
            sleep_mpeg_ticks( coded_frame->real_dts - start_dts_time + start_mpeg_time );
        }

        if( last_clock == -1 )
        {
            start_mpeg_time = get_wallclock_in_mpeg_ticks();
            start_dts_time = coded_frame->real_dts;
        }

        last_clock = get_wallclock_in_mpeg_ticks();
        last_dts = coded_frame->real_dts;

        add_to_mux_queue( h, coded_frame );

        //printf("\n send_delta %"PRIi64" \n", obe_mdate() - send_delta );
        //send_delta = obe_mdate();

        remove_from_smoothing_queue( h );
        num_smoothing_frames = 0;
    }

    return NULL;
}

const obe_smoothing_func_t x264_smoothing = { start_smoothing };
