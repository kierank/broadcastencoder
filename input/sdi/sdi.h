/*****************************************************************************
 * sdi.h: OBE SDI generic headers
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

#ifndef OBE_SDI_H
#define OBE_SDI_H

#include "common/common.h"
#include <libzvbi.h>

typedef struct
{
    int probe;
    vbi_raw_decoder vbi_decoder;

    /* Probing */
    int has_probed;
    int num_frame_data;
    obe_int_frame_data_t *frame_data;

    /* Decoding */
    obe_coded_frame_t *dvb_frame;

    /* TODO: start/end lines etc */
} obe_sdi_non_display_data_t;

/* NB: Lines start from 1 */
typedef struct
{
    int format;
    int line;
} obe_line_number_t;

typedef struct
{
    int service;
    int location;
} obe_non_display_data_location_t;

const obe_non_display_data_location_t non_display_data_locations[] =
{
    { CAPTIONS_CEA_608, USER_DATA_LOCATION_FRAME },
    { MISC_WSS,         USER_DATA_LOCATION_DVB_STREAM },
    { -1, -1 },
};

#endif
