/*****************************************************************************
 * /encoders/video/smoothing.c : Video encoder output smoothing
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

static void *start_smoothing( void *ptr )
{
    obe_t *h = ptr;
    int num_enc_smoothing_frames = 0, buffer_frames = 0, double_output = 0, release_late_frame = 0;
    int64_t start_dts = -1, start_pts = -1, last_clock = -1;
    obe_coded_frame_t *coded_frame[2] = { NULL };
    obe_output_stream_t *output_stream = NULL;

    struct sched_param param = {0};
    param.sched_priority = 50;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    /* FIXME: when we have soft pulldown this will need changing */
    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->is_video )
        {
            pthread_mutex_lock( &h->encoders[i]->queue.mutex );
            while( !h->encoders[i]->is_ready )
                pthread_cond_wait( &h->encoders[i]->queue.in_cv, &h->encoders[i]->queue.mutex );
            output_stream = get_output_stream( h, h->encoders[i]->output_stream_id );
            if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
            {
                x264_param_t *params = &output_stream->avc_param;
                buffer_frames = params->sc.i_buffer_size;
            }
            pthread_mutex_unlock( &h->encoders[i]->queue.mutex );
            break;
        }
    }

    double_output = output_stream->deinterlace_mode == 2;
    if( double_output )
        buffer_frames = 2;

    int64_t send_delta = 0;

    while( 1 )
    {
        pthread_mutex_lock( &h->enc_smoothing_queue.mutex );

        while( ulist_depth( &h->enc_smoothing_queue.ulist ) == num_enc_smoothing_frames && !h->cancel_enc_smoothing_thread )
            pthread_cond_wait( &h->enc_smoothing_queue.in_cv, &h->enc_smoothing_queue.mutex );

        if( h->cancel_enc_smoothing_thread )
        {
            pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
            break;
        }

        num_enc_smoothing_frames = ulist_depth( &h->enc_smoothing_queue.ulist );

        if( !h->enc_smoothing_buffer_complete )
        {
            if( num_enc_smoothing_frames >= buffer_frames )
            {
                h->enc_smoothing_buffer_complete = 1;
                start_dts = -1;
            }
            else
            {
                pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
                continue;
            }
        }

        //printf("\n smoothed frames %i \n", num_enc_smoothing_frames );

        coded_frame[0] = obe_coded_frame_t_from_uchain( ulist_pop( &h->enc_smoothing_queue.ulist ) );
        if( double_output && !release_late_frame )
        {
            if( num_enc_smoothing_frames > 1 )
                coded_frame[1] = obe_coded_frame_t_from_uchain( ulist_pop( &h->enc_smoothing_queue.ulist ) );
        }
        pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );

        /* The terminology can be a cause for confusion:
         *   pts refers to the pts from the input which is monotonic
         *   dts refers to the dts out of the encoder which is monotonic */

        pthread_mutex_lock( &h->obe_clock_mutex );

        //printf("\n dts gap %"PRIi64" \n", coded_frame[0]->real_dts - start_dts );
        //printf("\n pts gap %"PRIi64" \n", h->obe_clock_last_pts - start_pts );

        last_clock = h->obe_clock_last_pts;

        if( release_late_frame )
            release_late_frame = 0;
        else if( start_dts == -1 )
        {
            start_dts = coded_frame[0]->real_dts;
            /* Wait until the next clock tick */
            while( last_clock == h->obe_clock_last_pts && !h->cancel_enc_smoothing_thread )
                pthread_cond_wait( &h->obe_clock_cv, &h->obe_clock_mutex );
            start_pts = h->obe_clock_last_pts;
        }
        else if( coded_frame[0]->real_dts - start_dts > h->obe_clock_last_pts - start_pts )
        {
            //printf("\n waiting \n");
            while( last_clock == h->obe_clock_last_pts && !h->cancel_enc_smoothing_thread )
                pthread_cond_wait( &h->obe_clock_cv, &h->obe_clock_mutex );
        }
        /* otherwise, continue since the frame is late */

        pthread_mutex_unlock( &h->obe_clock_mutex );

        add_to_queue( &h->mux_queue, &coded_frame[0]->uchain );
        coded_frame[0] = NULL;

        if( double_output )
        {
            if( num_enc_smoothing_frames == 1 )
                release_late_frame = 1;
            else if( coded_frame[1] )
            {
                add_to_queue( &h->mux_queue, &coded_frame[1]->uchain );
                coded_frame[1] = NULL;
            }
        }

#if 0
        pthread_mutex_lock( &h->obe_clock_mutex );
        int64_t last_wallclock =  h->obe_clock_last_wallclock;
        int64_t last_pts = h->obe_clock_last_pts;
        pthread_mutex_unlock( &h->obe_clock_mutex );
        int64_t wallclock = get_wallclock_in_mpeg_ticks();
        int64_t ticks = last_pts + (wallclock - last_wallclock);
        printf("\n send_delta %"PRIi64" last_wallclock %" PRIi64 " last_pts %" PRIi64 " wallclock %" PRIi64 " \n", ticks - send_delta, last_wallclock, last_pts, wallclock );
        send_delta = ticks;
#else
        (void)send_delta;
        int64_t ticks = get_input_clock_in_mpeg_ticks(h);
#endif

        pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
        h->enc_smoothing_last_exit_time = ticks;
        pthread_cond_signal( &h->enc_smoothing_queue.out_cv );
        pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
        num_enc_smoothing_frames = 0;
    }

    return NULL;
}

const obe_smoothing_func_t enc_smoothing = { start_smoothing };
