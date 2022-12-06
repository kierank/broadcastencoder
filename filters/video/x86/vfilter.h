/*****************************************************************************
 * vfilter.h: video filter asm prototypes
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

#ifndef OBE_X86_VFILTER
#define OBE_X86_VFILTER

void obe_scale_plane_mmxext( uint16_t *src, int stride, int width, int height, int lshift, int rshift );
void obe_scale_plane_sse2( uint16_t *src, int stride, int width, int height, int lshift, int rshift );
void obe_scale_plane_avx( uint16_t *src, int stride, int width, int height, int lshift, int rshift );

#define ARGS void *src_ptr, ptrdiff_t src_stride, \
             void *dst_ptr, ptrdiff_t dst_stride, \
             uintptr_t width, uintptr_t height
void obe_downsample_chroma_fields_8_sse2( ARGS );
void obe_downsample_chroma_fields_10_sse2( ARGS );
void obe_downsample_chroma_fields_8_ssse3( ARGS );
void obe_downsample_chroma_fields_8_avx( ARGS );
void obe_downsample_chroma_fields_10_avx( ARGS );
void obe_downsample_chroma_fields_8_avx2( ARGS );
void obe_downsample_chroma_fields_10_avx2( ARGS );
void obe_downsample_chroma_fields_8_avx512icl( ARGS );
void obe_downsample_chroma_fields_10_avx512icl( ARGS );
#undef ARGS

#define ARGS uint16_t *src, ptrdiff_t src_stride, \
             uint8_t *dst,  ptrdiff_t dst_stride, \
             uintptr_t width, uintptr_t height
void obe_dither_plane_10_to_8_sse2( ARGS );
void obe_dither_plane_10_to_8_avx2( ARGS );
void obe_dither_plane_10_to_8_avx512icl( ARGS );
#undef ARGS

#endif
