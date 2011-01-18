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

#include <stdint.h>
#include <pthread.h>
#include "obe.h"

#define MAX_DEVICES 10
#define MAX_STREAMS 100

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
    cli_image_t img;

    // subtitles etc
}obe_video_frame_t;

typedef struct
{
    int stream_id;
    int stream_type;
    int stream_format;

    /** Video **/
    int bit_depth;
    int width;
    int height;

    /** Audio **/
    int channel_map;
    int sample_rate;

    char *transport_desc_text;
    char *codec_desc_text;

    /* Raw Audio */
    int sample_format;

    /* Compressed Audio */
     int bitrate;

    /** Subtitles **/

}obe_int_input_stream_t;

typedef struct
{
    int device_type;
    char *location;

    int num_streams;
    obe_int_input_stream_t streams[MAX_STREAMS];
}obe_device_t;

struct obe_t
{
    pthread_t main_thread;

    int num_devices;
    obe_device_t devices[MAX_DEVICES];

    int cur_stream_id;
};



#endif
