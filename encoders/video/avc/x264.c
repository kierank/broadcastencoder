/*****************************************************************************
 * x264.c : x264 encoding functions
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
#include "encoders/video/video.h"

static void convert_cli_to_lib_pic( x264_picture_t *lib, cli_image_t *img )
{
    memcpy( lib->img.i_stride, img->stride, sizeof(img->stride) );
    memcpy( lib->img.plane, img->plane, sizeof(img->plane) );
    lib->img.i_plane = img->planes;
    lib->img.i_csp = X264_CSP_I420;
}

static void *start_encoder( void *ptr )
{
    obe_vid_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    x264_t *s;
    x264_picture_t pic, pic_out;
    x264_nal_t *nal;
    int i_nal, frame_size = 0;
    int64_t pts = 0;
    int64_t *pts2;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    int i = 0;

    /* TODO check for width, height changes */

    /* Lock the mutex until we verify and fetch new parameters */
    pthread_mutex_lock( &encoder->encoder_mutex );

    s = x264_encoder_open( &enc_params->avc_param );
    if( !s )
    {
        pthread_mutex_unlock( &encoder->encoder_mutex );
        fprintf( stderr, "[x264]: encoder configuration failed\n" );
        return NULL;
    }

    x264_encoder_parameters( s, &enc_params->avc_param );
    x264_picture_init( &pic );

    encoder->encoder_params = malloc( sizeof(enc_params->avc_param) );
    if( !encoder->encoder_params )
    {
        pthread_mutex_unlock( &encoder->encoder_mutex );
        syslog( LOG_ERR, "Malloc failed\n" );
        goto fail;
    }
    memcpy( encoder->encoder_params, &enc_params->avc_param, sizeof(enc_params->avc_param) );

    encoder->is_ready = 1;
    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->encoder_cv );
    pthread_mutex_unlock( &encoder->encoder_mutex );

    while( 1 )
    {
        pthread_mutex_lock( &encoder->encoder_mutex );

        if( !encoder->num_raw_frames )
            pthread_cond_wait( &encoder->encoder_cv, &encoder->encoder_mutex );

        raw_frame = encoder->frames[0];
        pthread_mutex_unlock( &encoder->encoder_mutex );

        /* TODO handle user data */
        convert_cli_to_lib_pic( &pic, &raw_frame->img );
        pic.i_pts = pts++;
        pts2 = malloc( sizeof(int64_t) );
        if( !pts2 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto fail;
        }
        *pts2 = raw_frame->pts;
        pic.passthrough_opaque = pts2;

        frame_size = x264_encoder_encode( s, &nal, &i_nal, &pic, &pic_out );
        i++;

        raw_frame->release_frame( raw_frame );
        remove_frame_from_encode_queue( encoder );

        if( frame_size < 0 )
        {
            syslog( LOG_ERR, "x264_encoder_encode failed\n" );
            goto fail;
        }

        if( frame_size )
        {
            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
	    if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto fail;
            }
            memcpy( coded_frame->data, nal[0].p_payload, frame_size );
            coded_frame->is_video = 1;
            coded_frame->len = frame_size;
            coded_frame->real_dts = (int64_t)((pic_out.hrd_timing.cpb_removal_time * 90000LL) + 0.5);
            coded_frame->real_pts = (int64_t)((pic_out.hrd_timing.dpb_output_time * 90000LL) + 0.5);
            coded_frame->pts = *(int64_t*)pic_out.passthrough_opaque;
            coded_frame->random_access = pic_out.b_keyframe;
            coded_frame->priority = IS_X264_TYPE_I( pic_out.i_type );
            free( pic_out.passthrough_opaque );

            add_to_mux_queue( h, coded_frame );
        }
    }

fail:
    x264_encoder_close( s );
    free( ptr );

    return NULL;
}

const obe_vid_enc_func_t x264_encoder = { start_encoder };
