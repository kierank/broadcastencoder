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

#define MP2_AUDIO_BUFFER_SIZE 50000

static void *start_encoder( void *ptr )
{
    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    twolame_options *tl_opts = NULL;
    int output_size, frame_size, num_channels;
    int64_t cur_pts = -1;
    int32_t *s32_data;
    short   *s16_data;
    uint8_t *output_buffer = NULL, *output_pos;

    /* Lock the mutex until we verify parameters */
    pthread_mutex_lock( &encoder->encoder_mutex );

    tl_opts = twolame_init();
    if( !tl_opts )
    {
        fprintf( stderr, "[twolame] could load options" );
        pthread_mutex_unlock( &encoder->encoder_mutex );
        goto end;
    }

    /* TODO: setup bitrate reconfig, errors */
    twolame_set_bitrate( tl_opts, enc_params->bitrate );
    twolame_set_in_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_out_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_copyright( tl_opts, 1 );
    twolame_set_original( tl_opts, 1 );
    twolame_set_num_channels( tl_opts, enc_params->num_channels );
    twolame_set_error_protection( tl_opts, 1 );

    twolame_init_params( tl_opts );

    frame_size = (double)MP2_NUM_SAMPLES * 125 * enc_params->bitrate / enc_params->sample_rate;

    encoder->is_ready = 1;
    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->encoder_cv );
    pthread_mutex_unlock( &encoder->encoder_mutex );

    output_buffer = malloc( MP2_AUDIO_BUFFER_SIZE );
    if( !output_buffer )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }
    while( 1 )
    {
        pthread_mutex_lock( &encoder->encoder_mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            goto end;
        }

        if( !encoder->num_raw_frames )
            pthread_cond_wait( &encoder->encoder_cv, &encoder->encoder_mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            goto end;
        }

        raw_frame = encoder->frames[0];
        pthread_mutex_unlock( &encoder->encoder_mutex );

        if( cur_pts == -1 )
            cur_pts = raw_frame->pts;

        if( raw_frame->sample_fmt == AV_SAMPLE_FMT_S16 )
            s16_data = (short*)raw_frame->data;
        /* FIXME: is there a better way than dropping LSBs */
        else if( raw_frame->sample_fmt == AV_SAMPLE_FMT_S32 )
        {
            s32_data = (int32_t*)raw_frame->data;
            num_channels = av_get_channel_layout_nb_channels( raw_frame->channel_layout );
            s16_data = malloc( num_channels * raw_frame->num_samples * sizeof(short) );
            if( !s16_data )
            {
                syslog( LOG_ERR, "Malloc failed" );
                goto end;
            }
            for( int i = 0; i < num_channels * raw_frame->num_samples; i++ )
                s16_data[i] = s32_data[i] >> 16;
        }
        else //if( raw_frame->sample_fmt == AV_SAMPLE_FMT_AES )
        {
            // TODO
        }

        output_size = twolame_encode_buffer_interleaved( tl_opts, s16_data, raw_frame->num_samples, output_buffer, MP2_AUDIO_BUFFER_SIZE );

        if( raw_frame->sample_fmt != AV_SAMPLE_FMT_S16 && s16_data )
        {
            free( s16_data );
            s16_data = NULL;
        }

        if( output_size < 0 )
        {
            syslog( LOG_ERR, "[twolame] Encode failed\n" );
            goto end;
        }

        output_pos = output_buffer;

        /* Sometimes multiple frames will be output so split them up if necessary */
        while( output_size > 0 )
        {
            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
            memcpy( coded_frame->data, output_pos, frame_size );
            coded_frame->pts = cur_pts;
            add_to_mux_queue( h, coded_frame );

            /* We need to generate PTS because frame sizes have changed */
            cur_pts += (double)MP2_NUM_SAMPLES * 90000 / enc_params->sample_rate;
            output_size -= frame_size;
            output_pos += frame_size;
        }

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_frame_from_encode_queue( encoder );
    }

end:
    if( output_buffer )
        free( output_buffer );

    if( tl_opts )
        twolame_close( &tl_opts );
    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t twolame_encoder = { start_encoder };
