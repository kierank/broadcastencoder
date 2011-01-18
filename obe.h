/*****************************************************************************
 * obe.h : Open Broadcast Encoder API
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

#ifndef OBE_H
#define OBE_H

#include <stdint.h>
#include <x264.h>
#include <libavcore/avcore.h>

#define OBE_VERSION_MAJOR 0
#define OBE_VERSION_MINOR 1

/* Opaque Handle */
typedef struct obe_t obe_t;

enum input_type_e
{
    INPUT_URL,
    INPUT_DEVICE_SDI,
    INPUT_DEVICE_ASI,
};

/**** Initialisation Function ****/
obe_t *obe_setup( void );

typedef struct
{
    int input_type;
    char *location;

    char *name;
    char *provider_name;

} obe_input_t;

/**** Stream Formats ****/
enum stream_type_e
{
    STREAM_TYPE_VIDEO,
    STREAM_TYPE_AUDIO,
    STREAM_TYPE_SUBTITLES,
    STREAM_TYPE_MISC,
};

enum video_formats_e
{
    VIDEO_AVC,
    VIDEO_MPEG2,
};

enum audio_formats_e
{
    AUDIO_PCM,
    AUDIO_MP2,    /* MPEG-1 Layer II */
    AUDIO_AC_3,   /* ATSC A/52B / AC-3 */
    AUDIO_E_AC_3, /* ATSC A/52B Annex E / Enhanced AC-3 */
    AUDIO_E_DIST, /* E Distribution Audio */
    AUDIO_AAC,
    AUDIO_SMPTE_302M,
};

enum subtitle_formats_e
{
    SUBTITLES_DVB,
    SUBTITLES_EIA_608,
    SUBTITLES_CEA_708,
};

enum misc_formats_e
{
    MISC_AFD,     /* Active Format Description */
    MISC_TELETEXT,
    MISC_VANC,    /* Vertical Ancillary */
};

typedef struct
{
    int stream_id;
    int stream_type;
    int stream_format;

    char lang_code[4];

    char *transport_desc_text;
    char *codec_desc_text;

    /** Video **/
    int bit_depth;
    int width;
    int height;

    /** Audio **/
    int channel_map;
    int sample_rate;

    /* Raw Audio */
    int sample_format;

    /* Compressed Audio */
    int bitrate;

    /** Subtitles **/
} obe_input_stream_t;

int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_stream_t *streams, int *num_streams );

enum stream_action_e
{
    STREAM_PASSTHROUGH,
    STREAM_ENCODE,
};

/**** AVC Encoding ****/
int obe_populate_encoder_params( obe_t *h, int input_stream_id, x264_param_t *param ); 

int obe_setup_avc_encoding( obe_t *h, x264_param_t *param );

/**** 3DTV ****/
/* Arrangements - Frame Packing */
enum frame_packing_arrangement_e
{
    FRAME_PACKING_CHECKERBOARD,
    FRAME_PACKING_COLUMN,
    FRAME_PACKING_ROW,
    FRAME_PACKING_SIDE_BY_SIDE,
    FRAME_PACKING_TOP_BOTTOM,
    FRAME_PACKING_TEMPORAL,
};

/**** MPEG-1 Layer II Encoding ****/

/**** AC-3 Encoding ****/

/**** AAC Encoding ****/

/**** Transport Stream ****/

/** DVB **/

/** ATSC **/



int obe_start( obe_t *h );
int obe_stop( obe_t *h );

void obe_close( obe_t *h );

#endif
