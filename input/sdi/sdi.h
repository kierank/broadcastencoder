/*****************************************************************************
 * sdi.h: OBE SDI generic headers
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

#ifndef OBE_SDI_H
#define OBE_SDI_H

#include "common/common.h"
#undef ZVBI_DEBUG
#include <libzvbi.h>
#include <libavutil/crc.h>

/* In 27MHz ticks */
#define SDI_MAX_DELAY 1350000

typedef struct
{
    int line;
    int identifier;
    int unit_id;
    int len;
    uint8_t *data;
} obe_anc_vbi_t;

typedef struct
{
    int probe;
    vbi_raw_decoder vbi_decoder;

    /* Probing */
    int has_probed;
    int num_frame_data;
    obe_int_frame_data_t *frame_data;

    /* Decoding */
    obe_coded_frame_t *dvb_vbi_frame;
    obe_coded_frame_t *dvb_ttx_frame;

    /* VBI */
    int ntsc;
    int num_vbi;
    vbi_sliced vbi_slices[100];
    int has_vbi_frame;
    int has_ttx_frame;

    /* Ancillary VBI */
    int num_anc_vbi;
    obe_anc_vbi_t anc_vbi[100];

    /* Video Index Information */
    AVCRC crc[257];
    AVCRC crc_broken[257];

    obe_device_t *device;
} obe_sdi_non_display_data_t;

/* NB: Lines start from 1 */
typedef struct
{
    int format;
    int line;
    int field_two; /* where relevant */
} obe_line_number_t;

/* For NTSC this is not the same as the first coded line */
const static obe_line_number_t first_active_line[] =
{
    { INPUT_VIDEO_FORMAT_PAL,   23 },
    { INPUT_VIDEO_FORMAT_NTSC,  283 },
    { INPUT_VIDEO_FORMAT_720P_50,    26 },
    { INPUT_VIDEO_FORMAT_720P_5994,  26 },
    { INPUT_VIDEO_FORMAT_720P_60,    26 },
    { INPUT_VIDEO_FORMAT_1080I_50,   21 },
    { INPUT_VIDEO_FORMAT_1080I_5994, 21 },
    { INPUT_VIDEO_FORMAT_1080I_60,   21 },
    { INPUT_VIDEO_FORMAT_1080P_2398, 42 },
    { INPUT_VIDEO_FORMAT_1080P_24,   42 },
    { INPUT_VIDEO_FORMAT_1080P_25,   42 },
    { INPUT_VIDEO_FORMAT_1080P_2997, 42 },
    { INPUT_VIDEO_FORMAT_1080P_30,   42 },
    { INPUT_VIDEO_FORMAT_1080P_50,   42 },
    { INPUT_VIDEO_FORMAT_1080P_5994, 42 },
    { INPUT_VIDEO_FORMAT_1080P_60,   42 },
    { -1, -1 },
};

const static obe_line_number_t field_start_lines[] =
{
    { INPUT_VIDEO_FORMAT_PAL,  1, 314 }, /* Skip middle line */
    { INPUT_VIDEO_FORMAT_NTSC, 4, 267 }, /* Skip middle line */
    { INPUT_VIDEO_FORMAT_1080I_50,   1, 564 },
    { INPUT_VIDEO_FORMAT_1080I_5994, 1, 564 },
    { INPUT_VIDEO_FORMAT_1080I_60,   1, 564 },
    { -1, -1 },
};

void obe_v210_planar_unpack_c( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );
void obe_v210_line_to_nv20_c( uint16_t *dsty, intptr_t i_dsty, uint16_t *dstc, intptr_t i_dstc, uint32_t *src, intptr_t i_src, intptr_t width, intptr_t h );
void obe_v210_line_to_uyvy_c( uint16_t *dsty, intptr_t i_dsty, uint16_t *dstc, intptr_t i_dstc, uint32_t *src, intptr_t i_src, intptr_t width, intptr_t h );
void obe_yuv422p10_line_to_nv20_c( uint16_t *y, uint16_t *u, uint16_t *v, uint16_t *dst, int width );
void obe_yuv422p10_line_to_uyvy_c( uint16_t *y, uint16_t *u, uint16_t *v, uint16_t *dst, int width );
void obe_downscale_line_c( uint16_t *src, uint8_t *dst, int lines );
void obe_blank_line_nv20_c( uint16_t *dst, int width );
void obe_blank_line_uyvy_c( uint16_t *dst, int width );
int add_non_display_services( obe_sdi_non_display_data_t *non_display_data, obe_int_input_stream_t *stream, int location );
int check_probed_non_display_data( obe_sdi_non_display_data_t *non_display_data, int type );
int check_active_non_display_data( obe_raw_frame_t *raw_frame, int type );
int check_user_selected_non_display_data( obe_t *h, int type, int location );
int add_teletext_service( obe_sdi_non_display_data_t *non_display_data, obe_int_input_stream_t *stream );
int sdi_next_line( int format, int line_smpte );

#endif
