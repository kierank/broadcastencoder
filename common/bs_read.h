/*****************************************************************************
 * bits.h : Bitstream reading functions
 *****************************************************************************
 * Copyright (C) 2003 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef OBE_BS_READ_H
#define OBE_BS_READ_H

/**
 * \file
 * This file defines functions, structures for handling streams of bits in vlc
 * TODO: merge this with x264's bitstream writer if possible
 */

typedef struct
{
    uint8_t *p_start;
    uint8_t *p;
    uint8_t *p_end;

    ssize_t  i_left;    /* i_count number of available bits */
} bs_read_t;

static inline void bs_read_init( bs_read_t *s, const void *p_data, size_t i_data )
{
    s->p_start = (void *)p_data;
    s->p       = s->p_start;
    s->p_end   = s->p_start + i_data;
    s->i_left  = 8;
}

static inline int bs_read_pos( const bs_read_t *s )
{
    return( 8 * ( s->p - s->p_start ) + 8 - s->i_left );
}

static inline int bs_read_eof( const bs_read_t *s )
{
    return( s->p >= s->p_end ? 1: 0 );
}

static inline uint32_t bs_read( bs_read_t *s, int i_count )
{
     static const uint32_t i_mask[33] =
     {  0x00,
        0x01,      0x03,      0x07,      0x0f,
        0x1f,      0x3f,      0x7f,      0xff,
        0x1ff,     0x3ff,     0x7ff,     0xfff,
        0x1fff,    0x3fff,    0x7fff,    0xffff,
        0x1ffff,   0x3ffff,   0x7ffff,   0xfffff,
        0x1fffff,  0x3fffff,  0x7fffff,  0xffffff,
        0x1ffffff, 0x3ffffff, 0x7ffffff, 0xfffffff,
        0x1fffffff,0x3fffffff,0x7fffffff,0xffffffff};
    int      i_shr;
    uint32_t i_result = 0;

    while( i_count > 0 )
    {
        if( s->p >= s->p_end )
        {
            break;
        }

        if( ( i_shr = s->i_left - i_count ) >= 0 )
        {
            /* more in the buffer than requested */
            i_result |= ( *s->p >> i_shr )&i_mask[i_count];
            s->i_left -= i_count;
            if( s->i_left == 0 )
            {
                s->p++;
                s->i_left = 8;
            }
            return( i_result );
        }
        else
        {
            /* less in the buffer than requested */
           i_result |= (*s->p&i_mask[s->i_left]) << -i_shr;
           i_count  -= s->i_left;
           s->p++;
           s->i_left = 8;
        }
    }

    return( i_result );
}

static inline uint32_t bs_read1( bs_read_t *s )
{
    if( s->p < s->p_end )
    {
        unsigned int i_result;

        s->i_left--;
        i_result = ( *s->p >> s->i_left )&0x01;
        if( s->i_left == 0 )
        {
            s->p++;
            s->i_left = 8;
        }
        return i_result;
    }

    return 0;
}

static inline uint32_t bs_show( bs_read_t *s, int i_count )
{
    bs_read_t     s_tmp = *s;
    return bs_read( &s_tmp, i_count );
}

static inline void bs_skip( bs_read_t *s, ssize_t i_count )
{
    s->i_left -= i_count;

    if( s->i_left <= 0 )
    {
        const int i_bytes = ( -s->i_left + 8 ) / 8;

        s->p += i_bytes;
        s->i_left += 8 * i_bytes;
    }
}

#endif
