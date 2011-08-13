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
        if( non_display_data->frame_data[i].location == location )
            count++;
    }

    stream->num_frame_data = count;
    stream->frame_data = calloc( 1, stream->num_frame_data * sizeof(*stream->frame_data) );
    if( !stream->frame_data && stream->num_frame_data )
        return -1;

    for( int i = 0; i < non_display_data->num_frame_data; i++ )
    {
        if( non_display_data->frame_data[i].location == location )
        {
            stream->frame_data[idx].type = non_display_data->frame_data[i].type;
            stream->frame_data[idx].source = non_display_data->frame_data[i].source;
            stream->frame_data[idx].line_number = non_display_data->frame_data[i].line_number;
            idx++;
        }
    }

    return 0;
}

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
