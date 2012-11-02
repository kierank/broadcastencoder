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

struct udp_status
{
    obe_output_params_t *output_params;
    hnd_t *udp_handle;
};

static void close_output( void *handle )
{
    struct udp_status *status = handle;

    if( *status->udp_handle )
        udp_close( *status->udp_handle );
    free( status->output_params );
}

static void *open_output( void *ptr )
{
    obe_output_params_t *output_params = ptr;
    obe_t *h = output_params->h;
    struct udp_status status;
    hnd_t udp_handle = NULL;
    obe_udp_opts_t udp_opts;
    int num_muxed_data = 0;
    uint8_t **muxed_data;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    status.output_params = output_params;
    status.udp_handle = &udp_handle;
    pthread_cleanup_push( close_output, (void*)&status );

    udp_populate_opts( &udp_opts, output_params->output_opts.target );
    if( udp_open( &udp_handle, &udp_opts ) < 0 )
    {
        fprintf( stderr, "[udp] Could not create output\n" );
        return NULL;
    }

    while( 1 )
    {
        pthread_mutex_lock( &h->output_queue.mutex );
        while( !h->output_queue.size )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &h->output_queue.in_cv, &h->output_queue.mutex );
        }

        num_muxed_data = h->output_queue.size;

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &h->output_queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, h->output_queue.queue, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &h->output_queue.mutex );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            if( udp_write( udp_handle, &muxed_data[i][7*sizeof(int64_t)], TS_PACKETS_SIZE ) < 0 )
                syslog( LOG_ERR, "[udp] Failed to write UDP packet\n" );

            remove_from_queue( &h->output_queue );
            free( muxed_data[i] );
        }

        free( muxed_data );
        num_muxed_data = 0;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t udp_output = { open_output };
