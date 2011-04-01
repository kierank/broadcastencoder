/*****************************************************************************
 * common.h: OBE common headers and structures
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

#ifndef OBE_COMMON_H
#define OBE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <syslog.h>
#include "obe.h"

#define MAX_DEVICES 1
#define MAX_STREAMS 40

#define MAX_PROBE_TIME 200

/* Macros */
#define BOOLIFY(x) x = !!x
#define MIN(a,b) ( (a)<(b) ? (a) : (b) )
#define MAX(a,b) ( (a)>(b) ? (a) : (b) )

typedef void *hnd_t;

typedef struct
{
    int stream_id;
    int stream_type;
    int stream_format;

    /** libavformat **/
    int lavf_stream_idx;

    char lang_code[4];

    char *transport_desc_text;
    char *codec_desc_text;

    /* Timebase of transport */
    int transport_timebase_num;
    int transport_timebase_den;

    /* Timebase of codec */
    int timebase_num;
    int timebase_den;

    /* MPEG-TS */
    int pid;
    int ts_stream_id;
    int has_stream_identifier;
    int stream_identifier;
    int audio_type;

    /** Video **/
    int csp;
    int width;
    int height;
    int sar_num;
    int sar_den;
    int interlaced;
    int tff;

    /** Audio **/
    int64_t channel_layout;
    int sample_rate;

    /* Raw Audio */
    int sample_format;

    /* Compressed Audio */
    int bitrate;

    /* AAC */
    int aac_profile_and_level;
    int aac_type;
    int is_latm;

    /** Subtitles **/
    /* DVB */
    int dvb_subtitling_type;
    int composition_page_id;
    int ancillary_page_id;
    int has_dds;

    /** DVB Teletext **/
    int dvb_teletext_type;
    int dvb_teletext_magazine_number;
    int dvb_teletext_page_number;
} obe_int_input_stream_t;

typedef struct
{
    int device_id;
    int device_type;
    char *location;

    /* MPEG-TS */
    int program_num;
    int ts_id;
    int pmt_pid;
    int pcr_pid;

    pthread_mutex_t device_mutex;
    pthread_t device_thread;

    int num_input_streams;
    obe_int_input_stream_t *streams[MAX_STREAMS];

    int num_output_streams;
    obe_output_stream_t *output_streams;

    obe_input_stream_t *probed_streams;
} obe_device_t;

typedef struct
{
    int     csp;       /* colorspace */
    int     width;     /* width of the picture */
    int     height;    /* height of the picture */
    int     planes;    /* number of planes */
    uint8_t *plane[4]; /* pointers for each plane */
    int     stride[4]; /* strides for each plane */
} cli_image_t;

typedef struct
{
    int len;
    int type;
    uint8_t *data;
} obe_user_data_t;

typedef struct
{
    int hours;
    int mins;
    int seconds;
    int frames;
} obe_timecode_t;

typedef struct
{
    int stream_id;
    int64_t pts;

    void (*release_frame)( void* );

    /* Video */
    cli_image_t img;
    void *opaque;

    /* Ancillary / User-data */
    int num_user_data;
    obe_user_data_t *user_data;

    /* Non-video */
    int len;
    uint8_t *data;
    uint8_t *cur_pos;

    int valid_timecode;
    obe_timecode_t timecode;

    /* Audio */
    int num_samples;
    int sample_fmt;
    int channel_map;
    // TODO channel order
} obe_raw_frame_t;

typedef struct
{
    int stream_id;
    int is_ready;

    pthread_t encoder_thread;
    pthread_mutex_t encoder_mutex;
    pthread_cond_t  encoder_cv;

    hnd_t encoder_params;

    int num_raw_frames;
    obe_raw_frame_t **frames;
} obe_encoder_t;

typedef struct
{
    int stream_id;
    int is_video;

    int64_t pts;

    /* Video Only */
    int64_t real_pts;
    int64_t real_dts;
    int random_access;
    int priority;

    int len;
    uint8_t *data;
} obe_coded_frame_t;

typedef struct
{
    int bytes_left;
    uint8_t *data;
    uint8_t *cur_pos;

    /* MPEG-TS */
    int64_t *pcr_list;
    int64_t *pcr_list_pos;
} obe_muxed_data_t;

struct obe_t
{
    int is_active;

    /* Devices */
    pthread_mutex_t device_list_mutex;
    int num_devices;
    obe_device_t *devices[MAX_DEVICES];
    int cur_stream_id;

    /* Streams */
    int num_output_streams;
    obe_output_stream_t *output_streams;

    /* Mux */
    pthread_t mux_thread;
    obe_mux_opts_t mux_opts;

    /* Output */
    pthread_t output_thread;
    obe_output_opts_t output_opts;

    /* Filtering TODO */

    /* Input or Postfiltered frames for encoding */
    int num_encoders;
    obe_encoder_t *encoders[MAX_STREAMS];

    /* Encoded frames for muxing */
    pthread_mutex_t mux_mutex;
    pthread_cond_t  mux_cv;
    int num_coded_frames;
    obe_coded_frame_t **coded_frames;

    /* Muxed frames for transmission */
    pthread_mutex_t output_mutex;
    pthread_cond_t  output_cv;
    int num_muxed_data;
    obe_muxed_data_t **muxed_data;

    /* Statistics and Monitoring */
};

obe_device_t *new_device( void );
void destroy_device( obe_device_t *device );
obe_raw_frame_t *new_raw_frame( void );
void destroy_raw_frame( obe_raw_frame_t *raw_frame );
int remove_early_frames( obe_t *h, int64_t pts );
obe_coded_frame_t *new_coded_frame( int stream_id, int len );
void destroy_coded_frame( obe_coded_frame_t *coded_frame );
obe_muxed_data_t *new_muxed_data( int len );
void destroy_muxed_data( obe_muxed_data_t *muxed_data );

int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame );
int remove_frame_from_encode_queue( obe_encoder_t *encoder );
int add_to_mux_queue( obe_t *h, obe_coded_frame_t *coded_frame );
int remove_from_mux_queue( obe_t *h, obe_coded_frame_t *coded_frame );
int add_to_output_queue( obe_t *h, obe_muxed_data_t *muxed_data );
int remove_from_output_queue( obe_t *h );

obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id );
obe_encoder_t *get_encoder( obe_t *h, int stream_id );

#endif
