/*****************************************************************************
 * udp.c : UDP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Large Portions of this code originate from FFmpeg
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
#include "common/network/network.h"
#include "common/network/udp/udp.h"
#include "output/output.h"
#include <libavutil/fifo.h>

struct udp_status
{
    obe_output_params_t *output_params;
    hnd_t *udp_handle;
    AVFifoBuffer *fifo_data;
};

static void close_output( void *handle )
{
    struct udp_status *status = handle;

    if( status->fifo_data )
        av_fifo_free( status->fifo_data );
    if( *status->udp_handle )
        udp_close( *status->udp_handle );
    free( status->output_params );
}

static void *open_output( void *ptr )
{
    obe_output_params_t *output_params = ptr;
    obe_t *h = output_params->h;
    char *target = output_params->target;
    struct udp_status status;
    hnd_t udp_handle = NULL;
    int num_muxed_data = 0;
    obe_muxed_data_t **muxed_data;
    int64_t last_pcr = -1, last_clock = -1, delta, mpegtime;
    AVFifoBuffer *fifo_data = NULL, *fifo_pcr = NULL;
    uint8_t udp_buf[TS_PACKETS_SIZE];
    int64_t pcrs[7];

    struct sched_param param = {0};
    param.sched_priority = 49;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    fifo_data = av_fifo_alloc( TS_PACKETS_SIZE );
    if( !fifo_data )
    {
        fprintf( stderr, "[udp] Could not allocate data fifo" );
        return NULL;
    }

    status.output_params = output_params;
    status.udp_handle = &udp_handle;
    status.fifo_data = fifo_data;
    pthread_cleanup_push( close_output, (void*)&status );

    fifo_pcr = av_fifo_alloc( 7 * sizeof(int64_t) );
    if( !fifo_pcr )
    {
        fprintf( stderr, "[udp] Could not allocate pcr fifo" );
        return NULL;
    }

    if( udp_open( &udp_handle, target ) < 0 )
    {
        fprintf( stderr, "[udp] Could not create output" );
        return NULL;
    }

    while( 1 )
    {
        pthread_mutex_lock( &h->output_mutex );
        if( !h->num_muxed_data )
            pthread_cond_wait( &h->output_cv, &h->output_mutex );

        num_muxed_data = h->num_muxed_data;

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &h->output_mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, h->muxed_data, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &h->output_mutex );

        printf("\n START %i \n", num_muxed_data );

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

            remove_from_output_queue( h );
            destroy_muxed_data( muxed_data[i] );
        }

        free( muxed_data );

        while( av_fifo_size( fifo_data ) >= TS_PACKETS_SIZE )
        {
            av_fifo_generic_read( fifo_data, udp_buf, TS_PACKETS_SIZE, NULL );
            av_fifo_generic_read( fifo_pcr, pcrs, 7 * sizeof(int64_t), NULL );
            if( last_clock != -1 )
            {
                delta = pcrs[6] - last_pcr;
                mpegtime = get_wallclock_in_mpeg_ticks();
                if( last_clock + delta >= mpegtime )
                {
                    sleep_mpeg_ticks( last_clock + delta - mpegtime );
                }
                else
                {
                    printf("\n behind %f \n", (double)(last_clock + delta - mpegtime)/27000000 );
                }
            }
            last_pcr = pcrs[6];
            last_clock = get_wallclock_in_mpeg_ticks();

            udp_write( udp_handle, udp_buf, TS_PACKETS_SIZE ); // TODO handle fail
        }
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t udp_output = { open_output };
