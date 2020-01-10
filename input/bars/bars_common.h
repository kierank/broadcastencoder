/*****************************************************************************
 * bars_common.h: SMPTE bars common header
 *****************************************************************************
 * Copyright (C) 2014 Open Broadcast Systems Ltd.
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

#include "common/common.h"

typedef struct
{
    int video_format;
    int no_signal;

    char *bars_line1;
    char *bars_line2;
    char *bars_line3;
    char *bars_line4;

    int bars_beep;
    int bars_beep_interval;
} obe_bars_opts_t;

int open_bars( hnd_t *p_handle, obe_bars_opts_t *obe_bars_opts );
int get_bars( hnd_t ptr, obe_raw_frame_t **raw_frames );
void close_bars( hnd_t ptr );
