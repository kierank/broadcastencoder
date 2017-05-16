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
#include <libavutil/buffer.h>
#include "common/common.h"

static void *start_smoothing( void *ptr )
{
    obe_t *h = ptr;
    int num_muxed_data = 0, buffer_complete = 0;
    int64_t start_clock = -1, start_pcr, end_pcr, temporal_vbv_size = 0, cur_pcr;
    obe_muxed_data_t **muxed_data = NULL, *start_data, *end_data;
    AVFifoBuffer *fifo_data = NULL, *fifo_pcr = NULL;
    obe_buf_ref_t **output_buffers = NULL;
    AVBufferPool *buffer_pool = NULL;

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

    output_buffers = malloc( h->num_outputs * sizeof(*output_buffers) );
    if( !output_buffers )
    {
        fprintf( stderr, "[mux-smoothing] Could not allocate output buffers" );
        return NULL;
    }

    buffer_pool = av_buffer_pool_init( sizeof(obe_buf_ref_t) * 100, NULL );
    if( !buffer_pool )
    {
        fprintf( stderr, "[mux-smoothing] Could not allocate buffer pool" );
        return NULL;
    }

    if( h->obe_system != OBE_SYSTEM_TYPE_LOWEST_LATENCY )
    {
        for( int i = 0; i < h->num_encoders; i++ )
        {
            if( h->encoders[i]->is_video )
            {
                pthread_mutex_lock( &h->encoders[i]->queue.mutex );
                while( !h->encoders[i]->is_ready )
                    pthread_cond_wait( &h->encoders[i]->queue.in_cv, &h->encoders[i]->queue.mutex );
                obe_output_stream_t *output_stream = get_output_stream( h, h->encoders[i]->output_stream_id );
                x264_param_t *params = &output_stream->avc_param;
                temporal_vbv_size = av_rescale_q_rnd(
                (int64_t)params->rc.i_vbv_buffer_size * params->rc.f_vbv_buffer_init,
                (AVRational){1, params->rc.i_vbv_max_bitrate }, (AVRational){ 1, OBE_CLOCK }, AV_ROUND_UP );
                pthread_mutex_unlock( &h->encoders[i]->queue.mutex );
                break;
            }
        }
    }

    while( 1 )
    {
        pthread_mutex_lock( &h->mux_smoothing_queue.mutex );

        while( ulist_depth( &h->mux_smoothing_queue.ulist ) == num_muxed_data && !h->cancel_mux_smoothing_thread )
            pthread_cond_wait( &h->mux_smoothing_queue.in_cv, &h->mux_smoothing_queue.mutex );

        if( h->cancel_mux_smoothing_thread )
        {
            pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
            break;
        }

        num_muxed_data = ulist_depth( &h->mux_smoothing_queue.ulist );

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
            struct uchain *first_uchain = &h->mux_smoothing_queue.ulist;
            start_data = obe_muxed_data_t_from_uchain( ulist_peek( first_uchain ) );
            if( num_muxed_data == 1 )
                end_data = start_data;
            else
                end_data = obe_muxed_data_t_from_uchain( first_uchain->prev );

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
            pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }

        for( int i = 0; i < num_muxed_data; i++ )
            muxed_data[i] = obe_muxed_data_t_from_uchain( ulist_pop( &h->mux_smoothing_queue.ulist ) );
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

            destroy_muxed_data( muxed_data[i] );
        }

        free( muxed_data );
        muxed_data = NULL;
        num_muxed_data = 0;

        while( av_fifo_size( fifo_data ) >= TS_PACKETS_SIZE )
        {
            /* Wrap data buffer reference inside obe_buf_ref_t which is also buffer reffed.. */
            AVBufferRef *data_buf_ref = av_buffer_alloc( TS_PACKETS_SIZE + 7 * sizeof(int64_t) );
            av_fifo_generic_read( fifo_pcr, data_buf_ref->data, 7 * sizeof(int64_t), NULL );
            av_fifo_generic_read( fifo_data, &data_buf_ref->data[7 * sizeof(int64_t)], TS_PACKETS_SIZE, NULL );

            for( int i = 0; i < h->num_outputs; i++ )
            {
                AVBufferRef *self_buf_ref = av_buffer_pool_get( buffer_pool );
                obe_buf_ref_t *obe_buf_ref = (obe_buf_ref_t *)self_buf_ref->data;
                obe_buf_ref->self_buf_ref = self_buf_ref;
                uchain_init( &obe_buf_ref->uchain );
                obe_buf_ref->data_buf_ref = i == 0 ? data_buf_ref : av_buffer_ref( data_buf_ref );

                output_buffers[i] = obe_buf_ref;
              
                if( !output_buffers[i] )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return NULL;
                }
            }

            cur_pcr = AV_RN64( output_buffers[0]->data_buf_ref->data );

            if( start_clock != -1 )
            {
                sleep_input_clock( h, cur_pcr - start_pcr + start_clock );
            }

            if( start_clock == -1 )
            {
                start_clock = get_input_clock_in_mpeg_ticks( h );
                start_pcr = cur_pcr;
            }

            for( int i = 0; i < h->num_outputs; i++ )
            {
                if( add_to_queue( &h->outputs[i]->queue, &output_buffers[i]->uchain ) < 0 )
                    return NULL;
                output_buffers[i] = NULL;
            }
        }
    }

    av_fifo_free( fifo_data );
    av_fifo_free( fifo_pcr );
    av_buffer_pool_uninit( &buffer_pool );
    free( output_buffers );

    return NULL;
}

const obe_smoothing_func_t mux_smoothing = { start_smoothing };
