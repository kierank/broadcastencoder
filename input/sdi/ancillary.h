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
#include "sdi.h"

typedef struct
{
    uint8_t did;
    uint8_t sdid;
    int type;
} obe_anc_identifier_t;

const static obe_anc_identifier_t vanc_identifiers[] =
{
    /* SMPTE 2016 */
    { 0x41, 0x05, MISC_AFD }, /* Includes Bar-Data */
    { 0x41, 0x06, MISC_PAN_SCAN },

    /* SMPTE 2010 */
    { 0x41, 0x07, VANC_SCTE_104 },

    /* SMPTE 2031 */
    { 0x41, 0x08, VANC_DVB_SCTE_VBI },

    /* OP-47 / SMPTE RDD-8 */
    { 0x43, 0x02, VANC_OP47_SDP },
    { 0x43, 0x03, VANC_OP47_MULTI_PACKET },

    /* SMPTE 12M */
    { 0x60, 0x60, VANC_ATC },

    /* SMPTE 334 */
    { 0x61, 0x01, CAPTIONS_CEA_708 },
    { 0x61, 0x02, CAPTIONS_CEA_608 },

    /* SMPTE RP-207 */
    { 0x62, 0x01, VANC_DTV_PROGRAM_DESCRIPTION },

    { 0x62, 0x02, VANC_DTV_DATA_BROADCAST },

    /* SMPTE RP-208 (legacy) */
    { 0x62, 0x03, VANC_SMPTE_VBI },

    { 0, 0, 0 },
};


int parse_vanc_line( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                     uint16_t *line, int width, int line_number );
int send_scte35( obe_t *h, obe_sdi_non_display_data_t *non_display_parser, int64_t pts, int64_t duration );
int decode_scte104( obe_sdi_non_display_data_t *non_display_data, uint8_t *scte104 );
#endif
