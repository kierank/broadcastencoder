/*****************************************************************************
 * sdi.c: SDI functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Some code originates from the FFmpeg project
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

#include "sdi.h"
#include <libavutil/bswap.h>

#define READ_PIXELS(a, b, c)         \
    do {                             \
        val  = av_le2ne32( *src++ ); \
        *a++ =  val & 0x3ff;         \
        *b++ = (val >> 10) & 0x3ff;  \
        *c++ = (val >> 20) & 0x3ff;  \
    } while (0)

void obe_v210_planar_unpack_c( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width )
{
    uint32_t val;

    for( int i = 0; i < width - 5; i += 6 )
    {
        READ_PIXELS( u, y, v );
        READ_PIXELS( y, u, y );
        READ_PIXELS( v, y, u );
        READ_PIXELS( y, v, y );
    }
}

/* Convert v210 to the native HD-SDI pixel format. */
void obe_v210_line_to_nv20_c( uint32_t *src, uint16_t *dst, int width )
{
    int w;
    uint32_t val;
    uint16_t *uv = dst + width;
    for( w = 0; w < width - 5; w += 6 )
    {
        READ_PIXELS( uv, dst, uv );
        READ_PIXELS( dst, uv, dst );
        READ_PIXELS( uv, dst, uv );
        READ_PIXELS( dst, uv, dst );
    }

    if( w < width - 1 )
    {
        READ_PIXELS(uv, dst, uv);

        val    = av_le2ne32( *src++ );
        *dst++ =  val & 0x3ff;
    }

    if( w < width - 3 )
    {
        *uv++  = (val >> 10) & 0x3ff;
        *dst++ = (val >> 20) & 0x3ff;

        val    = av_le2ne32( *src++ );
        *uv++  =  val & 0x3ff;
        *dst++ = (val >> 10) & 0x3ff;
    }
}

/* Convert v210 to the native SD-SDI pixel format.
 * Width is always 720 samples */
void obe_v210_line_to_uyvy_c( uint32_t *src, uint16_t *dst, int width )
{
    uint32_t val;
    for( int i = 0; i < width; i += 6 )
    {
        READ_PIXELS( dst, dst, dst );
        READ_PIXELS( dst, dst, dst );
        READ_PIXELS( dst, dst, dst );
        READ_PIXELS( dst, dst, dst );
    }
}

/* Convert YUV422P10 to the native HD-SDI pixel format. */
void obe_yuv422p10_line_to_nv20_c( uint16_t *y, uint16_t *u, uint16_t *v, uint16_t *dst, int width )
{
    uint16_t *uv = dst + width;
    for( int i = 0; i < width; i += 2 )
    {
        *dst++ = *y++;
        *dst++ = *y++;
        *uv++  = *u++;
        *uv++  = *v++;
    }
}

/* Convert YUV422P10 to the native SD-SDI pixel format.
 * Width is always 720 samples */
void obe_yuv422p10_line_to_uyvy_c( uint16_t *y, uint16_t *u, uint16_t *v, uint16_t *dst, int width )
{
    for( int i = 0; i < width; i += 2 )
    {
        *dst++ = *u++;
        *dst++ = *y++;
        *dst++ = *v++;
        *dst++ = *y++;
    }
}

/* Downscale 10-bit lines to 8-bit lines for processing by libzvbi.
 * Width is always 720*2 samples */
void obe_downscale_line_c( uint16_t *src, uint8_t *dst, int lines )
{
    for( int i = 0; i < 720*2*lines; i++ )
        dst[i] = src[i] >> 2;
}

void obe_blank_line_nv20_c( uint16_t *dst, int width )
{
    uint16_t *uv = dst + width;
    for( int i = 0; i < width; i++ )
    {
        *dst++ = 0x40;
        *uv++  = 0x200;
    }
}

void obe_blank_line_uyvy_c( uint16_t *dst, int width )
{
    for( int i = 0; i < width; i++ )
    {
        *dst++ = 0x200;
        *dst++ = 0x40;
    }
}

int add_non_display_services( obe_sdi_non_display_data_t *non_display_data, obe_int_input_stream_t *stream, int location )
{
    int idx = 0, count = 0;

    for( int i = 0; i < non_display_data->num_frame_data; i++ )
    {
        if( non_display_data->frame_data[i].type == MISC_TELETEXT )
            continue;
        else if( non_display_data->frame_data[i].location == location )
            count++;
    }

    stream->num_frame_data = count;
    stream->frame_data = calloc( stream->num_frame_data, sizeof(*stream->frame_data) );
    if( !stream->frame_data && stream->num_frame_data )
        return -1;

    for( int i = 0; i < non_display_data->num_frame_data; i++ )
    {
        if( non_display_data->frame_data[i].type == MISC_TELETEXT )
            continue;
        else if( non_display_data->frame_data[i].location == location )
        {
            stream->frame_data[idx].type = non_display_data->frame_data[i].type;
            stream->frame_data[idx].source = non_display_data->frame_data[i].source;
            stream->frame_data[idx].num_lines = non_display_data->frame_data[i].num_lines;
            memcpy( stream->frame_data[idx].lines, non_display_data->frame_data[i].lines, non_display_data->frame_data[i].num_lines * sizeof(int) );
            idx++;
        }
    }

    return 0;
}

int check_probed_non_display_data( obe_sdi_non_display_data_t *non_display_data, int type )
{
    for( int i = 0; i < non_display_data->num_frame_data; i++ )
    {
        if( non_display_data->frame_data[i].type == type )
            return 1;
    }

    return 0;
}

int check_active_non_display_data( obe_raw_frame_t *raw_frame, int type )
{
    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        if( raw_frame->user_data[i].type == type )
            return 1;
    }

    return 0;
}

int check_user_selected_non_display_data( obe_t *h, int type, int location )
{
    obe_output_stream_t *output_stream;

    if( location == USER_DATA_LOCATION_DVB_STREAM )
    {
        output_stream = get_output_stream( h, VBI_RAW );
        if( !output_stream )
            return 0;

        switch( type )
        {
            case MISC_TELETEXT:
                return output_stream->dvb_vbi_opts.ttx;
            case MISC_TELETEXT_INVERTED:
                return output_stream->dvb_vbi_opts.inverted_ttx;
            case MISC_VPS:
                return output_stream->dvb_vbi_opts.vps;
            case MISC_WSS:
                return output_stream->dvb_vbi_opts.wss;
        }
    }
    else if( location == USER_DATA_LOCATION_FRAME )
    {
        /* Assumes video frame has stream_id=0 */
        output_stream = get_output_stream( h, 0 );
        /* Should never happen */
        if( !output_stream )
            return 0;

        switch( type )
        {
            case CAPTIONS_CEA_608:
                return output_stream->video_anc.cea_608;
            case CAPTIONS_CEA_708:
                return output_stream->video_anc.cea_708;
            case MISC_AFD:
                return output_stream->video_anc.afd;
            /* Actually WSS to AFD conversion */
            case MISC_WSS:
                return output_stream->video_anc.wss_to_afd;
        }
    }

    return 0;
}

int add_teletext_service( obe_sdi_non_display_data_t *non_display_data, obe_int_input_stream_t *stream )
{
    stream->frame_data = calloc( 1, sizeof(*stream->frame_data) );
    if( !stream->frame_data )
        return -1;

    for( int i = 0; i < non_display_data->num_frame_data; i++ )
    {
        if( non_display_data->frame_data[i].type == MISC_TELETEXT )
        {
            stream->frame_data[0].type = non_display_data->frame_data[i].type;
            stream->frame_data[0].source = non_display_data->frame_data[i].source;
            stream->frame_data[0].num_lines = non_display_data->frame_data[i].num_lines;
            memcpy( stream->frame_data[0].lines, non_display_data->frame_data[0].lines, non_display_data->frame_data[0].num_lines * sizeof(int) );
        }
    }

    return 0;
}

/* FIXME: these functions don't include the centre line */
int sdi_next_line( int format, int line_smpte )
{
    int i;

    if( !IS_INTERLACED( format ) )
        return line_smpte+1;

    for( i = 0; field_start_lines[i].format != -1; i++ )
    {
        if( format == field_start_lines[i].format )
            break;
    }

    if( line_smpte >= field_start_lines[i].line && line_smpte < field_start_lines[i].field_two )
        return field_start_lines[i].field_two - field_start_lines[i].line + line_smpte;
    else
        return line_smpte - field_start_lines[i].field_two + field_start_lines[i].line + 1;
}

int obe_convert_smpte_to_analogue( int format, int line_smpte, int *line_analogue, int *field )
{
    int i;

    if( !IS_INTERLACED( format ) )
        return -1;

    for( i = 0; field_start_lines[i].format != -1; i++ )
    {
        if( format == field_start_lines[i].format )
            break;
    }

    if( line_smpte >= field_start_lines[i].line && line_smpte < field_start_lines[i].field_two )
    {
        *line_analogue = line_smpte;
        *field = 1;
    }
    else
    {
        *line_analogue = line_smpte - field_start_lines[i].field_two + field_start_lines[i].line;
        *field = 2;
    }

    return 0;
}

int obe_convert_analogue_to_smpte( int format, int line_analogue, int field, int *line_smpte )
{
    int i;

    if( !IS_INTERLACED( format ) )
        return -1;

    if( field == 1 )
        *line_smpte = line_analogue;
    else
    {
        for( i = 0; field_start_lines[i].format != -1; i++ )
        {
            if( format == field_start_lines[i].format )
                break;
        }

        *line_smpte = field_start_lines[i].field_two - field_start_lines[i].line + line_analogue;
    }

    return 0;
}
