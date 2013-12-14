/*****************************************************************************
 * sdi.h: sdi asm prototypes
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

#ifndef OBE_X86_SDI
#define OBE_X86_SDI

void obe_downscale_line_mmx( uint16_t *src, uint8_t *dst, int lines );
void obe_downscale_line_sse2( uint16_t *src, uint8_t *dst, int lines );

void obe_v210_planar_unpack_unaligned_ssse3( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );
void obe_v210_planar_unpack_unaligned_avx( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );

void obe_v210_planar_unpack_aligned_ssse3( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );
void obe_v210_planar_unpack_aligned_avx( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );

void obe_v210_line_to_nv20_ssse3( uint16_t *dsty, intptr_t i_dsty, uint16_t *dstc, intptr_t i_dstc, uint32_t *src, intptr_t i_src, intptr_t width, intptr_t h );
void obe_v210_line_to_nv20_avx( uint16_t *dsty, intptr_t i_dsty, uint16_t *dstc, intptr_t i_dstc, uint32_t *src, intptr_t i_src, intptr_t width, intptr_t h );

#endif
