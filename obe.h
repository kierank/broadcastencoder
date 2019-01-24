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

#include <inttypes.h>
#include <libavutil/audioconvert.h>
#include <x264.h>

#define OBE_VERSION_MAJOR 0
#define OBE_VERSION_MINOR 1

/* Opaque Handle */
typedef struct obe_t obe_t;

/**** Initialisation Function ****/
obe_t *obe_setup( void );

/**** OBE configuration function */
enum obe_system_type_e
{
    OBE_SYSTEM_TYPE_GENERIC,
    OBE_SYSTEM_TYPE_LOWEST_LATENCY,
    OBE_SYSTEM_TYPE_LOW_LATENCY,
};

int obe_set_config( obe_t *h, int system_type );

enum input_video_connection_e
{
    INPUT_VIDEO_CONNECTION_SDI,
    INPUT_VIDEO_CONNECTION_HDMI,
    INPUT_VIDEO_CONNECTION_OPTICAL_SDI,
    INPUT_VIDEO_CONNECTION_COMPONENT,
    INPUT_VIDEO_CONNECTION_COMPOSITE,
    INPUT_VIDEO_CONNECTION_S_VIDEO,
};

enum input_audio_connection_e
{
    INPUT_AUDIO_EMBEDDED,
    INPUT_AUDIO_AES_EBU,
    INPUT_AUDIO_ANALOGUE,
};

enum input_video_format_e
{
    /* SD */
    INPUT_VIDEO_FORMAT_PAL,
    INPUT_VIDEO_FORMAT_NTSC,

    /* 720p HD */
    INPUT_VIDEO_FORMAT_720P_50,
    INPUT_VIDEO_FORMAT_720P_5994,
    INPUT_VIDEO_FORMAT_720P_60 , /* NB: actually 60.00Hz */

    /* 1080i/p HD */
    INPUT_VIDEO_FORMAT_1080I_50,
    INPUT_VIDEO_FORMAT_1080I_5994,
    INPUT_VIDEO_FORMAT_1080I_60, /* NB: actually 60.00Hz */

    INPUT_VIDEO_FORMAT_1080P_2398,
    INPUT_VIDEO_FORMAT_1080P_24,
    INPUT_VIDEO_FORMAT_1080P_25,
    INPUT_VIDEO_FORMAT_1080P_2997,
    INPUT_VIDEO_FORMAT_1080P_30, /* NB: actually 30.00Hz */
    INPUT_VIDEO_FORMAT_1080P_50,
    INPUT_VIDEO_FORMAT_1080P_5994,
    INPUT_VIDEO_FORMAT_1080P_60, /* NB: actually 60.00Hz */
};

enum input_type_e
{
    INPUT_URL,
    INPUT_DEVICE_DECKLINK,
    INPUT_DEVICE_LINSYS_SDI,
//    INPUT_DEVICE_V4L2,
//    INPUT_DEVICE_ASI,
};

enum tc_source_e
{
    TC_SOURCE_NONE,
    TC_SOURCE_RP188,
    TC_SOURCE_VITC,
};

typedef struct
{
    int input_type;
    char *location;

    int card_idx;

    int video_format;
    int video_connection;
    int audio_connection;
    int tc_source;
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
    /* Separate Streams */
    VIDEO_UNCOMPRESSED,
    VIDEO_AVC,
    VIDEO_MPEG2,

    AUDIO_PCM,
    AUDIO_MP2,    /* MPEG-1 Layer II */
    AUDIO_AC_3,   /* ATSC A/52B / AC-3 */
    AUDIO_E_AC_3, /* ATSC A/52B Annex E / Enhanced AC-3 */
//    AUDIO_E_DIST, /* E Distribution Audio */
    AUDIO_AAC,

    SUBTITLES_DVB,
    MISC_TELETEXT,
    MISC_TELETEXT_INVERTED,
    MISC_WSS,
    MISC_VPS,

    /* Per-frame Streams/Data */
    CAPTIONS_CEA_608,
    CAPTIONS_CEA_708,
    MISC_AFD,
    MISC_BAR_DATA,
    MISC_PAN_SCAN,

    /* VBI data services */
    VBI_RAW,         /* location flag */
    VBI_AMOL_48,
    VBI_AMOL_96,
    VBI_NABTS,
    VBI_TVG2X,
    VBI_CP,
    VBI_VITC,
    VBI_VIDEO_INDEX, /* location flag */

    /* Vertical Ancillary (location flags) */
    VANC_GENERIC,
    VANC_DVB_SCTE_VBI,
    VANC_OP47_SDP,
    VANC_OP47_MULTI_PACKET,
    VANC_ATC,
    VANC_DTV_PROGRAM_DESCRIPTION,
    VANC_DTV_DATA_BROADCAST,
    VANC_SMPTE_VBI,
    VANC_SCTE_104,
};

enum mp2_mode_e
{
    MP2_MODE_AUTO,
    MP2_MODE_STEREO,
    MP2_MODE_JOINT_STEREO,
    MP2_MODE_DUAL_CHANNEL,
};

enum mono_channel_e
{
    MONO_CHANNEL_LEFT,
    MONO_CHANNEL_RIGHT,
};

typedef struct
{
     int type;
     int source;
     int num_lines;
     int lines[100]; /* Lines are in SMPTE notation */
} obe_frame_data_t;

typedef struct
{
    int input_stream_id;
    int stream_type;
    int stream_format;

    char lang_code[4];

    char *transport_desc_text;
    char *codec_desc_text;

    /** Video **/
    int csp;
    int width;
    int height;
    int sar_num;
    int sar_den;
    int interlaced;
    int timebase_num;
    int timebase_den;

    /*  Video Streams: Per-frame data (e.g. Captions/AFD)
     *  VBI: Components of DVB-VBI packet */
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
    int aac_is_latm; /* LATM is sometimes known as MPEG-4 Encapsulation */

    /** Subtitles **/
    int dvb_has_dds; /* Has display definition segment (i.e HD subtitling) */

    /** Misc **/
    int source; /* e.g. VBI/VANC */
}obe_input_stream_t;

typedef struct
{
    char *name;
    char *provider_name;

    int num_streams;
    obe_input_stream_t *streams;
} obe_input_program_t;

/* Only one program is returned */
int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_program_t *program );

enum stream_action_e
{
    STREAM_PASSTHROUGH,
    STREAM_ENCODE,
};

enum dvb_teletext_type_e
{
    DVB_TTX_TYPE_INITIAL = 0x01,
    DVB_TTX_TYPE_SUB,
    DVB_TTX_TYPE_ADDITIONAL_INFO,
    DVB_TTX_TYPE_SCHEDULE,
    DVB_TTX_TYPE_SUB_HEARING_IMP,
};

enum audio_type_e
{
    AUDIO_TYPE_UNDEFINED,
    AUDIO_TYPE_CLEAN_EFFECTS,
    AUDIO_TYPE_HEARING_IMPAIRED,
    AUDIO_TYPE_VISUAL_IMPAIRED,
};

/* NOTE: this is the same structure as ts_dvb_ttx_t in libmpegts */
typedef struct
{
    char dvb_teletext_lang_code[4];
    int dvb_teletext_type;
    int dvb_teletext_magazine_number;
    int dvb_teletext_page_number;
} obe_teletext_opts_t;

typedef struct
{
    int passthrough_opts;

    int pid;

    /* Audio */
    int frames_per_pes;

    /* Not used for teletext */
    int write_lang_code;
    char lang_code[4];

    int audio_type;

    int has_stream_identifier;
    int stream_identifier;

    int num_teletexts;
    obe_teletext_opts_t *teletext_opts;

    /* DVB-VBI and DVB-TTX */
    int data_identifier;
} obe_ts_stream_opts_t;

/* Convert from SMPTE line notation (e.g. Line 284) to analogue line notation (e.g Line 21 Field 2)
 * Note: field is either 1 or 2. This function will not check the validity of the line.
 * Returns -1 if format is a progressive format */
int obe_convert_smpte_to_analogue( int format, int line_smpte, int *line_analogue, int *field );

/* Convert from analogue line notation (e.g Line 21 Field 2) to SMPTE line notation (e.g. Line 284)
 * Note: field is either 1 or 2. This function will not check the validity of the line
 * Returns -1 if format is a progressive format */
int obe_convert_analogue_to_smpte( int format, int line_analogue, int field, int *line_smpte );

/**** AVC Encoding ****/
/* Use this function to let OBE guess the encoding profile.
 * You can use the functions in the x264 API for tweaking or edit the parameter struct directly.
 * Be aware that some parameters will affect hardware support.
 */
int obe_populate_avc_encoder_params( obe_t *h, int input_stream_id, x264_param_t *param );

/**** 3DTV ****/
/* Arrangements - Frame Packing */
enum frame_packing_arrangement_e
{
    FRAME_PACKING_NONE          = -1,
    FRAME_PACKING_CHECKERBOARD,
    FRAME_PACKING_COLUMN,
    FRAME_PACKING_ROW,
    FRAME_PACKING_SIDE_BY_SIDE,
    FRAME_PACKING_TOP_BOTTOM,
    FRAME_PACKING_TEMPORAL,
};

/**** AC-3 Encoding ****/
/* Options:
 *
 * override - Metadata will be passed through if E-distribution audio or AC-3 audio is used or the SDI stream has metadata as per SMPTE 2020-AB.
 *            This flag forces the use of the specified settings
 *
 * dialnorm - dialogue normalisation (valid range -31 to -1)
 */
typedef struct
{
    int override;

    int dialnorm;
    int dsur_mode;
    int original;


} obe_audio_metadata_t;

/**** AAC Encoding ****/
enum aac_profile_e
{
    AAC_LC,
    AAC_HE_V1,
    AAC_HE_V2,
};

typedef struct
{
    int aac_profile;
    int latm_output;
} obe_aac_opts_t;

/**** Ancillary Data ****/
typedef struct
{
     unsigned int cea_608: 1;
     unsigned int cea_708: 1;
     unsigned int afd: 1;
     unsigned int wss_to_afd: 1;
} obe_frame_anc_opts_t;

typedef struct
{
     unsigned int ttx: 1;
     unsigned int inverted_ttx: 1;
     unsigned int vps: 1;
     unsigned int wss: 1;
} obe_dvb_vbi_opts_t;

/* Stream Options:
 *
 * input_stream_id - stream id of the INPUT stream
 * output_stream_id - unique id of the current OUTPUT stream
 * stream_action - stream action. Video streams must be encoded
 *
 * Encode Options: (ignored in passthrough mode)
 * stream_format - stream_format
 *
 * Audio Options:
 * sdi_channel_pair - channel pair to use for encoding stereo (starts from channel pair 1)
 *
 * */

typedef struct
{
    int input_stream_id;
    int output_stream_id;
    int stream_action;

    /** Encode options **/
    int stream_format;

    /* Video */
    int is_wide;
    obe_frame_anc_opts_t video_anc;

    /* AVC */
    x264_param_t avc_param;

    /* Audio */
    int bitrate;
    int sdi_audio_pair;
    uint64_t channel_layout;
    int mono_channel;
    int audio_offset; /* in milliseconds */

    /* Metadata */
    obe_audio_metadata_t audio_metadata;

    /* AAC */
    obe_aac_opts_t aac_opts;

    /* MP2 */
    int mp2_mode;

    /* DVB-VBI */
    obe_dvb_vbi_opts_t dvb_vbi_opts;

    /** Mux options **/
    /* MPEG-TS */
    obe_ts_stream_opts_t ts_opts;
} obe_output_stream_t;

int obe_setup_streams( obe_t *h, obe_output_stream_t *output_streams, int num_streams );

/**** Muxers *****/
enum muxers_e
{
    MUXERS_MPEGTS,
};

/**** Transport Stream ****/
enum obe_ts_type_t
{
    OBE_TS_TYPE_GENERIC,
    OBE_TS_TYPE_DVB,
    OBE_TS_TYPE_CABLELABS,
    OBE_TS_TYPE_ATSC,
    OBE_TS_TYPE_ISDB,
};

typedef struct
{
    int muxer;

    /** MPEG-TS **/
    int ts_type;
    int cbr;
    int ts_muxrate;

    int passthrough;
// TODO mention only passthrough next four things
// TODO mention default pids
    int ts_id;
    int program_num;
    int pmt_pid;
    int pcr_pid;

    int pcr_period;
    int pat_period;

    int is_3dtv;

    /* DVB */
    char *service_name;
    char *provider_name;

    /* ATSC */
    int sb_leak_rate;
    int sb_size;
} obe_mux_opts_t;

int obe_setup_muxer( obe_t *h, obe_mux_opts_t *mux_opts );

/**** Output *****/
enum output_e
{
    OUTPUT_UDP, /* MPEG-TS in UDP */
    OUTPUT_RTP, /* MPEG-TS in RTP in UDP */
//    OUTPUT_LINSYS_ASI,
//    OUTPUT_LINSYS_SMPTE_310M,
};

/* Output structure
 *
 * target - TODO document url parameters
 *
 */

typedef struct
{
    int type;
    char *target;
} obe_output_dest_t;

typedef struct
{
    int num_outputs;
    obe_output_dest_t *outputs;
} obe_output_opts_t;

int obe_setup_output( obe_t *h, obe_output_opts_t *output_opts );

int obe_start( obe_t *h );
int obe_stop( obe_t *h );

void obe_close( obe_t *h );

#endif
