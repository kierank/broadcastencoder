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
    INPUT_DEVICE_SDI_V4L2,
    INPUT_DEVICE_ASI,
};

/**** Initialisation Function ****/
obe_t *obe_setup( void );

typedef struct
{
    int input_type;
    char *location;
} obe_input_t;

/**** Stream Formats ****/
enum stream_type_e
{
    STREAM_TYPE_VIDEO,
    STREAM_TYPE_AUDIO,
    STREAM_TYPE_SUBTITLE,
    STREAM_TYPE_MISC,
};

enum stream_formats_e
{
    VIDEO_UNCOMPRESSED,
    VIDEO_AVC,
    VIDEO_MPEG2,

    AUDIO_PCM,
    AUDIO_MP2,    /* MPEG-1 Layer II */
    AUDIO_AC_3,   /* ATSC A/52B / AC-3 */
    AUDIO_E_AC_3, /* ATSC A/52B Annex E / Enhanced AC-3 */
    AUDIO_E_DIST, /* E Distribution Audio */
    AUDIO_AAC,

    SUBTITLES_DVB,
    SUBTITLES_EIA_608,
    SUBTITLES_CEA_708,

    MISC_TELETEXT,
    MISC_VANC    /* Vertical Ancillary */
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
    int width;
    int height;
    int csp;
    int interlaced;

    int subtitles_type;
    int has_afd;

    /** Audio **/
    int64_t channel_layout;
    int sample_rate;

    /* Raw Audio */
    int sample_format;

    /* Compressed Audio */
    int bitrate;

    /** Subtitles **/
    /* Has display definition segment (i.e HD subtitling) */
    int dvb_has_dds;
}obe_input_stream_t;

typedef struct
{
    char *name;
    char *provider_name;

    int num_streams;
    obe_input_stream_t *streams;
}obe_input_program_t;

/* Only one program is returned */
int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_program_t *program );

enum stream_action_e
{
    STREAM_PASSTHROUGH,
    STREAM_ENCODE,
    STREAM_LATM_TO_ADTS,
};

/**** AVC Encoding ****/
/* Use this function to let OBE guess the encoding profile.
 * You can use the functions in the x264 API for tweaking or edit directly.
 * Be aware that some parameters will affect speed of encoding and hardware support
 */
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

typedef struct
{
    int he_aac;
    int bitrate;
    int capped_vbr;

    int latm_output;
} obe_aac_opts_t;

/**** Transport Stream ****/

/** DVB **/

/** ATSC **/



int obe_start( obe_t *h );
int obe_stop( obe_t *h );

void obe_close( obe_t *h );

#endif
