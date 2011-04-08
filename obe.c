/*****************************************************************************
 * obe.c: open broadcast encoder functions
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
 *****************************************************************************/

#include "common/common.h"
#include "input/input.h"
#include "encoders/video/video.h"
#include "mux/mux.h"
#include "output/output.h"

/** Create/Destroy **/
/* Input device */
obe_device_t *new_device( void )
{
    obe_device_t *device = calloc( 1, sizeof(obe_device_t) );

    if( !device )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return NULL;
    }

    return device;
}

void destroy_device( obe_device_t *device )
{
    if( device->location )
        free( device->location );
    free( device );
}

/* Raw frame */
obe_raw_frame_t *new_raw_frame( void )
{
    obe_raw_frame_t *raw_frame = calloc( 1, sizeof(obe_raw_frame_t) );

    if( !raw_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return NULL;
    }

    return raw_frame;
}

/* Coded frame */
obe_coded_frame_t *new_coded_frame( int stream_id, int len )
{
    obe_coded_frame_t *coded_frame = calloc( 1, sizeof(*coded_frame) );
    if( !coded_frame )
        return NULL;

    coded_frame->stream_id = stream_id;
    coded_frame->len = len;
    coded_frame->data = malloc( len );
    if( !coded_frame->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        free( coded_frame );
        return NULL;
    }

    return coded_frame;
}

void destroy_coded_frame( obe_coded_frame_t *coded_frame )
{
    free( coded_frame->data );
    free( coded_frame );
}

/* Muxed data */
obe_muxed_data_t *new_muxed_data( int len )
{
    obe_muxed_data_t *muxed_data = calloc( 1, sizeof(*muxed_data) );
    if( !muxed_data )
        return NULL;

    muxed_data->bytes_left = len;
    muxed_data->data = malloc( len );
    if( !muxed_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        free( muxed_data );
        return NULL;
    }
    muxed_data->cur_pos = muxed_data->data;

    return muxed_data;
}

void destroy_muxed_data( obe_muxed_data_t *muxed_data )
{
    if( muxed_data->pcr_list )
        free( muxed_data->pcr_list );

    free( muxed_data->data );
    free( muxed_data );
}

/** Add/Remove from queues */
int add_to_filter_queue( obe_t *h, obe_raw_frame_t *raw_frame )
{
    obe_filter_t *filter = NULL;
    obe_raw_frame_t **tmp;

    /* TODO: when only the fly reconfig is enabled, lock mutex */
    for( int i = 0; i < h->num_filters; i++ )
    {
        for( int j = 0; j < h->filters[i]->num_stream_ids; j++ )
        {
            if( h->filters[i]->stream_id_list[j] == raw_frame->stream_id )
                filter = h->filters[i];
        }
    }

    if( !filter )
        return -1;

    pthread_mutex_lock( &filter->filter_mutex );
    tmp = realloc( filter->frames, sizeof(*filter->frames) * (filter->num_raw_frames+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    filter->frames = tmp;
    filter->frames[filter->num_raw_frames++] = raw_frame;
    pthread_cond_signal( &filter->filter_cv );
    pthread_mutex_unlock( &filter->filter_mutex );

    return 0;
}

int remove_frame_from_filter_queue( obe_filter_t *filter )
{
    obe_raw_frame_t **tmp;

    pthread_mutex_lock( &filter->filter_mutex );
    if( filter->num_raw_frames > 1 )
        memmove( &filter->frames[0], &filter->frames[1], sizeof(*filter->frames) * (filter->num_raw_frames-1) );
    tmp = realloc( filter->frames, sizeof(*filter->frames) * (filter->num_raw_frames-1) );
    filter->num_raw_frames--;
    if( !tmp && filter->num_raw_frames )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    filter->frames = tmp;
    pthread_mutex_unlock( &filter->filter_mutex );

    return 0;
}

int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame )
{
    obe_encoder_t *encoder = NULL;
    obe_raw_frame_t **tmp;

    /* TODO: when only the fly reconfig is enabled, lock mutex */
    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->stream_id == raw_frame->stream_id )
            encoder = h->encoders[i];
    }

    if( !encoder )
        return -1;

    pthread_mutex_lock( &encoder->encoder_mutex );
    tmp = realloc( encoder->frames, sizeof(*encoder->frames) * (encoder->num_raw_frames+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    encoder->frames = tmp;
    encoder->frames[encoder->num_raw_frames++] = raw_frame;
    pthread_cond_signal( &encoder->encoder_cv );
    pthread_mutex_unlock( &encoder->encoder_mutex );

    return 0;
}

int remove_frame_from_encode_queue( obe_encoder_t *encoder )
{
    obe_raw_frame_t **tmp;

    pthread_mutex_lock( &encoder->encoder_mutex );
    if( encoder->num_raw_frames > 1 )
        memmove( &encoder->frames[0], &encoder->frames[1], sizeof(*encoder->frames) * (encoder->num_raw_frames-1) );
    tmp = realloc( encoder->frames, sizeof(*encoder->frames) * (encoder->num_raw_frames-1) );
    encoder->num_raw_frames--;
    if( !tmp && encoder->num_raw_frames )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    encoder->frames = tmp;
    pthread_mutex_unlock( &encoder->encoder_mutex );

    return 0;
}

int add_to_mux_queue( obe_t *h, obe_coded_frame_t *coded_frame )
{
    obe_coded_frame_t **tmp;

    pthread_mutex_lock( &h->mux_mutex );
    tmp = realloc( h->coded_frames, sizeof(*h->coded_frames) * (h->num_coded_frames+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    h->coded_frames = tmp;
    h->coded_frames[h->num_coded_frames++] = coded_frame;
    pthread_cond_signal( &h->mux_cv );
    pthread_mutex_unlock( &h->mux_mutex );

    return 0;
}

int remove_from_mux_queue( obe_t *h, obe_coded_frame_t *coded_frame )
{
    obe_coded_frame_t **tmp;
    pthread_mutex_lock( &h->mux_mutex );
    for( int i = 0; i < h->num_coded_frames; i++ )
    {
        if( h->coded_frames[i] == coded_frame )
        {
            memmove( &h->coded_frames[i], &h->coded_frames[i+1], sizeof(*h->coded_frames) * (h->num_coded_frames-1-i) );
            tmp = realloc( h->coded_frames, sizeof(*h->coded_frames) * (h->num_coded_frames-1) );
            h->num_coded_frames--;
            if( !tmp && h->num_coded_frames )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            h->coded_frames = tmp;
            break;
        }
    }
    pthread_mutex_unlock( &h->mux_mutex );

    return 0;
}

int remove_early_frames( obe_t *h, int64_t pts )
{
    obe_coded_frame_t **tmp;
    for( int i = 0; i < h->num_coded_frames; i++ )
    {
        if( !h->coded_frames[i]->is_video && h->coded_frames[i]->pts < pts )
        {
            destroy_coded_frame( h->coded_frames[i] );
            memmove( &h->coded_frames[i], &h->coded_frames[i+1], sizeof(*h->coded_frames) * (h->num_coded_frames-1-i) );
            tmp = realloc( h->coded_frames, sizeof(*h->coded_frames) * (h->num_coded_frames-1) );
            h->num_coded_frames--;
            i--;
            if( !tmp && h->num_coded_frames )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            h->coded_frames = tmp;
        }
    }

    return 0;
}

int add_to_output_queue( obe_t *h, obe_muxed_data_t *muxed_data )
{
    obe_muxed_data_t **tmp;

    pthread_mutex_lock( &h->output_mutex );
    tmp = realloc( h->muxed_data, sizeof(*h->muxed_data) * (h->num_muxed_data+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    h->muxed_data = tmp;
    h->muxed_data[h->num_muxed_data++] = muxed_data;
    pthread_cond_signal( &h->output_cv );
    pthread_mutex_unlock( &h->output_mutex );

    return 0;
}

int remove_from_output_queue( obe_t *h )
{
    obe_muxed_data_t **tmp;

    pthread_mutex_lock( &h->output_mutex );
    if( h->num_muxed_data > 1 )
        memmove( &h->muxed_data[0], &h->muxed_data[1], sizeof(*h->muxed_data) * (h->num_muxed_data-1) );
    tmp = realloc( h->muxed_data, sizeof(*h->muxed_data) * (h->num_muxed_data-1) );
    h->num_muxed_data--;
    if( !tmp && h->num_muxed_data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    h->muxed_data = tmp;
    pthread_mutex_unlock( &h->output_mutex );

    return 0;
}

/** Get items **/
/* Input stream */
obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id )
{
    /* TODO lock and unlock when we can reconfig input streams */
    for( int j = 0; j < h->devices[0]->num_input_streams; j++ )
    {
        if( h->devices[0]->streams[j]->stream_id == input_stream_id )
            return h->devices[0]->streams[j];
    }
    return NULL;
}

/* Encoder */
obe_encoder_t *get_encoder( obe_t *h, int stream_id )
{
    /* TODO lock and unlock when we can reconfig encoders */
    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->stream_id == stream_id )
            return h->encoders[i];
    }
    return NULL;
}

obe_t *obe_setup( void )
{
    openlog( "obe", LOG_NDELAY | LOG_PID, LOG_USER );

    obe_t *h = calloc( 1, sizeof(*h) );
    if( !h )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    pthread_mutex_init( &h->device_list_mutex, NULL );

    return h;
}

int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_program_t *program )
{
    pthread_t thread;
    obe_int_input_stream_t *stream_in;
    obe_input_stream_t *stream_out;
    int rc;
    int probe_time = MAX_PROBE_TIME;
    int i = 0;
    int prev_devices = h->num_devices;
    int cur_devices;

    if( input_device == NULL || program == NULL )
    {
        fprintf( stderr, "Invalid input pointers \n" );
        return -1;
    }

    if( !input_device->location )
    {
        fprintf( stderr, "Invalid input location\n" );
        return -1;
    }

    if( h->num_devices == MAX_DEVICES )
    {
        fprintf( stderr, "No more devices allowed. \n" );
        return -1;
    }

    obe_input_probe_t *args = malloc( sizeof(*args) );
    if( !args )
        goto fail;

    args->h = h;
    args->location = malloc( strlen( input_device->location ) + 1 );
    if( !args->location )
        goto fail;

    strcpy( args->location, input_device->location );

    rc = pthread_create( &thread, NULL, lavf_input.probe_input, (void*)args );

    if( rc < 0 )
    {
        fprintf( stderr, "Couldn't create probe thread \n" );
        return -1;
    }

    printf( "Probing device \"%s\". Timeout %i seconds\n", input_device->location, probe_time );

    while( i++ < probe_time )
    {
        sleep( 1 );
        fprintf( stderr, "." );

        if( pthread_kill( thread, 0 ) == ESRCH )
            break;
    }

    pthread_cancel( thread );

    cur_devices = h->num_devices;

    if( args )
        free( args );

    if( prev_devices == cur_devices )
    {
        fprintf( stderr, "Could not probe device \n" );
        program = NULL;
        return -1;
    }

    // FIXME this needs to be made to support probing whilst running
    // TODO metadata etc
    program->num_streams = h->devices[h->num_devices-1]->num_input_streams;
    program->streams = calloc( 1, program->num_streams * sizeof(*program->streams) );
    if( !program->streams )
        goto fail;

    h->devices[h->num_devices-1]->probed_streams = program->streams;

    for( i = 0; i < program->num_streams; i++ )
    {
        stream_in = h->devices[h->num_devices-1]->streams[i];
        stream_out = &program->streams[i];

        stream_out->stream_id = stream_in->stream_id;
        stream_out->stream_type = stream_in->stream_type;
        stream_out->stream_format = stream_in->stream_format;

        stream_out->bitrate = stream_in->bitrate;
        if( stream_in->stream_type == STREAM_TYPE_VIDEO )
        {
            memcpy( &stream_out->csp, &stream_in->csp, offsetof( obe_input_stream_t, timebase_num ) - offsetof( obe_input_stream_t, csp ) );
            stream_out->timebase_num = stream_in->timebase_num;
            stream_out->timebase_den = stream_in->timebase_den;
        }
        else if( stream_in->stream_type == STREAM_TYPE_AUDIO )
        {
            memcpy( &stream_out->channel_layout, &stream_in->channel_layout,
            offsetof( obe_input_stream_t, bitrate ) - offsetof( obe_input_stream_t, channel_layout ) );
            stream_out->aac_is_latm = stream_in->is_latm;
        }

        memcpy( stream_out->lang_code, stream_in->lang_code, 4 );
    }

    return 0;

fail:

    if( args )
        free( args );

    fprintf( stderr, "Malloc failed. \n" );
    return -1;
}

int obe_populate_avc_encoder_params( obe_t *h, int input_stream_id, x264_param_t *param )
{
    obe_int_input_stream_t *stream = get_input_stream( h, input_stream_id );
    if( !stream )
    {
        fprintf( stderr, "Could not find stream \n" );
        return -1;
    }

    if( stream->stream_type != STREAM_TYPE_VIDEO )
    {
        fprintf( stderr, "Stream type is not video \n" );
        return -1;
    }

    if( !param )
    {
        fprintf( stderr, "Invalid parameter pointer \n" );
        return -1;
    }

    x264_param_default( param );
    param->b_vfr_input = 0;
    param->b_pic_struct = 1;
    param->i_open_gop = X264_OPEN_GOP_NORMAL;

    param->i_width = stream->width;
    param->i_height = stream->height;

    param->i_fps_num = stream->timebase_den;
    param->i_fps_den = stream->timebase_num;
    param->b_interlaced = stream->interlaced;
    if( param->b_interlaced )
        param->b_tff = stream->tff;

    param->vui.i_sar_width  = stream->sar_num;
    param->vui.i_sar_height = stream->sar_den;

    param->vui.i_overscan = 2;

    if( ( param->i_fps_num == 25 || param->i_fps_num == 50 ) && param->i_fps_den == 1 )
    {
        param->vui.i_vidformat = 1; // PAL
        param->vui.i_colorprim = 5; // BT.470-2 bg
        param->vui.i_transfer  = 5; // BT.470-2 bg
        param->vui.i_colmatrix = 5; // BT.470-2 bg
        param->i_keyint_max = param->i_fps_num >> 1;
    }
    else if( ( param->i_fps_num == 30000 || param->i_fps_num == 60000 ) && param->i_fps_den == 1001 )
    {
        param->vui.i_vidformat = 2; // NTSC
        param->vui.i_colorprim = 6; // BT.601-6
        param->vui.i_transfer  = 6; // BT.601-6
        param->vui.i_colmatrix = 6; // BT.601-6
        param->i_keyint_max = (param->i_fps_num / 1000) >> 1;
    }
    else
    {
        param->vui.i_vidformat = 5; // undefined
        param->vui.i_colorprim = 2; // undefined
        param->vui.i_transfer  = 2; // undefined
        param->vui.i_colmatrix = 2; // undefined
    }

    /* Change to BT.709 and set high profile for HD resolutions */
    if( param->i_width >= 1280 && param->i_height >= 720 )
    {
        param->vui.i_colorprim = 1;
        param->vui.i_transfer  = 1;
        param->vui.i_colmatrix = 1;
        x264_param_apply_profile( param, "high" );
    }
    else
        x264_param_apply_profile( param, "main" );

    param->sc.f_speed = 1.0;
    param->b_aud = 1;
    param->i_nal_hrd = X264_NAL_HRD_FAKE_VBR;
    //param->i_log_level = X264_LOG_NONE;

    return 0;
}

int obe_setup_streams( obe_t *h, obe_output_stream_t *output_streams, int num_streams )
{
    if( num_streams <= 0 )
    {
        fprintf( stderr, "Must have at least one stream \n" );
        return -1;
    }
    // TODO sanity check the inputs

    h->num_output_streams = num_streams;
    /* TODO deal with updating case */
    h->output_streams = malloc( num_streams * sizeof(*h->output_streams) );
    if( !h->output_streams )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }
    memcpy( h->output_streams, output_streams, num_streams * sizeof(*h->output_streams) );

    return 0;
}

int obe_setup_muxer( obe_t *h, obe_mux_opts_t *mux_opts )
{
    // TODO sanity check

    memcpy( &h->mux_opts, mux_opts, sizeof(obe_mux_opts_t) );
    return 0;
}

int obe_start( obe_t *h )
{
    int rc;
    obe_vid_enc_params_t *enc_params;
    obe_output_params_t *out_params;
    obe_output_func_t output;

    // sanity check

    if( h->mux_opts.muxer == OUTPUT_UDP )
        output = udp_output;
    else
        output = rtp_output;

    /* Open Output Thread */
    pthread_mutex_init( &h->output_mutex, NULL );
    pthread_cond_init( &h->output_cv, NULL );

    out_params = malloc( sizeof(*out_params) );
    if( !out_params )
    {
        // TODO fail
    }
    out_params->h = h;
    rc = pthread_create( &h->output_thread, NULL, output.open_output, (void*)out_params );

    if( rc < 0 )
    {
        fprintf( stderr, "Couldn't create output thread \n" );
        return -1;
    }

    struct sched_param s_param = {0};
    s_param.sched_priority = 99;

    pthread_setschedparam( h->output_thread, SCHED_RR, &s_param );

    /* Open Encoder Threads */
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        if( h->output_streams[i].stream_action == STREAM_ENCODE )
        {
            h->encoders[h->num_encoders] = calloc( 1, sizeof(obe_encoder_t) );
            if( !h->encoders[h->num_encoders] )
            {
                // TODO fail
            }
            h->encoders[h->num_encoders]->stream_id = h->output_streams[i].stream_id;
            pthread_mutex_init( &h->encoders[h->num_encoders]->encoder_mutex, NULL );
            pthread_cond_init( &h->encoders[h->num_encoders]->encoder_cv, NULL );
            enc_params = calloc( 1, sizeof(obe_vid_enc_params_t) );
            if( !enc_params )
            {
                // TODO fail
            }
            enc_params->h = h;
            enc_params->encoder = h->encoders[h->num_encoders];
            memcpy( &enc_params->avc_param, &h->output_streams[i].avc_param, sizeof(x264_param_t) );
            rc = pthread_create( &h->encoders[h->num_encoders]->encoder_thread, NULL, x264_encoder.start_encoder, (void*)enc_params );
            h->num_encoders++;
        }
    }

    /* Open Mux Thread */
    obe_mux_params_t *mux_params = calloc( 1, sizeof(*mux_params) );
    // FIXME fail
    mux_params->h = h;
    mux_params->device = h->devices[0];
    mux_params->num_output_streams = h->num_output_streams;
    mux_params->output_streams = h->output_streams;
    // FIXME fail
    rc = pthread_create( &h->mux_thread, NULL, ts_muxer.open_muxer, (void*)mux_params );

    if( rc < 0 )
    {
        fprintf( stderr, "Couldn't create output thread \n" );
        return -1;
    }

    struct sched_param s_param2 = {0};
    s_param2.sched_priority = 50;

    pthread_setschedparam( h->mux_thread, SCHED_RR, &s_param2 );

    /* Open Filter Thread */

    /* Open Input Thread */
    pthread_mutex_init( &h->devices[0]->device_mutex, NULL );
    obe_input_params_t *input_params = calloc( 1, sizeof(*input_params) );
    // FIXME fail
    input_params->h = h;
    input_params->device = h->devices[0];
    input_params->num_output_streams = h->num_output_streams;
    input_params->output_streams = h->output_streams;
    // FIXME fail

    // TODO: in the future give it only the streams which are necessary
    rc = pthread_create( &h->devices[0]->device_thread, NULL, lavf_input.open_input, (void*)input_params );

    h->is_active = 1;

    return 0;
};

void obe_close( obe_t *h )
{



    free( h );
}
