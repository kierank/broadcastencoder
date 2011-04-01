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

#include <sys/types.h>

typedef struct sdi_bs_t
{
    uint8_t *p_start;
    uint8_t *p;
    uint8_t *p_end;

    ssize_t  i_left;    /* i_count number of available bits */
} sdi_bs_t;

static const uint8_t mask[9] =
{
    0x00,
    0x01,      0x03,      0x07,      0x0f,
    0x1f,      0x3f,      0x7f,      0xff,
};

static inline void sdi_bs_init( sdi_bs_t *s, const void *p_data, size_t i_data )
{
    s->p_start = (void *)p_data;
    s->p       = s->p_start;
    s->p_end   = s->p_start + i_data;
    s->i_left  = 8;
}

static inline uint16_t sdi_bs_read( sdi_bs_t *s )
{
    uint32_t result = 0;

    result |= ( *s->p++ >> ( 8 - s->i_left ) );
    result |= ( *s->p & mask[10 - s->i_left] ) << s->i_left;

    s->i_left -= 2;
    if( s->i_left == 0 )
    {
        s->i_left = 8;
        s->p++;
    }

    return result;
}

static inline uint16_t sdi_bs_show( sdi_bs_t *s )
{
    sdi_bs_t     s_tmp = *s;
    return sdi_bs_read( &s_tmp );
}

#endif
