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

#include "config.h"

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uref.h>
#include <upipe/uref_pic.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/common.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/fifo.h>

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
#include <sys/time.h>
#include <time.h>
#include "obe.h"

#define MAX_STREAMS 40
#define MAX_CHANNELS 16

#define MAX_PROBE_TIME 20

#define OBE_CLOCK 27000000LL

#define MAX_SAMPLES 3000

/* Macros */
#define BOOLIFY(x) x = !!x
#define MIN(a,b) ( (a)<(b) ? (a) : (b) )
#define MAX(a,b) ( (a)>(b) ? (a) : (b) )

#define IS_SD(x) ((x) == INPUT_VIDEO_FORMAT_PAL || (x) == INPUT_VIDEO_FORMAT_NTSC)
#define IS_INTERLACED(x) (IS_SD(x) || (x) == INPUT_VIDEO_FORMAT_1080I_50 || (x) == INPUT_VIDEO_FORMAT_1080I_5994 )
#define IS_PROGRESSIVE(x) (!IS_INTERLACED(x))
#define IS_PAL(x) ((x) == INPUT_VIDEO_FORMAT_PAL || (x) == INPUT_VIDEO_FORMAT_1080I_50 || (x) == INPUT_VIDEO_FORMAT_1080P_25)

/* Audio formats */
#define AC3_NUM_SAMPLES 1536
#define MP2_NUM_SAMPLES 1152
#define AAC_NUM_SAMPLES 1024
#define OPUS_NUM_SAMPLES 960

/* T-STD buffer sizes */
#define AC3_BS_ATSC     2592
#define AC3_BS_DVB      5696
#define MISC_AUDIO_BS   3584

/* Network output */
#define TS_PACKETS_SIZE 1316

/* Audio sample patterns */
#define MAX_AUDIO_SAMPLE_PATTERN 5

/* NTSC */
#define NTSC_FIRST_CODED_LINE 23


/** @This declares two functions dealing with substructures included into a
 * larger structure.
 *
 * @param STRUCTURE name of the larger structure
 * @param SUBSTRUCT name of the smaller substructure
 * @param SUBNAME name to use for the functions
 * (STRUCTURE##_{to,from}_##SUBNAME)
 * @param SUB name of the @tt{struct SUBSTRUCT} field of @tt{struct STRUCTURE}
 */
#define UBASE_FROM_TO_TYPEDEF(STRUCTURE, SUBSTRUCT, SUBNAME, SUB)           \
/** @internal @This returns a pointer to SUBNAME.                           \
 *                                                                          \
 * @param STRUCTURE pointer to struct STRUCTURE                             \
 * @return pointer to struct SUBSTRUCT                                      \
 */                                                                         \
static UBASE_UNUSED inline SUBSTRUCT *                                      \
    STRUCTURE##_to_##SUBNAME(STRUCTURE *s)                                  \
{                                                                           \
    return &s->SUB;                                                         \
}                                                                           \
/** @internal @This returns a pointer to SUBNAME.                           \
 *                                                                          \
 * @param sub pointer to struct SUBSTRUCT                                   \
 * @return pointer to struct STRUCTURE                                      \
 */                                                                         \
static UBASE_UNUSED inline  STRUCTURE *                                     \
    STRUCTURE##_from_##SUBNAME(SUBSTRUCT *sub)                              \
{                                                                           \
    return container_of(sub, STRUCTURE, SUB);                               \
}

static inline int obe_clip3( int v, int i_min, int i_max )
{
    return ( (v < i_min) ? i_min : (v > i_max) ? i_max : v );
}

typedef void *hnd_t;

typedef struct
{
    int dvb_subtitling_type;
    int composition_page_id;
    int ancillary_page_id;
} obe_dvb_sub_info_t;

typedef struct
{
    int input_stream_id;
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
    int video_format;
    int csp;
    int width;
    int height;
    int sar_num;
    int sar_den;
    int interlaced;
    int tff;

    /* Per-frame Data */
    int num_frame_data;
    obe_frame_data_t *frame_data;

    /** Audio **/
    uint64_t channel_layout;
    int num_channels; /* set if channel layout is 0 */
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
    int has_dds;
    int dvb_subtitling_type;
    int composition_page_id;
    int ancillary_page_id;

    /** DVB Teletext **/
    int dvb_teletext_type;
    int dvb_teletext_magazine_number;
    int dvb_teletext_page_number;

    /* VBI */
    int vbi_ntsc;

    /** Misc **/
    int source;
} obe_int_input_stream_t;

typedef struct
{
    int device_id;
    int device_type;

    obe_input_t user_opts;

    /* MPEG-TS */
    int program_num;
    int ts_id;
    int pmt_pid;
    int pcr_pid;

    int stop;
    pthread_mutex_t device_mutex;
    pthread_t device_thread;

    int active;

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
    int     format;    /* image format */
    int     first_line; /* first line of image (SD from SDI only) */
} obe_image_t;

typedef struct
{
     int type;  /* For VBI (including VANC VBI) during input this uses stream_formats_e */
     int source;
     int num_lines;
     int lines[100];
     int location;
} obe_int_frame_data_t;

typedef struct
{
    uint8_t *audio_data[MAX_CHANNELS];
    int      linesize;
    uint64_t channel_layout; /* If this is zero then num_channels is set */
    int      num_channels;
    int      num_samples;
    int      sample_fmt;
} obe_audio_frame_t;

enum user_data_types_e
{
    /* Encapsulated frame data formats */
    USER_DATA_AVC_REGISTERED_ITU_T35 = 4,
    USER_DATA_AVC_UNREGISTERED,

    /* Encapsulated stream data formats */
    USER_DATA_DVB_VBI                = 32, /* VBI packets */

    /* Raw data formats */
    USER_DATA_CEA_608                = 64, /* Raw CEA-608 data 4 bytes, 2 bytes per field */
    USER_DATA_CEA_708_CDP,                 /* CEA-708 Caption Distribution Packet */
    USER_DATA_AFD,                         /* AFD word 1 from SMPTE 2016-3 */
    USER_DATA_BAR_DATA,                    /* Bar Data 5 words from SMPTE 2016-3 */
    USER_DATA_WSS,                         /* 3 bits of WSS to be converted to AFD */
};

enum user_data_location_e
{
    USER_DATA_LOCATION_FRAME,      /* e.g. AFD, Captions */
    USER_DATA_LOCATION_DVB_STREAM, /* e.g. DVB-VBI, DVB-TTX */
    /* TODO: various other minor locations */
};

typedef struct
{
    int service;
    int location;
} obe_non_display_data_location_t;

const static obe_non_display_data_location_t non_display_data_locations[] =
{
    { MISC_TELETEXT,    USER_DATA_LOCATION_DVB_STREAM },
    { MISC_TELETEXT_INVERTED, USER_DATA_LOCATION_DVB_STREAM },
    { MISC_WSS,         USER_DATA_LOCATION_DVB_STREAM },
    { MISC_VPS,         USER_DATA_LOCATION_DVB_STREAM },
    { CAPTIONS_CEA_608, USER_DATA_LOCATION_FRAME },
    { CAPTIONS_CEA_708, USER_DATA_LOCATION_FRAME },
    { MISC_AFD,         USER_DATA_LOCATION_FRAME },
    { MISC_BAR_DATA,    USER_DATA_LOCATION_FRAME },
    { MISC_PAN_SCAN,    USER_DATA_LOCATION_FRAME },
    { VBI_AMOL_48,      USER_DATA_LOCATION_DVB_STREAM },
    { VBI_AMOL_96,      USER_DATA_LOCATION_DVB_STREAM },
    { VBI_NABTS,        USER_DATA_LOCATION_DVB_STREAM },
    { VBI_TVG2X,        USER_DATA_LOCATION_DVB_STREAM },
    { VBI_CP,           USER_DATA_LOCATION_DVB_STREAM },
    /* Does VITC go in the codec or in the VBI? */
    { -1, -1 },
};

typedef struct
{
    int obe_name;
    int timebase_num;
    int timebase_den;
    int width;
    int height;
    int interlaced;
} obe_video_config_t;

const static obe_video_config_t video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_AUTODETECT,                 1,    25,    720, 576,   1 }, /* Set it up as PAL arbitrarily */
    { INPUT_VIDEO_FORMAT_PAL,                        1,    25,    720, 576,   1 },
    { INPUT_VIDEO_FORMAT_NTSC,                       1001, 30000, 720, 480,   1 },
    { INPUT_VIDEO_FORMAT_720P_50,                    1,    50,    1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_720P_5994,                  1001, 60000, 1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_720P_60,                    1,    60,    1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_1080I_50,                   1,    25,    1920, 1080, 1 },
    { INPUT_VIDEO_FORMAT_1080I_5994,                 1001, 30000, 1920, 1080, 1 },
    { INPUT_VIDEO_FORMAT_1080P_2398,                 1001, 24000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_24,                   1,    24,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_25,                   1,    25,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_2997,                 1001, 30000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_50,                   1,    50,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_5994,                 1001, 60000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_60,                   1,    60,    1920, 1080, 0 },
    { -1, -1, -1, -1, -1, -1 },
};

typedef struct
{
    int format;
    int downscale;
} obe_downscale_t;

const static obe_downscale_t downscale_tab[] =
{
    { INPUT_VIDEO_FORMAT_1080I_50,   INPUT_VIDEO_FORMAT_PAL  },
    { INPUT_VIDEO_FORMAT_1080I_5994, INPUT_VIDEO_FORMAT_NTSC },
    { -1, -1 },
};

typedef struct
{
    int format;
    int pattern[MAX_AUDIO_SAMPLE_PATTERN];
    int max;
    int mod;
} obe_audio_sample_pattern_t;

const static obe_audio_sample_pattern_t audio_sample_patterns[] =
{
    { INPUT_VIDEO_FORMAT_PAL,        { 1920 }, 1920, 1 },
    { INPUT_VIDEO_FORMAT_NTSC,       { 1602, 1601, 1602, 1601, 1602 }, 1602, 5 },
    { INPUT_VIDEO_FORMAT_720P_50,    {  960 },  960, 1 },
    { INPUT_VIDEO_FORMAT_720P_5994,  {  801,  800,  801,  801,  801 },  801, 5 },
    { INPUT_VIDEO_FORMAT_720P_60,    {  800 },  800, 1 },
    { INPUT_VIDEO_FORMAT_1080I_50,   { 1920 }, 1920, 1 },
    { INPUT_VIDEO_FORMAT_1080I_5994, { 1602, 1601, 1602, 1601, 1602 }, 1602, 5 },
    { INPUT_VIDEO_FORMAT_1080P_2398, { 2002 }, 2002, 1 },
    { INPUT_VIDEO_FORMAT_1080P_24,   { 2000 }, 2000, 1 },
    { INPUT_VIDEO_FORMAT_1080P_25,   { 1920 }, 1920, 1 },
    { INPUT_VIDEO_FORMAT_1080P_2997, { 1602, 1601, 1602, 1601, 1602 }, 1602, 5 },
    { INPUT_VIDEO_FORMAT_1080P_50,   {  960 },  960, 1 },
    { INPUT_VIDEO_FORMAT_1080P_5994, {  801,  800,  801,  801,  801 },  801, 5 },
    { INPUT_VIDEO_FORMAT_1080P_60,   {  800 }, 800, 1 },
    { -1 },
};

typedef struct
{
    int type;
    int source;
    int field; /* for single line of CEA-608 data, 0 if both lines, otherwise field number */
    int len;
    uint8_t *data;
} obe_user_data_t;

typedef struct
{
    int hours;
    int mins;
    int seconds;
    int frames;
    int drop_frame;
} obe_timecode_t;

typedef struct
{
    struct uchain ulist;

    pthread_mutex_t mutex;
    pthread_cond_t  in_cv;
    pthread_cond_t  out_cv;
} obe_queue_t;

typedef struct
{
    struct uchain uchain;
    
    int input_stream_id;
    int64_t pts;

    AVBufferRef *buf_ref[AV_NUM_DATA_POINTERS];
    struct uref *uref;

    void (*release_data)( void* );
    void (*release_frame)( void* );

    /* Video */
    /* Some devices output visible and VBI/VANC data together. In order
     * to avoid memcpying raw frames, we create two image structures.
     * The first one points to the allocated memory and the second points to the visible frame.
     * For most devices these are the same. */
    obe_image_t alloc_img;
    obe_image_t img;
    int sar_width;
    int sar_height;
    int sar_guess; /* This is set if the SAR cannot be determined from any WSS/AFD that might exist in the stream */
    int64_t arrival_time;

    /* Ancillary / User-data */
    int num_user_data;
    obe_user_data_t *user_data;

    /* Audio */
    obe_audio_frame_t audio_frame;
    int64_t video_pts; /* PTS of the associate video frame (for SMPTE 302M!) */
    int64_t video_duration; /* workaround NTSC issues */
    // TODO channel order
    // TODO audio metadata

    int valid_timecode;
    obe_timecode_t timecode;

    int reset_obe;
} obe_raw_frame_t;

UBASE_FROM_TO_TYPEDEF(obe_raw_frame_t, struct uchain, uchain, uchain)

typedef struct
{
    int num_stream_ids;
    int *stream_id_list;

    pthread_t filter_thread;
    obe_queue_t queue;
    int cancel_thread;

} obe_filter_t;

typedef struct
{
    int output_stream_id;
    int is_ready;
    int is_video;

    pthread_t encoder_thread;
    obe_queue_t queue;
    int cancel_thread;

    int params_update;

    /* HE-AAC and E-AC3 */
    int num_samples;
} obe_encoder_t;

typedef struct
{
    int output_stream_id;
    int total_size;

    int64_t pts;

    AVFifoBuffer *in_fifo;
    AVFifoBuffer *out_fifo;
    int num_in_frames;
    int num_out_frames;

    int bit_depth;
} obe_passthrough_t;

typedef struct
{
    /* Output */
    pthread_t output_thread;
    int cancel_thread;
    obe_output_dest_t output_dest;

    /* Muxed frame queue for transmission */
    obe_queue_t queue;
} obe_output_t;

typedef struct
{
    struct uchain uchain;
    
    int output_stream_id;
    int is_video;

    int64_t pts;

    /* Video Only */
    int64_t cpb_initial_arrival_time;
    int64_t cpb_final_arrival_time;
    int64_t real_dts;
    int64_t real_pts;
    int random_access;
    int priority;
    int64_t arrival_time;

    /* 302M audio */
    int64_t duration;

    int len;
    uint8_t *data;
} obe_coded_frame_t;

UBASE_FROM_TO_TYPEDEF(obe_coded_frame_t, struct uchain, uchain, uchain)

typedef struct
{
    struct uchain uchain;

    int len;
    uint8_t *data;

    /* MPEG-TS */
    int64_t *pcr_list;
} obe_muxed_data_t;

UBASE_FROM_TO_TYPEDEF(obe_muxed_data_t, struct uchain, uchain, uchain)

typedef struct
{
    struct uchain uchain;

    AVBufferRef *self_buf_ref;
    AVBufferRef *data_buf_ref;
} obe_buf_ref_t;

UBASE_FROM_TO_TYPEDEF(obe_buf_ref_t, struct uchain, uchain, uchain)

struct obe_t
{
    int is_active;
    int64_t start_time;
    int obe_system;
    int filter_bit_depth;

    /* OBE recovered clock */
    pthread_mutex_t obe_clock_mutex;
    pthread_cond_t  obe_clock_cv;
    int64_t         obe_clock_last_pts; /* from sdi clock */
    int64_t         obe_clock_last_wallclock; /* from cpu clock */

    /* Devices */
    obe_device_t device;

    /* Frame drop flags
     * TODO: make this work for multiple inputs and outputs */
    pthread_mutex_t drop_mutex;
    int encoder_drop;
    int mux_drop;

    /* Streams */
    int num_output_streams;
    obe_output_stream_t *output_streams;

    /** Individual Threads */
    /* Smoothing (video) */
    pthread_t enc_smoothing_thread;
    int cancel_enc_smoothing_thread;

    /* Mux */
    int mux_params_update;
    pthread_t mux_thread;
    int cancel_mux_thread;
    obe_mux_opts_t mux_opts;

    /* Smoothing (video) */
    pthread_t mux_smoothing_thread;
    int cancel_mux_smoothing_thread;

    /* Filtering */
    int num_filters;
    obe_filter_t *filters[MAX_STREAMS];

    /* Passthrough */
    int num_passthrough;
    obe_passthrough_t *passthrough[MAX_STREAMS];

    /** Multiple Threads **/
    /* Input or Postfiltered frames for encoding */
    int num_encoders;
    obe_encoder_t *encoders[MAX_STREAMS];

    /* Output data */
    int num_outputs;
    obe_output_t **outputs;

    /* Encoded frames in smoothing buffer */
    obe_queue_t     enc_smoothing_queue;

    int             enc_smoothing_buffer_complete;
    int64_t         enc_smoothing_last_exit_time;

    /* Encoded frame queue for muxing */
    obe_queue_t mux_queue;

    /* Muxed frames in smoothing buffer */
    obe_queue_t mux_smoothing_queue;

    /* Statistics and Monitoring */


};

typedef struct
{
    void* (*start_smoothing)( void *ptr );
} obe_smoothing_func_t;

extern const obe_smoothing_func_t enc_smoothing;
extern const obe_smoothing_func_t mux_smoothing;

int64_t obe_mdate( void );

obe_device_t *new_device( void );
void destroy_device( obe_device_t *device );
obe_raw_frame_t *new_raw_frame( void );
void destroy_raw_frame( obe_raw_frame_t *raw_frame );
obe_coded_frame_t *new_coded_frame( int stream_id, int len );
void destroy_coded_frame( obe_coded_frame_t *coded_frame );
void obe_release_video_data( void *ptr );
void obe_release_bufref( void *ptr );
void obe_release_video_uref( void *ptr );
void obe_release_audio_data( void *ptr );
void obe_release_frame( void *ptr );

obe_muxed_data_t *new_muxed_data( int len );
void destroy_muxed_data( obe_muxed_data_t *muxed_data );

void add_device( obe_t *h, obe_device_t *device );

int add_to_queue( obe_queue_t *queue, struct uchain *item );
int remove_from_queue( obe_queue_t *queue );
int remove_item_from_queue( obe_queue_t *queue, struct uchain *item );

int add_to_filter_queue( obe_t *h, obe_raw_frame_t *raw_frame );
int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame, int output_stream_id );
int remove_early_frames( obe_t *h, int64_t pts );
int add_to_output_queue( obe_t *h, obe_muxed_data_t *muxed_data );
int remove_from_output_queue( obe_t *h );

obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id );
obe_passthrough_t *get_passthrough( obe_t *h, int stream_id );
obe_encoder_t *get_encoder( obe_t *h, int stream_id );
obe_output_stream_t *get_output_stream( obe_t *h, int stream_id );
obe_output_stream_t *get_output_stream_by_format( obe_t *h, int format );
const obe_audio_sample_pattern_t *get_sample_pattern( int video_format );

int64_t get_wallclock_in_mpeg_ticks( void );
void sleep_mpeg_ticks( int64_t i_delay );
void obe_clock_tick( obe_t *h, int64_t value );
int64_t get_input_clock_in_mpeg_ticks( obe_t *h );
void sleep_input_clock( obe_t *h, int64_t i_delay );

int get_non_display_location( int type );

#endif
