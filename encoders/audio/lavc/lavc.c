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
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>

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
    void *audio_buf = NULL;
    int64_t cur_pts = -1;
    int i, frame_size, ret, got_pkt, num_frames = 0, total_size = 0, audio_buf_len;
    AVFifoBuffer *out_fifo = NULL;
    AVAudioResampleContext *avr = NULL;
    AVPacket pkt;
    AVCodecContext *codec = NULL;
    AVFrame frame;

    avcodec_register_all();

    codec = avcodec_alloc_context3( NULL );
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

    if( enc->sample_fmts[0] == -1 )
    {
        fprintf( stderr, "[lavc] No valid sample formats\n" );
        goto finish;
    }

    codec->sample_rate = enc_params->sample_rate;
    codec->bit_rate = enc_params->bitrate * 1000;
    codec->sample_fmt = enc->sample_fmts[0];
    codec->channels = av_get_channel_layout_nb_channels( enc_params->channel_layout );
    codec->channel_layout = enc_params->channel_layout;
    codec->time_base.num = 1;
    codec->time_base.den = OBE_CLOCK;

    if( avcodec_open2( codec, enc, NULL ) < 0 )
    {
        fprintf( stderr, "[lavc] Could not open encoder\n" );
        goto finish;
    }

    avr = avresample_alloc_context();
    if( !avr )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    av_opt_set_int( avr, "in_channel_layout",   codec->channel_layout, 0 );
    av_opt_set_int( avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32,     0 ); // FIXME
    av_opt_set_int( avr, "in_sample_rate",      enc_params->sample_rate, 0 );
    av_opt_set_int( avr, "out_channel_layout",  codec->channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_fmt",      codec->sample_fmt,     0 );
    av_opt_set_int( avr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,    0 );

    if( avresample_open( avr ) < 0 )
    {
        fprintf( stderr, "Could not open AVResample\n" );
        goto finish;
    }

    /* The number of samples per E-AC3 frame is unknown until the encoder is ready */
    if( enc_params->output_format == AUDIO_E_AC_3 )
    {
        pthread_mutex_lock( &encoder->queue.mutex );
        encoder->is_ready = 1;
        encoder->num_samples = codec->frame_size;
        /* Broadcast because input and muxer can be stuck waiting for encoder */
        pthread_cond_broadcast( &encoder->queue.in_cv );
        pthread_mutex_unlock( &encoder->queue.mutex );
    }

    frame_size = (double)codec->frame_size * 125 * enc_params->bitrate *
                 enc_params->frames_per_pes / enc_params->sample_rate;
    out_fifo = av_fifo_alloc( frame_size );
    if( !out_fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    audio_buf_len = codec->frame_size * av_get_bytes_per_sample( codec->sample_fmt ) * codec->channels;
    audio_buf = av_malloc( audio_buf_len );
    if( !audio_buf )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    avcodec_get_frame_defaults( &frame );

    while( 1 )
    {
        /* TODO: detect bitrate or channel reconfig */
        pthread_mutex_lock( &encoder->queue.mutex );
        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            goto finish;
        }

        if( !encoder->queue.size )
            pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            goto finish;
        }

        raw_frame = encoder->queue.queue[0];
        pthread_mutex_unlock( &encoder->queue.mutex );

        if( cur_pts == -1 )
            cur_pts = raw_frame->pts;

        if( avresample_convert( avr, NULL, 0, raw_frame->num_samples, (void**)&raw_frame->data, raw_frame->len, raw_frame->num_samples ) < 0 )
        {
            syslog( LOG_ERR, "[lavc] Sample format conversion failed\n" );
            break;
        }

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_from_queue( &encoder->queue );

        while( avresample_available( avr ) >= codec->frame_size )
        {
            got_pkt = 0;
            frame.nb_samples = codec->frame_size;
            avresample_read( avr, &audio_buf, codec->frame_size );

            if( avcodec_fill_audio_frame( &frame, codec->channels, codec->sample_fmt, audio_buf, audio_buf_len, 0 ) < 0 )
	    {
                syslog( LOG_ERR, "[lavc] Could not fill audio frame\n" );
                break;
            }

            av_init_packet( &pkt );
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

            total_size += pkt.size;
            num_frames++;

            if( av_fifo_realloc2( out_fifo, av_fifo_size( out_fifo ) + pkt.size ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                break;
            }

            av_fifo_generic_write( out_fifo, pkt.data, pkt.size, NULL );
            obe_free_packet( &pkt );

            if( num_frames == enc_params->frames_per_pes )
            {
                coded_frame = new_coded_frame( encoder->stream_id, total_size );
                if( !coded_frame )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    goto finish;
                }

                av_fifo_generic_read( out_fifo, coded_frame->data, total_size, NULL );

                coded_frame->pts = cur_pts;
                coded_frame->random_access = 1; /* Every frame output is a random access point */
                add_to_queue( &h->mux_queue, coded_frame );

                /* We need to generate PTS because frame sizes have changed */
                cur_pts += (double)codec->frame_size * OBE_CLOCK * enc_params->frames_per_pes / enc_params->sample_rate;

                total_size = num_frames = 0;
            }
        }
    }

finish:
    if( audio_buf )
        av_free( audio_buf );

    if( out_fifo )
        av_fifo_free( out_fifo );

    if( avr )
        avresample_free( &avr );

    if( codec )
    {
        avcodec_close( codec );
        av_free( codec );
    }

    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t lavc_encoder = { start_encoder };
