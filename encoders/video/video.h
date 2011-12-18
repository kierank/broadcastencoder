/*****************************************************************************
 * video.h : OBE video encoding headers
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

#ifndef OBE_ENCODERS_VIDEO_H
#define OBE_ENCODERS_VIDEO_H

typedef struct
{
    void* (*start_encoder)( void *ptr );
} obe_vid_enc_func_t;

typedef struct
{
    obe_t *h;
    obe_encoder_t *encoder;
    x264_param_t avc_param;
} obe_vid_enc_params_t;

extern const obe_vid_enc_func_t x264_encoder;

#endif
