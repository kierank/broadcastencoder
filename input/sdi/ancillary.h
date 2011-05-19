/*****************************************************************************
 * ancillary.h : OBE ancillary headers
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

#ifndef OBE_ANCILLARY_H
#define OBE_ANCILLARY_H

#include "common/common.h"

typedef struct
{
    int did;
    int sdid;
    int type;
} obe_vanc_identifier_t;

obe_vanc_identifier_t vanc_identifiers =
{
    { 0x41, 0x5, MISC_AFD },
    { 0x41, 0x6, MISC_PAN_SCAN },
    { 0x41, 0x8, ANC_DVB_SCTE_VBI },

    { 0x43, 0x1, ANC_OP47_SDP },
    { 0x43, 0x2, ANC_OP47_MULTI_PACKET },

    { 0x60, 0x60, ANC_ATC },

    { 0x61, 0x1, SUBTITLES_CEA_708 },
    { 0x61, 0x1, SUBTITLES_CEA_608 },

    { 0x62, 0x1, ANC_DTV_PROGRAM_DESCRIPTION },
    { 0x62, 0x2, ANC_DTV_DATA_BROADCAST },
    { 0x62, 0x3, ANC_SMPTE_VBI },

    { 0, 0, 0 },
}

#endif
