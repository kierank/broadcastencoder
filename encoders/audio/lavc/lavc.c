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
#include "common/lavc.h"
#include "encoders/audio/audio.h"
#include <libavutil/fifo.h>
#include <libavcodec/audioconvert.h>
#include <libavformat/avformat.h>

typedef struct
{
    int obe_name;
    int lavc_name;
} lavc_encoder_t;

static const lavc_encoder_t lavc_encoders[] =
{
    { AUDIO_AC_3, CODEC_ID_AC3 },
    { AUDIO_E_AC_3, CODEC_ID_EAC3 },
    { AUDIO_AAC,  CODEC_ID_AAC },
    { -1, -1 },
};

static void *start_encoder( void *ptr )
{
    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    int64_t cur_pts = -1;
    int i, frame_size = 0, ret, frame_samples_size, is_latm, got_pkt;
    void *audio_buf = NULL, *samples = NULL;
    AVFifoBuffer *in_fifo = NULL, *out_fifo = NULL;
    uint8_t *avio_buf = NULL;
    AVAudioConvert *audio_conv = NULL;
    AVFormatContext *fmt = NULL;
    AVStream *st = NULL;
    AVPacket pkt;
    AVIOContext *avio = NULL;
    AVCodecContext *codec = NULL;
    AVFrame frame;;

    avcodec_register_all();

    /* AAC audio needs ADTS or LATM encapsulation */
    if( enc_params->output_format == AUDIO_AAC )
    {
        is_latm = enc_params->aac_opts.latm_output;
        av_register_all();

        fmt = avformat_alloc_context();
        if( !fmt )
        {
            fprintf( stderr, "Malloc failed\n" );
            goto finish;
        }

        fmt->oformat = av_guess_format( is_latm ? "latm" : "adts", NULL, NULL );
        if( !fmt->oformat )
        {
            fprintf( stderr, "ADTS muxer not found\n" );
            goto finish;
        }

        st = avformat_new_stream( fmt, NULL );
        if( !st )
        {
            fprintf( stderr, "Malloc failed\n" );
            goto finish;
        }
        codec = st->codec;
    }
    else
    {
        codec = avcodec_alloc_context3( NULL );
        if( !codec )
        {
            fprintf( stderr, "Malloc failed\n" );
            goto finish;
        }
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

    if( enc->sample_fmts[0] == -1 )
    {
        fprintf( stderr, "[lavc] No valid sample formats\n" );
        goto finish;
    }

    codec->sample_rate = enc_params->sample_rate;
    codec->bit_rate = enc_params->bitrate * 1000;
    codec->sample_fmt = enc->sample_fmts[0];
    codec->channels = enc_params->num_channels;
    codec->channel_layout = AV_CH_LAYOUT_STEREO;
    codec->time_base.num = 1;
    codec->time_base.den = OBE_CLOCK;

    if( avcodec_open2( codec, enc, NULL ) < 0 )
    {
        fprintf( stderr, "[lavc] Could not open encoder\n" );
        goto finish;
    }

    /* The number of samples per E-AC3 frame is unknown until the encoder is ready */
    if( enc_params->output_format == AUDIO_E_AC_3 )
    {
        pthread_mutex_lock( &encoder->encoder_mutex );
        encoder->is_ready = 1;
        encoder->num_samples = codec->frame_size;
        /* Broadcast because input and muxer can be stuck waiting for encoder */
        pthread_cond_broadcast( &encoder->encoder_cv );
        pthread_mutex_unlock( &encoder->encoder_mutex );
    }

    int in_stride = av_get_bytes_per_sample( enc_params->sample_format );
    int out_stride = av_get_bytes_per_sample( enc->sample_fmts[0] );

    in_fifo = av_fifo_alloc( OBE_MAX_CHANNELS * codec->frame_size * in_stride * 2 );
    if( !in_fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    if( enc_params->output_format == AUDIO_AC_3 )
    {
        frame_size = (double)codec->frame_size * 125 * enc_params->bitrate *
                     enc_params->frames_per_pes / enc_params->sample_rate;
        out_fifo = av_fifo_alloc( frame_size );
        if( !out_fifo )
        {
            fprintf( stderr, "Malloc failed\n" );
            goto finish;
        }
    }

    /* This works on "planar" audio so pretend it's just one audio plane */
    audio_conv = av_audio_convert_alloc( enc->sample_fmts[0], 1, enc_params->sample_format, 1, NULL, 0 );
    if( !audio_conv )
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

    avcodec_get_frame_defaults( &frame );

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

        in_stride = av_get_bytes_per_sample( raw_frame->sample_fmt );
        int num_samples = raw_frame->len / in_stride;
        int sample_bytes = num_samples * out_stride;
        int istride[6] = { in_stride };
        int ostride[6] = { out_stride };
        const void *ibuf[6] = { raw_frame->data };

        audio_buf = av_malloc( sample_bytes );
        if( !audio_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto finish;
        }

        void *obuf[6] = { audio_buf };

        if( av_audio_convert( audio_conv, obuf, ostride, ibuf, istride, num_samples ) < 0 )
        {
            syslog( LOG_ERR, "[lavc] Could not convert audio sample format\n" );
            goto finish;
        }

        if( av_fifo_realloc2( in_fifo, av_fifo_size( in_fifo ) + sample_bytes ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto finish;
        }

        av_fifo_generic_write( in_fifo, audio_buf, sample_bytes, NULL );
        av_freep( &audio_buf );

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_frame_from_encode_queue( encoder );

        av_init_packet( &pkt );

        while( av_fifo_size( in_fifo ) >= frame_samples_size )
        {
            av_fifo_generic_read( in_fifo, samples, frame_samples_size, NULL );

            frame.data[0] = samples;
            frame.linesize[0] = frame_samples_size;
            frame.nb_samples = codec->frame_size;

            pkt.data = NULL;
            pkt.size = 0;

            ret = avcodec_encode_audio2( codec, &pkt, &frame, &got_pkt );
            if( ret < 0 )
            {
                syslog( LOG_ERR, "[lavc] Audio encoding failed\n" );
                goto finish;
            }

            if( !got_pkt )
                continue;

            /* Encapsulate AAC frames in ADTS or LATM */
            if( enc_params->output_format == AUDIO_AAC )
            {
                /* Allocate a dynamic memory buffer with avio */
                if( avio_open_dyn_buf( &avio ) )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    goto finish;
                }
                fmt->pb = avio;
                /* TODO: handle fails */
                avformat_write_header( fmt, NULL );
                av_write_frame( fmt, &pkt );
                frame_size = avio_close_dyn_buf( avio, &avio_buf );
            }
            else if( enc_params->output_format == AUDIO_AC_3 )
            {
                av_fifo_generic_write( out_fifo, pkt.data, pkt.size, NULL );
                if( av_fifo_size( out_fifo ) < frame_size )
                    continue;
            }
            else
                frame_size = pkt.size;

            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto finish;
            }

            if( enc_params->output_format == AUDIO_AAC )
            {
                memcpy( coded_frame->data, avio_buf, frame_size );
                av_freep( &avio_buf );
            }
            else if( enc_params->output_format == AUDIO_AC_3 )
                av_fifo_generic_read( out_fifo, coded_frame->data, frame_size, NULL );
            else
                memcpy( coded_frame->data, pkt.data, pkt.size );

            coded_frame->pts = cur_pts;
            coded_frame->random_access = 1; /* Every frame output is a random access point */
            add_to_mux_queue( h, coded_frame );

            /* We need to generate PTS because frame sizes have changed */
            cur_pts += (double)codec->frame_size * OBE_CLOCK * enc_params->frames_per_pes / enc_params->sample_rate;
            obe_free_packet( &pkt );
        }
    }

finish:
    if( audio_buf )
        free( audio_buf );

    if( samples )
        free( samples );

    if( in_fifo )
        av_fifo_free( in_fifo );

    if( out_fifo )
        av_fifo_free( out_fifo );

    if( avio_buf )
        av_free( avio_buf );

    if( audio_conv )
        av_audio_convert_free( audio_conv );

    if( fmt )
        avformat_free_context( fmt );

    if( codec )
    {
        avcodec_close( codec );
        av_free( codec );
    }

    if( st )
        av_free( st );

    if( fmt )
        avformat_free_context( fmt );

    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t lavc_encoder = { start_encoder };
