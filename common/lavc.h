/*****************************************************************************
 * lavc.h: libavcodec common functions header
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

#ifndef OBE_INPUT_LAVC_H
#define OBE_INPUT_LAVC_H

#include <libavcodec/avcodec.h>

int obe_get_buffer( AVCodecContext *codec, AVFrame *pic );
void obe_release_buffer( AVCodecContext *codec, AVFrame *pic );
int obe_reget_buffer( AVCodecContext *codec, AVFrame *pic );
int obe_lavc_lockmgr( void **mutex, enum AVLockOp op );

#endif
