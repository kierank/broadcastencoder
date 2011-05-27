/*****************************************************************************
 * lavc.c: libavcodec audio encoding functions
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
#include <libavutil/fifo.h>
#include <libavcodec/audioconvert.h>
#include <libavcodec/avcodec.h>

typedef struct
{
    int obe_name;
    int lavc_name;
    int frame_samples;
} lavc_encoder_t;

static const lavc_encoder_t lavc_encoders[] =
{
    { AUDIO_AC_3, CODEC_ID_AC3, AC3_NUM_SAMPLES },
    /* TODO: E-AC3 */
    //{ AUDIO_AAC,  CODEC_ID_AAC, AAC_NUM_SAMPLES },
    { -1, -1, -1 },
};

static void *start_encoder( void *ptr )
{
    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    int64_t cur_pts = -1;
    int i, frame_size, frame_samples_size;
    void *audio_buf = NULL, *samples = NULL;
    AVFifoBuffer *fifo = NULL;
    uint8_t *output_buffer = NULL;
    AVAudioConvert *audio_conv = NULL;

    avcodec_init();
    avcodec_register_all();

    AVCodecContext *codec = avcodec_alloc_context();
    if( !codec )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    for( i = 0; lavc_encoders[i].obe_name != -1; i++ )
    {
        if( lavc_encoders[i].obe_name == enc_params->output_format )
            break;
    }

    if( lavc_encoders[i].obe_name == -1 )
    {
        fprintf( stderr, "[lavc] Could not find encoder1\n" );
        goto finish;
    }

    AVCodec *enc = avcodec_find_encoder( lavc_encoders[i].lavc_name );
    if( !enc )
    {
        fprintf( stderr, "[lavc] Could not find encoder2\n" );
        goto finish;
    }

    codec->sample_rate = enc_params->sample_rate;
    codec->bit_rate = enc_params->bitrate * 1000;
    codec->sample_fmt = AV_SAMPLE_FMT_FLT;
    codec->channels = enc_params->num_channels;
    codec->channel_layout = AV_CH_LAYOUT_STEREO;

    if( avcodec_open( codec, enc ) < 0 )
    {
        fprintf( stderr, "[lavc] Could not open encoder\n" );
        goto finish;
    }

    int in_stride = av_get_bits_per_sample_fmt( enc_params->sample_format ) / 8;
    int out_stride = av_get_bits_per_sample_fmt( AV_SAMPLE_FMT_FLT ) / 8;

    fifo = av_fifo_alloc( OBE_MAX_CHANNELS * codec->frame_size * in_stride * 2 );
    if( !fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    /* This works on "planar" audio so pretend it's just one audio plane */
    audio_conv = av_audio_convert_alloc( AV_SAMPLE_FMT_FLT, 1, enc_params->sample_format, 1, NULL, 0 );
    if( !audio_conv )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    output_buffer = malloc( FF_MIN_BUFFER_SIZE );
    if( !output_buffer )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    frame_samples_size = codec->frame_size * enc_params->num_channels * out_stride;

    samples = malloc( frame_samples_size );
    if( !samples )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    while( 1 )
    {
        /* TODO: detect bitrate or channel reconfig */
        pthread_mutex_lock( &encoder->encoder_mutex );
        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            goto finish;
        }

        if( !encoder->num_raw_frames )
            pthread_cond_wait( &encoder->encoder_cv, &encoder->encoder_mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            goto finish;
        }

        raw_frame = encoder->frames[0];
        pthread_mutex_unlock( &encoder->encoder_mutex );

        if( cur_pts == -1 )
            cur_pts = raw_frame->pts;

        in_stride = av_get_bits_per_sample_fmt( raw_frame->sample_fmt ) / 8;
        int num_samples = raw_frame->len / in_stride;
        int sample_bytes = num_samples * out_stride;
        int istride[6] = { in_stride };
        int ostride[6] = { out_stride };
        const void *ibuf[6] = { raw_frame->data };

        audio_buf = malloc( sample_bytes );
        if( !audio_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto finish;
        }

        void *obuf[6] = { audio_buf };

        if( av_audio_convert( audio_conv, obuf, ostride, ibuf, istride, num_samples ) < 0 )
        {
            syslog( LOG_ERR, "[lavf] Could not convert audio sample format\n" );
            goto finish;
        }

        if( av_fifo_realloc2( fifo, av_fifo_size( fifo ) + sample_bytes ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto finish;
        }

        av_fifo_generic_write( fifo, audio_buf, sample_bytes, NULL );

        free( audio_buf );
        audio_buf = NULL;

        while( av_fifo_size( fifo ) >= frame_samples_size )
        {
            av_fifo_generic_read( fifo, samples, frame_samples_size, NULL );

            frame_size = avcodec_encode_audio( codec, output_buffer, FF_MIN_BUFFER_SIZE, samples );
            if( frame_size < 0 )
            {
                syslog( LOG_ERR, "[lavf] Audio encoding failed\n" );
                goto finish;
            }

            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto finish;
            }

            memcpy( coded_frame->data, output_buffer, frame_size );
            coded_frame->pts = cur_pts;
            add_to_mux_queue( h, coded_frame );

            /* We need to generate PTS because frame sizes have changed */
            cur_pts += (double)lavc_encoders[i].frame_samples * 90000 / enc_params->sample_rate;
        }

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_frame_from_encode_queue( encoder );
    }

finish:
    if( fifo )
        av_fifo_free( fifo );

    if( output_buffer )
        free( output_buffer );

    if( audio_conv )
        av_audio_convert_free( audio_conv );

    if( audio_buf )

    if( samples )
        free( samples );

    if( codec )
    {
        avcodec_close( codec );
        av_free( codec );
    }

    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t lavc_encoder = { start_encoder };
