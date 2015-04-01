/*****************************************************************************
 * audio.c: basic audio filtering system
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd
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
 */

#include "common/common.h"
#include "common/lavc.h"
#include "audio.h"

#define MAX_SAMPLES 3000

static void *start_filter( void *ptr )
{
    obe_raw_frame_t *raw_frame, *split_raw_frame;
    obe_coded_frame_t *coded_frame;
    obe_aud_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_output_stream_t *output_stream;
    int num_channels, got_pkt;
    AVCodecContext *codec = NULL;
    AVPacket pkt;
    AVFrame *frame = NULL;

    avcodec_register_all();

    codec = avcodec_alloc_context3( NULL );
    if( !codec )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    AVCodec *enc = avcodec_find_encoder( AV_CODEC_ID_S302M );
    if( !enc )
    {
        fprintf( stderr, "[302m] Could not find encoder\n" );
        goto finish;
    }

    codec->sample_rate = 48000;
    codec->sample_fmt = AV_SAMPLE_FMT_S32;

    if( avcodec_open2( codec, enc, NULL ) < 0 )
    {
        fprintf( stderr, "[302m] Could not open encoder\n" );
        goto finish;
    }

    frame = avcodec_alloc_frame();
    if( !frame )
    {
        fprintf( stderr, "[302m] Could not allocate frame\n" );
        goto finish;
    }
    avcodec_get_frame_defaults( frame );

    /* allocate interleaved buffer */
    if( av_samples_alloc( frame->data, frame->linesize, 8, MAX_SAMPLES, AV_SAMPLE_FMT_S32, 32 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        goto finish;
    }

    while( 1 )
    {
        pthread_mutex_lock( &filter->queue.mutex );

        while( !filter->queue.size && !filter->cancel_thread )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            break;
        }

        raw_frame = filter->queue.queue[0];
        pthread_mutex_unlock( &filter->queue.mutex );

        /* ignore the video track */
        for( int i = 1; i < h->num_encoders; i++ )
        {
            output_stream = get_output_stream( h, h->encoders[i]->output_stream_id );
            num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

            if( output_stream->stream_format == AUDIO_S302M )
            {
                codec->bits_per_raw_sample = output_stream->bit_depth;
                codec->channels = output_stream->num_pairs * 2;
                frame->nb_samples = raw_frame->audio_frame.num_samples;

                int16_t *dst16 = (int16_t *)frame->data[0];
                int32_t *dst32 = (int32_t *)frame->data[0];
                /* Interleave audio (and convert bit-depth if necessary) */
                for( int j = 0; j < MIN(frame->nb_samples, MAX_SAMPLES); j++)
                {
                    for( int k = 0; k < codec->channels; k++ )
                    {
                        int32_t *src = (int32_t*)raw_frame->audio_frame.audio_data[((output_stream->sdi_audio_pair-1)<<1)+k];

                        if( codec->bits_per_raw_sample == 16 )
                            dst16[k] = (src[j] >> 16) & 0xffff;
                        else
                            dst32[k] = src[j];
                    }

                    dst16 += codec->channels;
                    dst32 += codec->channels;
                }

                av_init_packet( &pkt );
                pkt.data = NULL;
                pkt.size = 0;

                got_pkt = 0;
                while( !got_pkt )
                {
                    int ret = avcodec_encode_audio2( codec, &pkt, frame, &got_pkt );
                    if( ret < 0 )
                    {
                        syslog( LOG_ERR, "[lavc] Audio encoding failed\n" );
                        goto finish;
                    }
                }

                coded_frame = new_coded_frame( h->encoders[i]->output_stream_id, pkt.size );
                if( !coded_frame )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    goto finish;
                }
                memcpy( coded_frame->data, pkt.data, pkt.size );
                av_free_packet( &pkt );

                coded_frame->pts = raw_frame->video_pts;
                coded_frame->duration = raw_frame->video_duration;
                coded_frame->random_access = 1; /* Every frame output is a random access point */
                add_to_queue( &h->mux_queue, coded_frame );
            }
            else /* compressed format */
            {
                split_raw_frame = new_raw_frame();
                if( !split_raw_frame )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return NULL;
                }
                memcpy( split_raw_frame, raw_frame, sizeof(*split_raw_frame) );
                memset( split_raw_frame->audio_frame.audio_data, 0, sizeof(split_raw_frame->audio_frame.audio_data) );
                split_raw_frame->audio_frame.linesize = split_raw_frame->audio_frame.num_channels = 0;
                split_raw_frame->audio_frame.channel_layout = output_stream->channel_layout;

                if( av_samples_alloc( split_raw_frame->audio_frame.audio_data, &split_raw_frame->audio_frame.linesize, num_channels,
                                      split_raw_frame->audio_frame.num_samples, split_raw_frame->audio_frame.sample_fmt, 32 ) < 0 )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return NULL;
                }

                av_samples_copy( split_raw_frame->audio_frame.audio_data,
                                 &raw_frame->audio_frame.audio_data[((output_stream->sdi_audio_pair-1)<<1)+output_stream->mono_channel], 0, 0,
                                 split_raw_frame->audio_frame.num_samples, num_channels, split_raw_frame->audio_frame.sample_fmt );

                split_raw_frame->release_data = obe_release_audio_data;
                split_raw_frame->pts += (int64_t)output_stream->audio_offset * OBE_CLOCK/1000;

                add_to_encode_queue( h, split_raw_frame, h->encoders[i]->output_stream_id );
            }
        }

        remove_from_queue( &filter->queue );
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        raw_frame = NULL;
    }

finish:
    if( frame )
       av_frame_free( &frame );

    if( frame->data )
        av_freep( &frame->data );

    if( codec )
        avcodec_free_context( &codec );

    free( filter_params );

    return NULL;
}

const obe_aud_filter_func_t audio_filter = { start_filter };
