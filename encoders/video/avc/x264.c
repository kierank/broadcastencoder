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
#include <libavutil/mathematics.h>

static void x264_logger( void *p_unused, int i_level, const char *psz_fmt, va_list arg )
{
    if( i_level <= X264_LOG_INFO )
        vsyslog( i_level == X264_LOG_INFO ? LOG_INFO : i_level == X264_LOG_WARNING ? LOG_WARNING : LOG_ERR, psz_fmt, arg );
}

static int convert_obe_to_x264_pic( x264_picture_t *pic, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    int idx = 0, count = 0;

    x264_picture_init( pic );

    memcpy( pic->img.i_stride, img->stride, sizeof(img->stride) );
    memcpy( pic->img.plane, img->plane, sizeof(img->plane) );
    pic->img.i_plane = img->planes;
    pic->img.i_csp = img->csp == PIX_FMT_YUV422P || img->csp == PIX_FMT_YUV422P10 ? X264_CSP_I422 : X264_CSP_I420;

    if( X264_BIT_DEPTH == 10 )
        pic->img.i_csp |= X264_CSP_HIGH_DEPTH;

    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        /* Only give correctly formatted data to the encoder */
        if( raw_frame->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 ||
            raw_frame->user_data[i].type == USER_DATA_AVC_UNREGISTERED )
        {
            count++;
        }
    }

    pic->extra_sei.num_payloads = count;

    if( pic->extra_sei.num_payloads )
    {
        pic->extra_sei.sei_free = free;
        pic->extra_sei.payloads = malloc( pic->extra_sei.num_payloads * sizeof(*pic->extra_sei.payloads) );

        if( !pic->extra_sei.payloads )
            return -1;

        for( int i = 0; i < raw_frame->num_user_data; i++ )
        {
            /* Only give correctly formatted data to the encoder */
            if( raw_frame->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 ||
                raw_frame->user_data[i].type == USER_DATA_AVC_UNREGISTERED )
            {
                pic->extra_sei.payloads[idx].payload_type = raw_frame->user_data[i].type;
                pic->extra_sei.payloads[idx].payload_size = raw_frame->user_data[i].len;
                pic->extra_sei.payloads[idx].payload = raw_frame->user_data[i].data;
                idx++;
            }
            else
            {
                syslog( LOG_WARNING, "Invalid user data presented to encoder - type %i \n", raw_frame->user_data[i].type );
                free( raw_frame->user_data[i].data );
            }
            /* Set the pointer to NULL so only x264 can free the data if necessary */
            raw_frame->user_data[i].data = NULL;
        }
    }
    else if( raw_frame->num_user_data )
    {
        for( int i = 0; i < raw_frame->num_user_data; i++ )
        {
            syslog( LOG_WARNING, "Invalid user data presented to encoder - type %i \n", raw_frame->user_data[i].type );
            free( raw_frame->user_data[i].data );
        }
    }

    if( raw_frame->valid_timecode )
    {
        pic->timecode[0].b_valid = 1;
        pic->timecode[0].i_hours = raw_frame->timecode.hours;
        pic->timecode[0].i_minutes = raw_frame->timecode.mins;
        pic->timecode[0].i_seconds = raw_frame->timecode.seconds;
        pic->timecode[0].i_frame = raw_frame->timecode.frames;
        pic->timecode[0].b_drop = raw_frame->timecode.drop_frame;
        pic->timecode[0].b_sync = pic->timecode[0].b_discontinuity = pic->timecode[0].i_counting_type = 0;
        pic->timecode[0].i_type = X264_TIMECODE_FULL;
    }

    return 0;
}

static void *start_encoder( void *ptr )
{
    obe_vid_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    x264_t *s = NULL;
    x264_picture_t pic, pic_out;
    x264_nal_t *nal;
    int i_nal, frame_size = 0;
    int64_t pts = 0, arrival_time = 0, frame_duration, buffer_duration;
    int64_t *pts2;
    float buffer_fill;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;

    /* TODO: check for width, height changes */

    /* Lock the mutex until we verify and fetch new parameters */
    pthread_mutex_lock( &encoder->queue.mutex );

    enc_params->avc_param.pf_log = x264_logger;
    s = x264_encoder_open( &enc_params->avc_param );
    if( !s )
    {
        pthread_mutex_unlock( &encoder->queue.mutex );
        fprintf( stderr, "[x264]: encoder configuration failed\n" );
        goto end;
    }

    x264_encoder_parameters( s, &enc_params->avc_param );

    encoder->encoder_params = malloc( sizeof(enc_params->avc_param) );
    if( !encoder->encoder_params )
    {
        pthread_mutex_unlock( &encoder->queue.mutex );
        syslog( LOG_ERR, "Malloc failed\n" );
        goto end;
    }
    memcpy( encoder->encoder_params, &enc_params->avc_param, sizeof(enc_params->avc_param) );

    encoder->is_ready = 1;
    /* XXX: This will need fixing for soft pulldown streams */
    frame_duration = av_rescale_q( 1, (AVRational){enc_params->avc_param.i_fps_den, enc_params->avc_param.i_fps_num}, (AVRational){1, OBE_CLOCK} );
    buffer_duration = frame_duration * enc_params->avc_param.sc.i_buffer_size;

    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->queue.in_cv );
    pthread_mutex_unlock( &encoder->queue.mutex );

    while( 1 )
    {
        pthread_mutex_lock( &encoder->queue.mutex );

        while( !encoder->queue.size && !encoder->cancel_thread )
            pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            break;
        }

        /* Reset the speedcontrol buffer if the source has dropped frames. Otherwise speedcontrol
         * stays in an underflow state and is locked to the fastest preset */
        pthread_mutex_lock( &h->drop_mutex );
        if( h->encoder_drop )
        {
            pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
            h->enc_smoothing_buffer_complete = 0;
            pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
            syslog( LOG_INFO, "Speedcontrol reset\n" );
            x264_speedcontrol_sync( s, enc_params->avc_param.sc.i_buffer_size, enc_params->avc_param.sc.f_buffer_init, 0 );
            h->encoder_drop = 0;
        }
        pthread_mutex_unlock( &h->drop_mutex );

        raw_frame = encoder->queue.queue[0];
        pthread_mutex_unlock( &encoder->queue.mutex );

        if( convert_obe_to_x264_pic( &pic, raw_frame ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }

        /* FIXME: if frames are dropped this might not be true */
        pic.i_pts = pts++;
        pts2 = malloc( sizeof(int64_t) );
        if( !pts2 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }
        pts2[0] = raw_frame->pts;
        pic.opaque = pts2;
        pic.param = NULL;

        /* If the AFD has changed, then change the SAR. x264 will write the SAR at the next keyframe
         * TODO: allow user to force keyframes in order to be frame accurate */
        if( raw_frame->sar_width  != enc_params->avc_param.vui.i_sar_width ||
            raw_frame->sar_height != enc_params->avc_param.vui.i_sar_height )
        {
            enc_params->avc_param.vui.i_sar_width  = raw_frame->sar_width;
            enc_params->avc_param.vui.i_sar_height = raw_frame->sar_height;

            pic.param = &enc_params->avc_param;
        }

        /* Update speedcontrol based on the system state */
        if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
        {
            pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
            if( h->enc_smoothing_buffer_complete )
            {
                /* Wait until a frame is sent out. */
                while( !h->enc_smoothing_last_exit_time )
                    pthread_cond_wait( &h->enc_smoothing_queue.out_cv, &h->enc_smoothing_queue.mutex );

                /* time elapsed since last frame was removed */
                int64_t last_frame_delta = get_input_clock_in_mpeg_ticks( h ) - h->enc_smoothing_last_exit_time;

                if( h->enc_smoothing_queue.size )
                {
                    obe_coded_frame_t *first_frame, *last_frame;
                    first_frame = h->enc_smoothing_queue.queue[0];
                    last_frame = h->enc_smoothing_queue.queue[h->enc_smoothing_queue.size-1];
                    int64_t frame_durations = last_frame->real_dts - first_frame->real_dts + frame_duration;
                    buffer_fill = (float)(frame_durations - last_frame_delta)/buffer_duration;
                }
                else
                    buffer_fill = (float)(-1 * last_frame_delta)/buffer_duration;

                x264_speedcontrol_sync( s, buffer_fill, enc_params->avc_param.sc.i_buffer_size, 1 );
            }

            pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
        }

        frame_size = x264_encoder_encode( s, &nal, &i_nal, &pic, &pic_out );

        arrival_time = raw_frame->arrival_time;
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_from_queue( &encoder->queue );

        if( frame_size < 0 )
        {
            syslog( LOG_ERR, "x264_encoder_encode failed\n" );
            break;
        }

        if( frame_size )
        {
            coded_frame = new_coded_frame( encoder->output_stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                break;
            }
            memcpy( coded_frame->data, nal[0].p_payload, frame_size );
            coded_frame->is_video = 1;
            coded_frame->len = frame_size;
            coded_frame->cpb_initial_arrival_time = pic_out.hrd_timing.cpb_initial_arrival_time;
            coded_frame->cpb_final_arrival_time = pic_out.hrd_timing.cpb_final_arrival_time;
            coded_frame->real_dts = pic_out.hrd_timing.cpb_removal_time;
            coded_frame->real_pts = pic_out.hrd_timing.dpb_output_time;
            pts2 = pic_out.opaque;
            coded_frame->pts = pts2[0];
            coded_frame->random_access = pic_out.b_keyframe;
            coded_frame->priority = IS_X264_TYPE_I( pic_out.i_type );
            free( pic_out.opaque );

            if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY )
            {
                coded_frame->arrival_time = arrival_time;
                add_to_queue( &h->mux_queue, coded_frame );
                //printf("\n Encode Latency %"PRIi64" \n", obe_mdate() - coded_frame->arrival_time );
            }
            else
                add_to_queue( &h->enc_smoothing_queue, coded_frame );
        }
     }

end:
    if( s )
        x264_encoder_close( s );
    free( enc_params );

    return NULL;
}

const obe_vid_enc_func_t x264_encoder = { start_encoder };
