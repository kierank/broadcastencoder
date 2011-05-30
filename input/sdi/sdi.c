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
        val  = av_le2ne32(*src++);   \
        *a++ =  val & 0x3FF;         \
        *b++ = (val >> 10) & 0x3FF;  \
        *c++ = (val >> 20) & 0x3FF;  \
    } while (0)

/* Convert v210 to the native HD-SDI pixel format. */
void v210_line_to_nv20_c( uint32_t *src, uint16_t *dst, int width )
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

    if( w < width - 1)
    {
        READ_PIXELS(uv, dst, uv);

        val    = av_le2ne32(*src++);
        *dst++ =  val & 0x3ff;
    }

    if( w < width - 3 )
    {
        *uv++  = (val >> 10) & 0x3ff;
        *dst++ = (val >> 20) & 0x3ff;

        val    = av_le2ne32(*src++);
        *uv++  =  val & 0x3ff;
        *dst++ = (val >> 10) & 0x3ff;
    }
}

/* Convert v210 to the native SD-SDI pixel format.
 * Width is always 720 samples */
void v210_line_to_uyvy_c( uint32_t *src, uint16_t *dst, int width )
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
void downscale_line_c( uint16_t *src, uint8_t *dst, int lines )
{
    for( int i = 0; i < 720*2*lines; i++ )
        dst[i] = src[i] >> 2;
}
