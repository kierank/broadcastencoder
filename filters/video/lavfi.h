/*****************************************************************************
 * lavfi.h: libavfilter headers
 *****************************************************************************
 * Copyright (C) 2010-2012 Open Broadcast Systems Ltd
 *
 * Authors: Kieran Kunhya <kieran@ob-encoder.com>
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

#ifndef OBE_FILTERS_VIDEO_LAVFI_H
#define OBE_FILTERS_VIDEO_LAVFI_H

int init_lavfi( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame );
int lavfi_filter_frame( obe_t *h, obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame );

#endif
