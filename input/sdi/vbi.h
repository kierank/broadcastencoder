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

#define NUM_ACTIVE_VBI_LINES  2

#define PAL_VIDEO_INDEX_LINE  11
#define NTSC_VIDEO_INDEX_LINE 14
// TODO: Video index field 2

enum dvb_vbi_data_unit_id_e
{
    DATA_UNIT_ID_EBU_TTX_NON_SUB = 0x02,
    DATA_UNIT_ID_EBU_TTX_SUB,
    DATA_UNIT_ID_TTX_INVERTED    = 0xc0,
    DATA_UNIT_ID_VPS             = 0xc3,
    DATA_UNIT_ID_WSS,
    DATA_UNIT_ID_CEA_608,
    DATA_UNIT_ID_AMOL_48         = 0xd0,
    DATA_UNIT_ID_AMOL_96,
    DATA_UNIT_ID_NABTS           = 0xd5,
    DATA_UNIT_ID_TVG2X,
    DATA_UNIT_ID_CP,
    DATA_UNIT_ID_VITC            = 0xd9,
};

/* Convert DVB/SCTE data indentifier to OBE internal format */
const static int data_indentifier_table[][2] =
{
    { DATA_UNIT_ID_EBU_TTX_NON_SUB, MISC_TELETEXT },
    { DATA_UNIT_ID_EBU_TTX_SUB,     MISC_TELETEXT },
    { DATA_UNIT_ID_TTX_INVERTED,    MISC_TELETEXT_INVERTED },
    { DATA_UNIT_ID_VPS,             MISC_VPS },
    { DATA_UNIT_ID_WSS,             MISC_WSS },
    { DATA_UNIT_ID_CEA_608,         CAPTIONS_CEA_608 },
    { DATA_UNIT_ID_AMOL_48,         VBI_AMOL_48 },
    { DATA_UNIT_ID_AMOL_96,         VBI_AMOL_96 },
    { DATA_UNIT_ID_NABTS,           VBI_NABTS },
    { DATA_UNIT_ID_TVG2X,           VBI_TVG2X },
    { DATA_UNIT_ID_CP,              VBI_CP },
    { DATA_UNIT_ID_VITC,            VBI_VITC },
    { -1, -1 },
};

int setup_vbi_parser( obe_sdi_non_display_data_t *non_display_data );
int decode_vbi( obe_t *h, obe_sdi_non_display_data_t *non_display_data, uint8_t *lines, obe_raw_frame_t *raw_frame );
int decode_video_index_information( obe_t *h, obe_sdi_non_display_data_t *non_display_data, uint16_t *line, obe_raw_frame_t *raw_frame, int line_number );
int send_vbi_and_ttx( obe_t *h, obe_sdi_non_display_data_t *non_display_parser, int64_t pts );

#endif
