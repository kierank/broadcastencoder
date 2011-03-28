/*****************************************************************************
 * sdi_bits.h : Bit handling helpers
 *****************************************************************************
 * Copyright (C) 2003 the VideoLAN team
 * $Id: 0f9cbfe93686319fc2285767b8c4019555451f4c $
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

#ifndef SDI_BITS_H
#define SDI_BITS_H

/**
 * \file
 * This file defines functions, structures for handling streams of bits in vlc
 */

typedef struct sdi_bs_s
{
    uint8_t *p_start;
    uint8_t *p;
    uint8_t *p_end;

    ssize_t  i_left;    /* i_count number of available bits */
} sdi_bs_t;

static inline void sdi_bs_init( sdi_bs_t *s, const void *p_data, size_t i_data )
{
    s->p_start = (void *)p_data;
    s->p       = s->p_start;
    s->p_end   = s->p_start + i_data;
    s->i_left  = 8;
}

static inline int sdi_bs_pos( const sdi_bs_t *s )
{
    return( 8 * ( s->p - s->p_start ) + 8 - s->i_left );
}

static inline int sdi_bs_eof( const sdi_bs_t *s )
{
    return( s->p >= s->p_end ? 1: 0 );
}

static inline uint16_t sdi_bs_read( sdi_bs_t *s )
{
    static const uint16_t i_mask[17] =
    { 
        0x00,
        0x01,      0x03,      0x07,      0x0f,
        0x1f,      0x3f,      0x7f,      0xff,
        0x1ff,     0x3ff,     0x7ff,     0xfff,
        0x1fff,    0x3fff,    0x7fff,    0xffff,
    };
    int      i_shr;
    uint32_t i_result = 0;
    int      i_count = 10;

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
             return i_result;
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

    return i_result;
}

static inline uint16_t sdi_bs_show10( sdi_bs_t *s )
{
    sdi_bs_t     s_tmp = *s;
    return sdi_bs_read( &s_tmp );
}

static inline void sdi_bs_skip( sdi_bs_t *s, ssize_t i_count )
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
