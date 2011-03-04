/*****************************************************************************
 * bitstream.h: bitstream writing
 *****************************************************************************
 * Copyright (C) 2003-2010 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
 *          Laurent Aimar <fenrir@via.ecp.fr>
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

#ifndef OBE_BS_H
#define OBE_BS_H

#define WORD_SIZE sizeof(long)

/* Unions for type-punning.
 * Mn: load or store n bits, aligned, native-endian
 * CPn: copy n bits, aligned, native-endian
 * we don't use memcpy for CPn because memcpy's args aren't assumed to be aligned */
typedef union { uint16_t i; uint8_t  c[2]; } x264_union16_t;
typedef union { uint32_t i; uint16_t b[2]; uint8_t  c[4]; } x264_union32_t;
typedef union { uint64_t i; uint32_t a[2]; uint16_t b[4]; uint8_t c[8]; } x264_union64_t;
typedef struct { uint64_t i[2]; } x264_uint128_t;
typedef union { x264_uint128_t i; uint64_t a[2]; uint32_t b[4]; uint16_t c[8]; uint8_t d[16]; } x264_union128_t;
#define M16(src) (((x264_union16_t*)(src))->i)
#define M32(src) (((x264_union32_t*)(src))->i)
#define M64(src) (((x264_union64_t*)(src))->i)
#define M128(src) (((x264_union128_t*)(src))->i)
#define M128_ZERO ((x264_uint128_t){{0,0}})
#define CP16(dst,src) M16(dst) = M16(src)
#define CP32(dst,src) M32(dst) = M32(src)
#define CP64(dst,src) M64(dst) = M64(src)
#define CP128(dst,src) M128(dst) = M128(src)

#if WORDS_BIGENDIAN
#define endian_fix(x) (x)
#define endian_fix64(x) (x)
#define endian_fix32(x) (x)
#define endian_fix16(x) (x)
#else
static inline uint32_t endian_fix32( uint32_t x )
{
    return (x<<24) + ((x<<8)&0xff0000) + ((x>>8)&0xff00) + (x>>24);
}
static inline uint64_t endian_fix64( uint64_t x )
{
    return endian_fix32(x>>32) + ((uint64_t)endian_fix32(x)<<32);
}
static inline intptr_t endian_fix( intptr_t x )
{
    return WORD_SIZE == 8 ? endian_fix64(x) : endian_fix32(x);
}
static inline uint16_t endian_fix16( uint16_t x )
{
    return (x<<8)|(x>>8);
}
#endif

typedef struct bs_s
{
    uint8_t *p_start;
    uint8_t *p;
    uint8_t *p_end;

    intptr_t cur_bits;
    int     i_left;    /* i_count number of available bits */
} bs_t;

static inline void bs_init( bs_t *s, void *p_data, int i_data )
{
    int offset = ((intptr_t)p_data & 3);
    s->p       = s->p_start = (uint8_t*)p_data - offset;
    s->p_end   = (uint8_t*)p_data + i_data;
    s->i_left  = (WORD_SIZE - offset)*8;
    s->cur_bits = endian_fix32( M32(s->p) );
    s->cur_bits >>= (4-offset)*8;
}
static inline int bs_pos( bs_t *s )
{
    return( 8 * (s->p - s->p_start) + (WORD_SIZE*8) - s->i_left );
}

/* Write the rest of cur_bits to the bitstream; results in a bitstream no longer 32-bit aligned. */
static inline void bs_flush( bs_t *s )
{
    M32( s->p ) = endian_fix32( s->cur_bits << (s->i_left&31) );
    s->p += WORD_SIZE - (s->i_left >> 3);
    s->i_left = WORD_SIZE*8;
}
/* The inverse of bs_flush: prepare the bitstream to be written to again. */
static inline void bs_realign( bs_t *s )
{
    int offset = ((intptr_t)s->p & 3);
    if( offset )
    {
        s->p       = (uint8_t*)s->p - offset;
        s->i_left  = (WORD_SIZE - offset)*8;
        s->cur_bits = endian_fix32( M32(s->p) );
        s->cur_bits >>= (4-offset)*8;
    }
}

static inline void bs_write( bs_t *s, int i_count, uint32_t i_bits )
{
    if( WORD_SIZE == 8 )
    {
        s->cur_bits = (s->cur_bits << i_count) | i_bits;
        s->i_left -= i_count;
        if( s->i_left <= 32 )
        {
#if WORDS_BIGENDIAN
            M32( s->p ) = s->cur_bits >> (32 - s->i_left);
#else
            M32( s->p ) = endian_fix( s->cur_bits << s->i_left );
#endif
            s->i_left += 32;
            s->p += 4;
        }
    }
    else
    {
        if( i_count < s->i_left )
        {
            s->cur_bits = (s->cur_bits << i_count) | i_bits;
            s->i_left -= i_count;
        }
        else
        {
            i_count -= s->i_left;
            s->cur_bits = (s->cur_bits << s->i_left) | (i_bits >> i_count);
            M32( s->p ) = endian_fix( s->cur_bits );
            s->p += 4;
            s->cur_bits = i_bits;
            s->i_left = 32 - i_count;
        }
    }
}

/* Special case to eliminate branch in normal bs_write. */
/* Golomb never writes an even-size code, so this is only used in slice headers. */
static inline void bs_write32( bs_t *s, uint32_t i_bits )
{
    bs_write( s, 16, i_bits >> 16 );
    bs_write( s, 16, i_bits );
}

static inline void bs_write1( bs_t *s, uint32_t i_bit )
{
    s->cur_bits <<= 1;
    s->cur_bits |= i_bit;
    s->i_left--;
    if( s->i_left == WORD_SIZE*8-32 )
    {
        M32( s->p ) = endian_fix32( s->cur_bits );
        s->p += 4;
        s->i_left = WORD_SIZE*8;
    }
}

static inline void bs_align_0( bs_t *s )
{
    bs_write( s, s->i_left&7, 0 );
    bs_flush( s );
}
static inline void bs_align_1( bs_t *s )
{
    bs_write( s, s->i_left&7, (1 << (s->i_left&7)) - 1 );
    bs_flush( s );
}
static inline void bs_align_10( bs_t *s )
{
    if( s->i_left&7 )
        bs_write( s, s->i_left&7, 1 << ( (s->i_left&7) - 1 ) );
}

/* golomb functions */

static const uint8_t x264_ue_size_tab[256] =
{
     1, 1, 3, 3, 5, 5, 5, 5, 7, 7, 7, 7, 7, 7, 7, 7,
     9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,
    11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,
    13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
    13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
    13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
    13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
};

static inline void bs_write_ue_big( bs_t *s, unsigned int val )
{
    int size = 0;
    int tmp = ++val;
    if( tmp >= 0x10000 )
    {
        size = 32;
        tmp >>= 16;
    }
    if( tmp >= 0x100 )
    {
        size += 16;
        tmp >>= 8;
    }
    size += x264_ue_size_tab[tmp];
    bs_write( s, size>>1, 0 );
    bs_write( s, (size>>1)+1, val );
}

/* Only works on values under 255. */
static inline void bs_write_ue( bs_t *s, int val )
{
    bs_write( s, x264_ue_size_tab[val+1], val+1 );
}

static inline void bs_write_se( bs_t *s, int val )
{
    int size = 0;
    /* Faster than (val <= 0 ? -val*2+1 : val*2) */
    /* 4 instructions on x86, 3 on ARM */
    int tmp = 1 - val*2;
    if( tmp < 0 ) tmp = val*2;
    val = tmp;

    if( tmp >= 0x100 )
    {
        size = 16;
        tmp >>= 8;
    }
    size += x264_ue_size_tab[tmp];
    bs_write( s, size, val );
}

static inline void bs_write_te( bs_t *s, int x, int val )
{
    if( x == 1 )
        bs_write1( s, 1^val );
    else //if( x > 1 )
        bs_write_ue( s, val );
}

static inline void bs_rbsp_trailing( bs_t *s )
{
    bs_write1( s, 1 );
    bs_write( s, s->i_left&7, 0  );
}

#endif
