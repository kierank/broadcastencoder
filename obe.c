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
#include "common/lavc.h"
#include "input/input.h"
#include "filters/video/video.h"
#include "filters/audio/audio.h"
#include "encoders/video/video.h"
#include "encoders/audio/audio.h"
#include "mux/mux.h"
#include "output/output.h"

/** Utilities **/
int64_t obe_mdate( void )
{
    struct timespec ts_current;
    clock_gettime( CLOCK_MONOTONIC, &ts_current );
    return (int64_t)ts_current.tv_sec * 1000000 + (int64_t)ts_current.tv_nsec / 1000;
}

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
    for( int i = 0; i < device->num_input_streams; i++ )
        free( device->streams[i] );
    if( device->probed_streams )
        free( device->probed_streams );
    if( device->location )
        free( device->location );
    free( device );
}

/* Raw frame */
obe_raw_frame_t *new_raw_frame( void )
{
    obe_raw_frame_t *raw_frame = calloc( 1, sizeof(*raw_frame) );

    if( !raw_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return NULL;
    }

    return raw_frame;
}

/* Coded frame */
obe_coded_frame_t *new_coded_frame( int output_stream_id, int len )
{
    obe_coded_frame_t *coded_frame = calloc( 1, sizeof(*coded_frame) );
    if( !coded_frame )
        return NULL;

    coded_frame->output_stream_id = output_stream_id;
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

void obe_release_video_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->alloc_img.plane[0] );
}

void obe_release_audio_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->audio_frame.audio_data[0] );
}

void obe_release_frame( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     for( int i = 0; i < raw_frame->num_user_data; i++ )
         free( raw_frame->user_data[i].data );
     free( raw_frame->user_data );
     free( raw_frame );
}

/* Muxed data */
obe_muxed_data_t *new_muxed_data( int len )
{
    obe_muxed_data_t *muxed_data = calloc( 1, sizeof(*muxed_data) );
    if( !muxed_data )
        return NULL;

    muxed_data->len = len;
    muxed_data->data = malloc( len );
    if( !muxed_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        free( muxed_data );
        return NULL;
    }

    return muxed_data;
}

void destroy_muxed_data( obe_muxed_data_t *muxed_data )
{
    if( muxed_data->pcr_list )
        free( muxed_data->pcr_list );

    free( muxed_data->data );
    free( muxed_data );
}

/** Add/Remove misc **/
void add_device( obe_t *h, obe_device_t *device )
{
    pthread_mutex_lock( &h->device_list_mutex );
    h->devices[h->num_devices++] = device;
    pthread_mutex_unlock( &h->device_list_mutex );
}

/** Add/Remove from queues */
void obe_init_queue( obe_queue_t *queue )
{
    pthread_mutex_init( &queue->mutex, NULL );
    pthread_cond_init( &queue->in_cv, NULL );
    pthread_cond_init( &queue->out_cv, NULL );
}

void obe_destroy_queue( obe_queue_t *queue )
{
    free( queue->queue );

    pthread_mutex_unlock( &queue->mutex );
    pthread_mutex_destroy( &queue->mutex );
    pthread_cond_destroy( &queue->in_cv );
    pthread_cond_destroy( &queue->out_cv );
}

int add_to_queue( obe_queue_t *queue, void *item )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    queue->queue = tmp;
    queue->queue[queue->size++] = item;

    pthread_cond_signal( &queue->in_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

int remove_from_queue( obe_queue_t *queue )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    if( queue->size > 1 )
        memmove( &queue->queue[0], &queue->queue[1], sizeof(*queue->queue) * (queue->size-1) );
    tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size-1) );
    queue->size--;
    if( !tmp && queue->size )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    queue->queue = tmp;

    pthread_cond_signal( &queue->out_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

int remove_item_from_queue( obe_queue_t *queue, void *item )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        if( queue->queue[i] == item )
        {
            memmove( &queue->queue[i], &queue->queue[i+1], sizeof(*queue->queue) * (queue->size-1-i) );
            tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size-1) );
            queue->size--;
            if( !tmp && queue->size )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            queue->queue = tmp;
            break;
        }
    }

    pthread_cond_signal( &queue->out_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

/* Filter queue */
int add_to_filter_queue( obe_t *h, obe_raw_frame_t *raw_frame )
{
    obe_filter_t *filter = NULL;

    for( int i = 0; i < h->num_filters; i++ )
    {
        for( int j = 0; j < h->filters[i]->num_stream_ids; j++ )
        {
            if( h->filters[i]->stream_id_list[j] == raw_frame->input_stream_id )
            {
                filter = h->filters[i];
                break;
            }
        }
    }

    if( !filter )
        return -1;

    return add_to_queue( &filter->queue, raw_frame );
}

static void destroy_filter( obe_filter_t *filter )
{
    obe_raw_frame_t *raw_frame;
    pthread_mutex_lock( &filter->queue.mutex );
    for( int i = 0; i < filter->queue.size; i++ )
    {
        raw_frame = filter->queue.queue[i];
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    obe_destroy_queue( &filter->queue );

    free( filter->stream_id_list );
    free( filter );
}

/* Encode queue */
int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame, int output_stream_id )
{
    obe_encoder_t *encoder = NULL;

    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->output_stream_id == output_stream_id )
        {
            encoder = h->encoders[i];
            break;
        }
    }

    if( !encoder )
        return -1;

    return add_to_queue( &encoder->queue, raw_frame );
}

static void destroy_encoder( obe_encoder_t *encoder )
{
    obe_raw_frame_t *raw_frame;
    pthread_mutex_lock( &encoder->queue.mutex );
    for( int i = 0; i < encoder->queue.size; i++ )
    {
        raw_frame = encoder->queue.queue[i];
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    obe_destroy_queue( &encoder->queue );

    if( encoder->encoder_params )
        free( encoder->encoder_params );

    free( encoder );
}

static void destroy_enc_smoothing( obe_queue_t *queue )
{
    obe_coded_frame_t *coded_frame;
    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        coded_frame = queue->queue[i];
        destroy_coded_frame( coded_frame );
    }

    obe_destroy_queue( queue );
}

static void destroy_mux( obe_t *h )
{
    pthread_mutex_lock( &h->mux_queue.mutex );
    for( int i = 0; i < h->mux_queue.size; i++ )
        destroy_coded_frame( h->mux_queue.queue[i] );

    obe_destroy_queue( &h->mux_queue );

    if( h->mux_opts.service_name )
        free( h->mux_opts.service_name );
    if( h->mux_opts.provider_name )
        free( h->mux_opts.provider_name );
}

static void destroy_mux_smoothing( obe_queue_t *queue )
{
    obe_muxed_data_t *muxed_data;
    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        muxed_data = queue->queue[i];
        destroy_muxed_data( muxed_data );
    }

    obe_destroy_queue( queue );
}

int remove_early_frames( obe_t *h, int64_t pts )
{
    void **tmp;
    for( int i = 0; i < h->mux_queue.size; i++ )
    {
        obe_coded_frame_t *frame = h->mux_queue.queue[i];
        if( !frame->is_video && frame->pts < pts )
        {
            destroy_coded_frame( frame );
            memmove( &h->mux_queue.queue[i], &h->mux_queue.queue[i+1], sizeof(*h->mux_queue.queue) * (h->mux_queue.size-1-i) );
            tmp = realloc( h->mux_queue.queue, sizeof(*h->mux_queue.queue) * (h->mux_queue.size-1) );
            h->mux_queue.size--;
            i--;
            if( !tmp && h->mux_queue.size )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            h->mux_queue.queue = tmp;
        }
    }

    return 0;
}

/* Output queue */
static void destroy_output( obe_output_t *output )
{
    pthread_mutex_lock( &output->queue.mutex );
    for( int i = 0; i < output->queue.size; i++ )
        av_buffer_unref( output->queue.queue[i] );

    obe_destroy_queue( &output->queue );
    free( output );
}

/** Get items **/
/* Input stream */
obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id )
{
    for( int j = 0; j < h->devices[0]->num_input_streams; j++ )
    {
        if( h->devices[0]->streams[j]->input_stream_id == input_stream_id )
            return h->devices[0]->streams[j];
    }
    return NULL;
}

/* Encoder */
obe_encoder_t *get_encoder( obe_t *h, int output_stream_id )
{
    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->output_stream_id == output_stream_id )
            return h->encoders[i];
    }
    return NULL;
}

/* Output */
obe_output_stream_t *get_output_stream( obe_t *h, int output_stream_id )
{
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        if( h->output_streams[i].output_stream_id == output_stream_id )
            return &h->output_streams[i];
    }
    return NULL;
}

obe_output_stream_t *get_output_stream_by_format( obe_t *h, int format )
{
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        if( h->output_streams[i].stream_format == format )
            return &h->output_streams[i];
    }
    return NULL;
}

obe_t *obe_setup( void )
{
    openlog( "obe", LOG_NDELAY | LOG_PID, LOG_USER );

    if( X264_BIT_DEPTH == 9 || X264_BIT_DEPTH > 10 )
    {
        fprintf( stderr, "x264 bit-depth of %i not supported\n", X264_BIT_DEPTH );
        return NULL;
    }

    obe_t *h = calloc( 1, sizeof(*h) );
    if( !h )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    pthread_mutex_init( &h->device_list_mutex, NULL );

    if( av_lockmgr_register( obe_lavc_lockmgr ) < 0 )
    {
        fprintf( stderr, "Could not register lavc lock manager\n" );
        free( h );
        return NULL;
    }

    return h;
}

int obe_set_config( obe_t *h, int system_type )
{
    if( system_type < OBE_SYSTEM_TYPE_GENERIC && system_type > OBE_SYSTEM_TYPE_LOW_LATENCY )
    {
        fprintf( stderr, "Invalid OBE system type\n" );
        return -1;
    }

    h->obe_system = system_type;

    return 0;
}

/* TODO handle error conditions */
int64_t get_wallclock_in_mpeg_ticks( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );

    return ((int64_t)ts.tv_sec * (int64_t)27000000) + (int64_t)(ts.tv_nsec * 27 / 1000);
}

void sleep_mpeg_ticks( int64_t i_time )
{
    struct timespec ts;
    ts.tv_sec = i_time / 27000000;
    ts.tv_nsec = ((i_time % 27000000) * 1000) / 27;

    clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, &ts );
}

void obe_clock_tick( obe_t *h, int64_t value )
{
    /* Use this signal as the SDI clocksource */
    pthread_mutex_lock( &h->obe_clock_mutex );
    h->obe_clock_last_pts = value;
    h->obe_clock_last_wallclock = get_wallclock_in_mpeg_ticks();
    pthread_mutex_unlock( &h->obe_clock_mutex );
    pthread_cond_broadcast( &h->obe_clock_cv );
}

int64_t get_input_clock_in_mpeg_ticks( obe_t *h )
{
    int64_t value;
    pthread_mutex_lock( &h->obe_clock_mutex );
    value = h->obe_clock_last_pts + ( get_wallclock_in_mpeg_ticks() - h->obe_clock_last_wallclock );
    pthread_mutex_unlock( &h->obe_clock_mutex );

    return value;
}

void sleep_input_clock( obe_t *h, int64_t i_time )
{
    int64_t wallclock_time;
    pthread_mutex_lock( &h->obe_clock_mutex );
    wallclock_time = ( i_time - h->obe_clock_last_pts ) + h->obe_clock_last_wallclock;
    pthread_mutex_unlock( &h->obe_clock_mutex );

    sleep_mpeg_ticks( wallclock_time );
}

int get_non_display_location( int type )
{
    /* Set the appropriate location */
    for( int i = 0; non_display_data_locations[i].service != -1; i++ )
    {
        if( non_display_data_locations[i].service == type )
            return non_display_data_locations[i].location;
    }

    return -1;
}

static int obe_validate_input_params( obe_input_t *input_device )
{
    if( input_device->input_type == INPUT_DEVICE_DECKLINK )
    {
        /* TODO check */
    }

    return 0;
}

static int __pthread_cancel( pthread_t thread )
{
    if ( thread )
        return pthread_cancel( thread );
    return -1;
}

static int __pthread_join( pthread_t thread, void **retval )
{
    if ( thread )
        return pthread_join( thread, retval );
    return -1;
}

int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_program_t *program )
{
    pthread_t thread;
    void *ret_ptr;
    obe_int_input_stream_t *stream_in;
    obe_input_stream_t *stream_out;
    obe_input_probe_t *args = NULL;

    obe_input_func_t  input;

    int probe_time = MAX_PROBE_TIME;
    int i = 0;
    int prev_devices = h->num_devices;
    int cur_devices;

    if( !input_device || !program )
    {
        fprintf( stderr, "Invalid input pointers \n" );
        return -1;
    }

    if( h->num_devices == MAX_DEVICES )
    {
        fprintf( stderr, "No more devices allowed \n" );
        return -1;
    }

    if( input_device->input_type == INPUT_URL )
    {
        //input = lavf_input;
        fprintf( stderr, "URL input is not supported currently \n" );
        goto fail;
    }
#if HAVE_DECKLINK
    else if( input_device->input_type == INPUT_DEVICE_DECKLINK )
        input = decklink_input;
#endif
    else if( input_device->input_type == INPUT_DEVICE_LINSYS_SDI )
        input = linsys_sdi_input;
    else
    {
        fprintf( stderr, "Invalid input device \n" );
        return -1;
    }

    if( input_device->input_type == INPUT_URL && !input_device->location )
    {
        fprintf( stderr, "Invalid input location\n" );
        return -1;
    }

    args = malloc( sizeof(*args) );
    if( !args )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }

    args->h = h;
    memcpy( &args->user_opts, input_device, sizeof(*input_device) );
    if( input_device->location )
    {
       args->user_opts.location = malloc( strlen( input_device->location ) + 1 );
       if( !args->user_opts.location)
       {
           fprintf( stderr, "Malloc failed \n" );
           goto fail;
        }

        strcpy( args->user_opts.location, input_device->location );
    }

    if( obe_validate_input_params( input_device ) < 0 )
        goto fail;

    if( pthread_create( &thread, NULL, input.probe_input, (void*)args ) < 0 )
    {
        fprintf( stderr, "Couldn't create probe thread \n" );
        goto fail;
    }

    if( input_device->location )
        printf( "Probing device: \"%s\". ", input_device->location );
    else if( input_device->input_type == INPUT_DEVICE_LINSYS_SDI )
        printf( "Probing device: Linsys card %i. ", input_device->card_idx );
    else
        printf( "Probing device: Decklink card %i. ", input_device->card_idx );

    printf( "Timeout %i seconds \n", probe_time );

    while( i++ < probe_time )
    {
        sleep( 1 );
        fprintf( stderr, "." );

        if( pthread_kill( thread, 0 ) == ESRCH )
            break;
    }

    __pthread_cancel( thread );
    __pthread_join( thread, &ret_ptr );

    cur_devices = h->num_devices;

    if( prev_devices == cur_devices )
    {
        fprintf( stderr, "Could not probe device \n" );
        program = NULL;
        return -1;
    }

    // TODO metadata etc
    program->num_streams = h->devices[h->num_devices-1]->num_input_streams;
    program->streams = calloc( program->num_streams, sizeof(*program->streams) );
    if( !program->streams )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }

    h->devices[h->num_devices-1]->probed_streams = program->streams;

    for( i = 0; i < program->num_streams; i++ )
    {
        stream_in = h->devices[h->num_devices-1]->streams[i];
        stream_out = &program->streams[i];

        stream_out->input_stream_id = stream_in->input_stream_id;
        stream_out->stream_type = stream_in->stream_type;
        stream_out->stream_format = stream_in->stream_format;

        stream_out->bitrate = stream_in->bitrate;

        stream_out->num_frame_data = stream_in->num_frame_data;
        stream_out->frame_data = stream_in->frame_data;

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
    {
        if( args->user_opts.location )
            free( args->user_opts.location );
        free( args );
    }

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

    if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY )
        x264_param_default_preset( param, "veryfast", "zerolatency" );
    else
        x264_param_default( param );

    param->b_deterministic = 0;
    param->b_vfr_input = 0;
    param->b_pic_struct = 1;
    param->b_open_gop = 1;
    param->rc.i_rc_method = X264_RC_ABR;

    param->i_width = stream->width;
    param->i_height = stream->height;

    param->i_fps_num = stream->timebase_den;
    param->i_fps_den = stream->timebase_num;
    param->b_interlaced = stream->interlaced;
    if( param->b_interlaced )
        param->b_tff = stream->tff;

    /* A reasonable default. x264 won't go higher than this parameter irrespective of speedcontrol */
    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
        param->i_frame_reference = 4;

    if( stream->sar_num && stream->sar_den )
    {
        param->vui.i_sar_width  = stream->sar_num;
        param->vui.i_sar_height = stream->sar_den;
    }

    param->vui.i_overscan = 2;

    if( ( param->i_fps_num == 25 || param->i_fps_num == 50 ) && param->i_fps_den == 1 )
    {
        param->vui.i_vidformat = 1; // PAL
        param->vui.i_colorprim = 5; // BT.470-2 bg
        param->vui.i_transfer  = 5; // BT.470-2 bg
        param->vui.i_colmatrix = 5; // BT.470-2 bg
        param->i_keyint_max = param->i_fps_num == 50 ? 48 : 24;
    }
    else if( ( param->i_fps_num == 30000 || param->i_fps_num == 60000 ) && param->i_fps_den == 1001 )
    {
        param->vui.i_vidformat = 2; // NTSC
        param->vui.i_colorprim = 6; // BT.601-6
        param->vui.i_transfer  = 6; // BT.601-6
        param->vui.i_colmatrix = 6; // BT.601-6
        param->i_keyint_max = param->i_fps_num / 1000;
    }
    else
    {
        param->vui.i_vidformat = 5; // undefined
        param->vui.i_colorprim = 2; // undefined
        param->vui.i_transfer  = 2; // undefined
        param->vui.i_colmatrix = 2; // undefined
    }

    /* Change to BT.709 for HD resolutions */
    if( param->i_width >= 1280 && param->i_height >= 720 )
    {
        param->vui.i_colorprim = 1;
        param->vui.i_transfer  = 1;
        param->vui.i_colmatrix = 1;
    }

    x264_param_apply_profile( param, X264_BIT_DEPTH == 10 ? "high10" : "high" );
    param->i_nal_hrd = X264_NAL_HRD_FAKE_VBR;
    param->b_aud = 1;
    param->i_log_level = X264_LOG_INFO;

    //param->rc.f_vbv_buffer_init = 0.1;

    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        param->sc.f_speed = 1.0;
        param->sc.b_alt_timer = 1;
        if( param->i_width >= 1280 && param->i_height >= 720 )
            param->sc.max_preset = 7; /* on the conservative side for HD */
        else
        {
            param->sc.max_preset = 10;
            param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
        }

        param->rc.i_lookahead = param->i_keyint_max;
    }

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
    h->output_streams = malloc( num_streams * sizeof(*h->output_streams) );
    if( !h->output_streams )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }
    memcpy( h->output_streams, output_streams, num_streams * sizeof(*h->output_streams) );

    // TODO sort out VBI

    return 0;
}

int obe_setup_muxer( obe_t *h, obe_mux_opts_t *mux_opts )
{
    // TODO sanity check

    memcpy( &h->mux_opts, mux_opts, sizeof(obe_mux_opts_t) );

    if( mux_opts->service_name )
    {
        h->mux_opts.service_name = malloc( strlen( mux_opts->service_name ) + 1 );
        if( !h->mux_opts.service_name )
        {
           fprintf( stderr, "Malloc failed \n" );
           return -1;
        }

        strcpy( h->mux_opts.service_name, mux_opts->service_name );
    }
    if( mux_opts->provider_name )
    {
        h->mux_opts.provider_name = malloc( strlen( mux_opts->provider_name ) + 1 );
        if( !h->mux_opts.provider_name )
        {
            fprintf( stderr, "Malloc failed \n" );
            return -1;
        }

        strcpy( h->mux_opts.provider_name, mux_opts->provider_name );
    }

    return 0;
}

int obe_setup_output( obe_t *h, obe_output_opts_t *output_opts )
{
    // TODO further sanity checks
    if( output_opts->num_outputs <= 0 )
    {
       fprintf( stderr, "Invalid number of outputs \n" );
       return -1;
    }

    h->outputs = malloc( output_opts->num_outputs * sizeof(*h->outputs) );
    if( !h->outputs )
    {
       fprintf( stderr, "Malloc failed\n" );
       return -1;
    }

    for( int i = 0; i < output_opts->num_outputs; i++ )
    {
        h->outputs[i] = calloc( 1, sizeof(*h->outputs[i]) );
        if( !h->outputs[i] )
        {
           fprintf( stderr, "Malloc failed\n" );
           return -1;
        }
        h->outputs[i]->output_dest.type = output_opts->outputs[i].type;
        if( output_opts->outputs[i].target )
        {
            h->outputs[i]->output_dest.target = malloc( strlen( output_opts->outputs[i].target ) + 1 );
            if( !h->outputs[i]->output_dest.target )
            {
                fprintf( stderr, "Malloc failed\n" );
                return -1;
            }
            strcpy( h->outputs[i]->output_dest.target, output_opts->outputs[i].target );
        }
    }
    h->num_outputs = output_opts->num_outputs;

    return 0;
}

int obe_start( obe_t *h )
{
    obe_int_input_stream_t  *input_stream;
    obe_vid_filter_params_t *vid_filter_params;
    obe_aud_filter_params_t *aud_filter_params;
    obe_vid_enc_params_t *vid_enc_params;
    obe_aud_enc_params_t *aud_enc_params;

    obe_input_func_t  input;
    obe_aud_enc_func_t audio_encoder;
    obe_output_func_t output;

    int num_samples = 0;

    /* TODO: a lot of sanity checks */
    /* TODO: decide upon thread priorities */

    /* Setup mutexes and cond vars */
    pthread_mutex_init( &h->devices[0]->device_mutex, NULL );
    pthread_mutex_init( &h->drop_mutex, NULL );
    obe_init_queue( &h->enc_smoothing_queue );
    obe_init_queue( &h->mux_queue );
    obe_init_queue( &h->mux_smoothing_queue );
    pthread_mutex_init( &h->obe_clock_mutex, NULL );
    pthread_cond_init( &h->obe_clock_cv, NULL );

    if( h->devices[0]->device_type == INPUT_URL )
    {
        //input = lavf_input;
        fprintf( stderr, "URL input is not supported currently \n" );
        goto fail;
    }
#if HAVE_DECKLINK
    else if( h->devices[0]->device_type == INPUT_DEVICE_DECKLINK )
        input = decklink_input;
#endif
    else if( h->devices[0]->device_type == INPUT_DEVICE_LINSYS_SDI )
        input = linsys_sdi_input;
    else
    {
        fprintf( stderr, "Invalid input device \n" );
        goto fail;
    }

    /* Open Output Threads */
    for( int i = 0; i < h->num_outputs; i++ )
    {
        obe_init_queue( &h->outputs[i]->queue );
        output = ip_output;

        if( pthread_create( &h->outputs[i]->output_thread, NULL, output.open_output, (void*)h->outputs[i] ) < 0 )
        {
            fprintf( stderr, "Couldn't create output thread \n" );
            goto fail;
        }
    }

    /* Open Encoder Threads */
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        if( h->output_streams[i].stream_action == STREAM_ENCODE )
        {
            h->encoders[h->num_encoders] = calloc( 1, sizeof(obe_encoder_t) );
            if( !h->encoders[h->num_encoders] )
            {
                fprintf( stderr, "Malloc failed \n" );
                goto fail;
            }
            obe_init_queue( &h->encoders[h->num_encoders]->queue );
            h->encoders[h->num_encoders]->output_stream_id = h->output_streams[i].output_stream_id;

            if( h->output_streams[i].stream_format == VIDEO_AVC )
            {
                x264_param_t *x264_param = &h->output_streams[i].avc_param;
                if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY )
                {
                    /* This doesn't need to be particularly accurate since x264 calculates the correct value internally */
                    x264_param->rc.i_vbv_buffer_size = (double)x264_param->rc.i_vbv_max_bitrate * x264_param->i_fps_den / x264_param->i_fps_num;
                }

                vid_enc_params = calloc( 1, sizeof(*vid_enc_params) );
                if( !vid_enc_params )
                {
                    fprintf( stderr, "Malloc failed \n" );
                    goto fail;
                }
                vid_enc_params->h = h;
                vid_enc_params->encoder = h->encoders[h->num_encoders];
                h->encoders[h->num_encoders]->is_video = 1;

                memcpy( &vid_enc_params->avc_param, &h->output_streams[i].avc_param, sizeof(x264_param_t) );
                if( pthread_create( &h->encoders[h->num_encoders]->encoder_thread, NULL, x264_encoder.start_encoder, (void*)vid_enc_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create encode thread \n" );
                    goto fail;
                }
            }
            else if( h->output_streams[i].stream_format == AUDIO_AC_3 || h->output_streams[i].stream_format == AUDIO_E_AC_3 ||
                     h->output_streams[i].stream_format == AUDIO_AAC  || h->output_streams[i].stream_format == AUDIO_MP2 )
            {
                audio_encoder = h->output_streams[i].stream_format == AUDIO_MP2 ? twolame_encoder : lavc_encoder;
                num_samples = h->output_streams[i].stream_format == AUDIO_MP2 ? MP2_NUM_SAMPLES :
                              h->output_streams[i].stream_format == AUDIO_AAC ? AAC_NUM_SAMPLES : AC3_NUM_SAMPLES;

                aud_enc_params = calloc( 1, sizeof(*aud_enc_params) );
                if( !aud_enc_params )
                {
                    fprintf( stderr, "Malloc failed \n" );
                    goto fail;
                }
                aud_enc_params->h = h;
                aud_enc_params->encoder = h->encoders[h->num_encoders];
                aud_enc_params->stream = &h->output_streams[i];

                input_stream = get_input_stream( h, h->output_streams[i].input_stream_id );
                aud_enc_params->input_sample_format = input_stream->sample_format;
                aud_enc_params->sample_rate = input_stream->sample_rate;
                /* TODO: check the bitrate is allowed by the format */

                h->output_streams[i].sdi_audio_pair = MAX( h->output_streams[i].sdi_audio_pair, 0 );

                /* Choose the optimal number of audio frames per PES
                 * TODO: This should be set after the encoder has told us the frame size */
                if( !h->output_streams[i].ts_opts.frames_per_pes && h->obe_system == OBE_SYSTEM_TYPE_GENERIC &&
                    h->output_streams[i].stream_format != AUDIO_E_AC_3 )
                {
                    int buf_size = h->output_streams[i].stream_format == AUDIO_MP2 || h->output_streams[i].stream_format == AUDIO_AAC ? MISC_AUDIO_BS : AC3_BS_DVB;
                    if( buf_size == AC3_BS_DVB && ( h->mux_opts.ts_type == OBE_TS_TYPE_CABLELABS || h->mux_opts.ts_type == OBE_TS_TYPE_ATSC ) )
                        buf_size = AC3_BS_ATSC;
                    /* AAC does not have exact frame sizes but this should be a good approximation */
                    int single_frame_size = (double)num_samples * 125 * h->output_streams[i].bitrate / input_stream->sample_rate;
                    if( h->output_streams[i].aac_opts.aac_profile == AAC_HE_V1 || h->output_streams[i].aac_opts.aac_profile == AAC_HE_V2 )
                        single_frame_size <<= 1;
                    int frames_per_pes = MAX( buf_size / single_frame_size, 1 );
                    frames_per_pes = MIN( frames_per_pes, 6 );
                    h->output_streams[i].ts_opts.frames_per_pes = aud_enc_params->frames_per_pes = frames_per_pes;
                }
                else
                    h->output_streams[i].ts_opts.frames_per_pes = aud_enc_params->frames_per_pes = 1;

                if( pthread_create( &h->encoders[h->num_encoders]->encoder_thread, NULL, audio_encoder.start_encoder, (void*)aud_enc_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create encode thread \n" );
                    goto fail;
                }
            }

            h->num_encoders++;
        }
    }

    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        /* Open Encoder Smoothing Thread */
        if( pthread_create( &h->enc_smoothing_thread, NULL, enc_smoothing.start_smoothing, (void*)h ) < 0 )
        {
            fprintf( stderr, "Couldn't create encoder smoothing thread \n" );
            goto fail;
        }
    }

    /* Open Mux Smoothing Thread */
    if( pthread_create( &h->mux_smoothing_thread, NULL, mux_smoothing.start_smoothing, (void*)h ) < 0 )
    {
        fprintf( stderr, "Couldn't create mux smoothing thread \n" );
        goto fail;
    }


    /* Open Mux Thread */
    obe_mux_params_t *mux_params = calloc( 1, sizeof(*mux_params) );
    if( !mux_params )
    {
        fprintf( stderr, "Malloc failed \n" );
        goto fail;
    }
    mux_params->h = h;
    mux_params->device = h->devices[0];
    mux_params->num_output_streams = h->num_output_streams;
    mux_params->output_streams = h->output_streams;

    if( pthread_create( &h->mux_thread, NULL, ts_muxer.open_muxer, (void*)mux_params ) < 0 )
    {
        fprintf( stderr, "Couldn't create mux thread \n" );
        goto fail;
    }

    /* Open Filter Thread */
    for( int i = 0; i < h->devices[0]->num_input_streams; i++ )
    {
        input_stream = h->devices[0]->streams[i];
        if( input_stream && ( input_stream->stream_type == STREAM_TYPE_VIDEO || input_stream->stream_type == STREAM_TYPE_AUDIO ) )
        {
            h->filters[h->num_filters] = calloc( 1, sizeof(obe_filter_t) );
            if( !h->filters[h->num_filters] )
                goto fail;

            obe_init_queue( &h->filters[h->num_filters]->queue );

            h->filters[h->num_filters]->num_stream_ids = 1;
            h->filters[h->num_filters]->stream_id_list = malloc( sizeof(*h->filters[h->num_filters]->stream_id_list) );
            if( !h->filters[h->num_filters]->stream_id_list )
            {
                fprintf( stderr, "Malloc failed\n" );
                goto fail;
            }

            h->filters[h->num_filters]->stream_id_list[0] = input_stream->input_stream_id;

            if( input_stream->stream_type == STREAM_TYPE_VIDEO )
            {
                vid_filter_params = calloc( 1, sizeof(*vid_filter_params) );
                if( !vid_filter_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }

                vid_filter_params->h = h;
                vid_filter_params->filter = h->filters[h->num_filters];
                vid_filter_params->input_stream = input_stream;
                vid_filter_params->target_csp = h->output_streams[i].avc_param.i_csp & X264_CSP_MASK;

                if( pthread_create( &h->filters[h->num_filters]->filter_thread, NULL, video_filter.start_filter, vid_filter_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create video filter thread \n" );
                    goto fail;
                }
            }
            else
            {
                aud_filter_params = calloc( 1, sizeof(*aud_filter_params) );
                if( !aud_filter_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }

                aud_filter_params->h = h;
                aud_filter_params->filter = h->filters[h->num_filters];

                if( pthread_create( &h->filters[h->num_filters]->filter_thread, NULL, audio_filter.start_filter, aud_filter_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create filter thread \n" );
                    goto fail;
                }
            }

            h->num_filters++;
        }
    }

    /* Open Input Thread */
    obe_input_params_t *input_params = calloc( 1, sizeof(*input_params) );
    if( !input_params )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto fail;
    }
    input_params->h = h;
    input_params->device = h->devices[0];

    /* TODO: in the future give it only the streams which are necessary */
    input_params->num_output_streams = h->num_output_streams;
    input_params->output_streams = h->output_streams;
    input_params->audio_samples = num_samples;

    if( pthread_create( &h->devices[0]->device_thread, NULL, input.open_input, (void*)input_params ) < 0 )
    {
        fprintf( stderr, "Couldn't create input thread \n" );
        goto fail;
    }

    h->is_active = 1;

    return 0;

fail:

    obe_close( h );

    return -1;
};

void obe_close( obe_t *h )
{
    void *ret_ptr;

    fprintf( stderr, "closing obe \n" );

    /* Cancel input thread */
    for( int i = 0; i < h->num_devices; i++ )
    {
        __pthread_cancel( h->devices[i]->device_thread );
        __pthread_join( h->devices[i]->device_thread, &ret_ptr );
    }

    fprintf( stderr, "input cancelled \n" );

    /* Cancel filter threads */
    for( int i = 0; i < h->num_filters; i++ )
    {
        pthread_mutex_lock( &h->filters[i]->queue.mutex );
        h->filters[i]->cancel_thread = 1;
        pthread_cond_signal( &h->filters[i]->queue.in_cv );
        pthread_mutex_unlock( &h->filters[i]->queue.mutex );
        __pthread_join( h->filters[i]->filter_thread, &ret_ptr );
    }

    fprintf( stderr, "filters cancelled \n" );

    /* Cancel encoder threads */
    for( int i = 0; i < h->num_encoders; i++ )
    {
        pthread_mutex_lock( &h->encoders[i]->queue.mutex );
        h->encoders[i]->cancel_thread = 1;
        pthread_cond_signal( &h->encoders[i]->queue.in_cv );
        pthread_mutex_unlock( &h->encoders[i]->queue.mutex );
        __pthread_join( h->encoders[i]->encoder_thread, &ret_ptr );
    }

    fprintf( stderr, "encoders cancelled \n" );

    /* Cancel encoder smoothing thread */
    if ( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
        h->cancel_enc_smoothing_thread = 1;
        pthread_cond_signal( &h->enc_smoothing_queue.in_cv );
        pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
        /* send a clock tick in case smoothing is waiting for one */
        pthread_mutex_lock( &h->obe_clock_mutex );
        pthread_cond_broadcast( &h->obe_clock_cv );
        pthread_mutex_unlock( &h->obe_clock_mutex );
        if ( h->enc_smoothing_thread )
            __pthread_join( h->enc_smoothing_thread, &ret_ptr );
    }

    fprintf( stderr, "encoder smoothing cancelled \n" );

    /* Cancel mux thread */
    pthread_mutex_lock( &h->mux_queue.mutex );
    h->cancel_mux_thread = 1;
    pthread_cond_signal( &h->mux_queue.in_cv );
    pthread_mutex_unlock( &h->mux_queue.mutex );
    __pthread_join( h->mux_thread, &ret_ptr );

    fprintf( stderr, "mux cancelled \n" );

    /* Cancel mux smoothing thread */
    pthread_mutex_lock( &h->mux_smoothing_queue.mutex );
    h->cancel_mux_smoothing_thread = 1;
    pthread_cond_signal( &h->mux_smoothing_queue.in_cv );
    pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
    __pthread_join( h->mux_smoothing_thread, &ret_ptr );

    fprintf( stderr, "mux smoothing cancelled \n" );

    /* Cancel output threads */
    for( int i = 0; i < h->num_outputs; i++ )
    {
        pthread_mutex_lock( &h->outputs[i]->queue.mutex );
        h->outputs[i]->cancel_thread = 1;
        pthread_cond_signal( &h->outputs[i]->queue.in_cv );
        pthread_mutex_unlock( &h->outputs[i]->queue.mutex );
        /* could be blocking on OS so have to cancel thread too */
        __pthread_cancel( h->outputs[i]->output_thread );
        __pthread_join( h->outputs[i]->output_thread, &ret_ptr );
    }

    fprintf( stderr, "output thread cancelled \n" );

    /* Destroy devices */
    for( int i = 0; i < h->num_devices; i++ )
        destroy_device( h->devices[i] );

    fprintf( stderr, "devices destroyed \n" );

    /* Destroy filters */
    for( int i = 0; i < h->num_filters; i++ )
        destroy_filter( h->filters[i] );

    fprintf( stderr, "filters destroyed \n" );

    /* Destroy encoders */
    for( int i = 0; i < h->num_encoders; i++ )
        destroy_encoder( h->encoders[i] );

    fprintf( stderr, "encoders destroyed \n" );

    destroy_enc_smoothing( &h->enc_smoothing_queue );
    fprintf( stderr, "encoder smoothing destroyed \n" );

    /* Destroy mux */
    destroy_mux( h );

    fprintf( stderr, "mux destroyed \n" );

    destroy_mux_smoothing( &h->mux_smoothing_queue );
    fprintf( stderr, "mux smoothing destroyed \n" );

    /* Destroy output */
    for( int i = 0; i < h->num_outputs; i++ )
        destroy_output( h->outputs[i] );

    free( h->outputs );

    fprintf( stderr, "output destroyed \n" );

    free( h->output_streams );
    /* TODO: free other things */

    /* Destroy lock manager */
    av_lockmgr_register( NULL );

    free( h );
    h = NULL;
}
