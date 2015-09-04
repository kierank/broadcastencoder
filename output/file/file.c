/*****************************************************************************
 * file.c : File output functions
 *****************************************************************************
 * Copyright (C) 2013 Open Broadcast Systems Ltd.
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
#include "output/output.h"
#include <libavutil/buffer.h>

struct file_status
{
    obe_output_t *output;
    FILE **fp;
};

static void close_output( void *handle )
{
    struct file_status *status = handle;

    fclose( *status->fp );

    if( status->output->output_dest.target  )
        free( status->output->output_dest.target );
}

static void *open_output( void *ptr )
{
    obe_output_t *output = ptr;
    obe_output_dest_t *output_dest = &output->output_dest;
    struct file_status status;
    FILE *fp = NULL;
    int num_muxed_data = 0;
    AVBufferRef **muxed_data;

    status.output = output;
    status.fp = &fp;
    pthread_cleanup_push( close_output, (void*)&status );

    fp = fopen( output_dest->target, "wb" );
    if( !fp )
    {
        fprintf( stderr, "[file] Could not open file" );
        return NULL;
    }

    while( 1 )
    {
        pthread_mutex_lock( &output->queue.mutex );
        while( ulist_empty( &output->queue.ulist ) && !output->cancel_thread )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &output->queue.in_cv, &output->queue.mutex );
        }

        if( output->cancel_thread )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            break;
        }

        num_muxed_data = ulist_depth( &output->queue.ulist );

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }

        //FIXME
        pthread_mutex_unlock( &output->queue.mutex );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            fwrite( &muxed_data[i]->data[7*sizeof(int64_t)], 1, TS_PACKETS_SIZE, fp );

            av_buffer_unref( &muxed_data[i] );
        }

        free( muxed_data );
        muxed_data = NULL;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t file_output = { open_output };
