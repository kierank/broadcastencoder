/*****************************************************************************
 * mux/ts/smoothing.c : Mux output smoothing
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd.
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

#include <libavutil/mathematics.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/fifo.h>
#include "common/common.h"

static void *start_smoothing( void *ptr )
{
    obe_t *h = ptr;
    int num_muxed_data = 0, buffer_complete = 0;
    int64_t start_clock = -1, start_pcr, end_pcr, temporal_vbv_size = 0, cur_pcr;
    obe_muxed_data_t **muxed_data = NULL, *start_data, *end_data;
    AVFifoBuffer *fifo_data = NULL, *fifo_pcr = NULL;
    uint8_t *output_buf;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    /* This thread buffers one VBV worth of frames */
    fifo_data = av_fifo_alloc( TS_PACKETS_SIZE );
    if( !fifo_data )
    {
        fprintf( stderr, "[mux-smoothing] Could not allocate data fifo" );
        return NULL;
    }

    fifo_pcr = av_fifo_alloc( 7 * sizeof(int64_t) );
    if( !fifo_pcr )
    {
        fprintf( stderr, "[mux-smoothing] Could not allocate pcr fifo" );
        return NULL;
    }

    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        for( int i = 0; i < h->num_encoders; i++ )
        {
            if( h->encoders[i]->is_video )
            {
                pthread_mutex_lock( &h->encoders[i]->queue.mutex );
                while( !h->encoders[i]->is_ready )
                    pthread_cond_wait( &h->encoders[i]->queue.in_cv, &h->encoders[i]->queue.mutex );
                x264_param_t *params = h->encoders[i]->encoder_params;
                temporal_vbv_size = av_rescale_q_rnd( 1, (AVRational){ params->rc.i_vbv_max_bitrate,
                                    params->rc.i_vbv_buffer_size }, (AVRational){ 1, OBE_CLOCK }, AV_ROUND_UP );
                pthread_mutex_unlock( &h->encoders[i]->queue.mutex );
                break;
            }
        }
    }

    while( 1 )
    {
        pthread_mutex_lock( &h->mux_smoothing_queue.mutex );

        if( h->cancel_mux_smoothing_thread )
        {
            pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
            break;
        }

        while( h->mux_smoothing_queue.size == num_muxed_data )
            pthread_cond_wait( &h->mux_smoothing_queue.in_cv, &h->mux_smoothing_queue.mutex );

        if( h->cancel_mux_smoothing_thread )
        {
            pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
            break;
        }

        num_muxed_data = h->mux_smoothing_queue.size;

        /* Refill the buffer after a drop */
        pthread_mutex_lock( &h->drop_mutex );
        if( h->mux_drop )
        {
            syslog( LOG_INFO, "Mux smoothing buffer reset\n" );
            h->mux_drop = 0;
            av_fifo_reset( fifo_data );
            av_fifo_reset( fifo_pcr );
            buffer_complete = 0;
            start_clock = -1;
        }
        pthread_mutex_unlock( &h->drop_mutex );

        if( !buffer_complete )
        {
            start_data = h->mux_smoothing_queue.queue[0];
            end_data = h->mux_smoothing_queue.queue[num_muxed_data-1];

            start_pcr = start_data->pcr_list[0];
            end_pcr = end_data->pcr_list[(end_data->len / 188)-1];
            if( end_pcr - start_pcr >= temporal_vbv_size )
            {
                buffer_complete = 1;
                start_clock = -1;
            }
            else
            {
                pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
                continue;
            }
        }

        //printf("\n mux smoothed frames %i \n", num_muxed_data );

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &h->output_queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, h->mux_smoothing_queue.queue, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            if( av_fifo_realloc2( fifo_data, av_fifo_size( fifo_data ) + muxed_data[i]->len ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }

            av_fifo_generic_write( fifo_data, muxed_data[i]->data, muxed_data[i]->len, NULL );

            if( av_fifo_realloc2( fifo_pcr, av_fifo_size( fifo_pcr ) + ((muxed_data[i]->len * sizeof(int64_t)) / 188) ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }

            av_fifo_generic_write( fifo_pcr, muxed_data[i]->pcr_list, (muxed_data[i]->len * sizeof(int64_t)) / 188, NULL );

            remove_from_queue( &h->mux_smoothing_queue );
            destroy_muxed_data( muxed_data[i] );
        }

        free( muxed_data );
        muxed_data = NULL;
        num_muxed_data = 0;

        while( av_fifo_size( fifo_data ) >= TS_PACKETS_SIZE )
        {
            output_buf = malloc( TS_PACKETS_SIZE + 7 * sizeof(int64_t) );
            if( !output_buf )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }
            av_fifo_generic_read( fifo_pcr, output_buf, 7 * sizeof(int64_t), NULL );
            av_fifo_generic_read( fifo_data, &output_buf[7 * sizeof(int64_t)], TS_PACKETS_SIZE, NULL );

            cur_pcr = AV_RN64( output_buf );

            if( start_clock != -1 )
            {
                sleep_input_clock( h, cur_pcr - start_pcr + start_clock );
            }

            if( start_clock == -1 )
            {
                start_clock = get_input_clock_in_mpeg_ticks( h );
                start_pcr = cur_pcr;
            }

            if( add_to_queue( &h->output_queue, output_buf ) < 0 )
                return NULL;
            output_buf = NULL;
        }
    }

    return NULL;
}

const obe_smoothing_func_t mux_smoothing = { start_smoothing };
