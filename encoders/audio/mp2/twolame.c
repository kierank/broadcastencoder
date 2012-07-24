/*****************************************************************************
 * twolame.c : twolame encoding functions
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
 ******************************************************************************/

#include "common/common.h"
#include "encoders/audio/audio.h"
#include <twolame.h>
#include <libavutil/fifo.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>

#define MP2_AUDIO_BUFFER_SIZE 50000

static void *start_encoder( void *ptr )
{
    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    twolame_options *tl_opts = NULL;
    int output_size, frame_size, linesize; /* Linesize in libavresample terminology is the entire buffer size for packed formats */
    int64_t cur_pts = -1;
    void *audio_buf = NULL;
    uint8_t *output_buf = NULL;
    AVAudioResampleContext *avr = NULL;
    AVFifoBuffer *fifo = NULL;

    /* Lock the mutex until we verify parameters */
    pthread_mutex_lock( &encoder->queue.mutex );

    tl_opts = twolame_init();
    if( !tl_opts )
    {
        fprintf( stderr, "[twolame] could load options" );
        pthread_mutex_unlock( &encoder->queue.mutex );
        goto end;
    }

    /* TODO: setup bitrate reconfig, errors */
    twolame_set_bitrate( tl_opts, enc_params->bitrate );
    twolame_set_in_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_out_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_copyright( tl_opts, 1 );
    twolame_set_original( tl_opts, 1 );
    twolame_set_num_channels( tl_opts, av_get_channel_layout_nb_channels( enc_params->channel_layout ) );
    twolame_set_error_protection( tl_opts, 1 );

    twolame_init_params( tl_opts );

    frame_size = twolame_get_framelength( tl_opts ) * enc_params->frames_per_pes;

    encoder->is_ready = 1;
    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->queue.in_cv );
    pthread_mutex_unlock( &encoder->queue.mutex );

    output_buf = malloc( MP2_AUDIO_BUFFER_SIZE );
    if( !output_buf )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    avr = avresample_alloc_context();
    if( !avr )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    av_opt_set_int( avr, "in_channel_layout",   enc_params->channel_layout,  0 );
    av_opt_set_int( avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32,    0 ); // FIXME
    av_opt_set_int( avr, "in_sample_rate",      enc_params->sample_rate, 0 );
    av_opt_set_int( avr, "out_channel_layout",  enc_params->channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_fmt",      AV_SAMPLE_FMT_FLT,   0 );
    av_opt_set_int( avr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,  0 );

    if( avresample_open( avr ) < 0 )
    {
        fprintf( stderr, "Could not open AVResample\n" );
        goto end;
    }

    /* Setup the output FIFO */
    fifo = av_fifo_alloc( frame_size );
    if( !fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    while( 1 )
    {
        pthread_mutex_lock( &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            break;
        }

        if( !encoder->queue.size )
            pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            break;
        }

        raw_frame = encoder->queue.queue[0];
        pthread_mutex_unlock( &encoder->queue.mutex );

        if( cur_pts == -1 )
            cur_pts = raw_frame->pts;

        /* Allocate the output buffer */
        if( av_samples_alloc( (uint8_t**)&audio_buf, &linesize, av_get_channel_layout_nb_channels( raw_frame->channel_layout ), raw_frame->num_samples,
                              AV_SAMPLE_FMT_FLT, 0 ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        if( avresample_convert( avr, &audio_buf, linesize, raw_frame->num_samples, (void**)&raw_frame->data, raw_frame->len, raw_frame->num_samples ) < 0 )
        {
            syslog( LOG_ERR, "[twolame] Sample format conversion failed\n" );
            break;
        }

        output_size = twolame_encode_buffer_float32_interleaved( tl_opts, audio_buf, raw_frame->num_samples, output_buf, MP2_AUDIO_BUFFER_SIZE );

        if( output_size < 0 )
        {
            syslog( LOG_ERR, "[twolame] Encode failed\n" );
            break;
        }

        free( audio_buf );
        audio_buf = NULL;

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_from_queue( &encoder->queue );

        if( av_fifo_realloc2( fifo, av_fifo_size( fifo ) + output_size ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }

        av_fifo_generic_write( fifo, output_buf, output_size, NULL );

        while( av_fifo_size( fifo ) >= frame_size )
        {
            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
            av_fifo_generic_read( fifo, coded_frame->data, frame_size, NULL );
            coded_frame->pts = cur_pts;
            coded_frame->random_access = 1; /* Every frame output is a random access point */

            add_to_queue( &h->mux_queue, coded_frame );
            /* We need to generate PTS because frame sizes have changed */
            cur_pts += (double)MP2_NUM_SAMPLES * OBE_CLOCK * enc_params->frames_per_pes / enc_params->sample_rate;
        }
    }

end:
    if( output_buf )
        free( output_buf );

    if( audio_buf )
        free( audio_buf );

    if( avr )
        avresample_free( &avr );

    if( fifo )
        av_fifo_free( fifo );

    if( tl_opts )
        twolame_close( &tl_opts );
    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t twolame_encoder = { start_encoder };
