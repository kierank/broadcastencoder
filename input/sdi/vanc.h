/*****************************************************************************
 * vanc.h : OBE vanc headers
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

#ifndef OBE_INPUT_SDI_VANC_H
#define OBE_INPUT_SDI_VANC_H

#include "common/common.h"

/* DID, SDID, Type */
int obe_vanc_identifier[][3] =
{
    { 0x41, 0x5, 0 }, // TODO AFD

    { 0x43, 0x2, 0 }, // TODO OP-47

    { 0x61, 0x1, SUBTITLES_CEA_708 },
    { 0x61, 0x1, SUBTITLES_CEA_608 },

    { 0x62, 0x1, 0 }, // TODO Program description (DTV)
    { 0x62, 0x2, 0 }, // TODO Data broadcast (DTV)
    { 0x62, 0x3, 0 }, // TODO VBI data

    { 0, 0, 0 },
}

#endif
