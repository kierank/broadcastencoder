/*****************************************************************************
 * vbi.h: OBE VBI parsing functions
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

#ifndef OBE_VBI_H
#define OBE_VBI_H

#define NUM_ACTIVE_VBI_LINES 4

#define PAL_VIDEO_INDEX_LINE  11
#define NTSC_VIDEO_INDEX_LINE 14

int setup_vbi_parser( obe_sdi_non_display_data_t *non_display_data, int ntsc );
int decode_vbi( obe_sdi_non_display_data_t *non_display_data, uint8_t *lines, obe_raw_frame_t *raw_frame );

#endif
