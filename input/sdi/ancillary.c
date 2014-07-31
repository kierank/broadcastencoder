/*****************************************************************************
 * ancillary.c: OBE ancillary data parsing functions
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

#include "common/common.h"
#include "ancillary.h"
#include "sdi.h"
#include "vbi.h"

#define READ_8(x) ((x) & 0xff)

static int get_vanc_type( uint8_t did, uint8_t sdid )
{
    for( int i = 0; vanc_identifiers[i].did != 0; i++ )
    {
        if( did == vanc_identifiers[i].did && sdid == vanc_identifiers[i].sdid )
            return vanc_identifiers[i].type;
    }

    return -1;
}

/* TODO/FIXME: check parity, ideally using x86's PF
 *             is it possible to check parity but follow 8-bit backwards compatibility? */

static int parse_afd( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                      uint16_t *line, int line_number, int len )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;

    if( READ_8( line[0] ) != 8 )
    {
        syslog( LOG_ERR, "Skipping AFD in VANC on line %d - incorrect DC word\n", line_number );
        return -1;
    }

    /* Skip DC word */
    line++;

    /* TODO: make Bar Data optional */

    /* AFD is duplicated on the second field so skip it if we've already detected it */
    if( non_display_data->probe )
    {
        /* TODO: mention existence of second line of AFD? */
        if( check_probed_non_display_data( non_display_data, MISC_AFD ) )
            return 0;

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+2) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data];
        non_display_data->num_frame_data += 2;

        /* AFD */
        frame_data->type = MISC_AFD;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        /* Bar data */
        frame_data++;
        frame_data->type = MISC_BAR_DATA;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        return 0;
    }

    /* Return if user didn't select AFD */
    if( !check_user_selected_non_display_data( h, MISC_AFD, USER_DATA_LOCATION_FRAME ) )
        return 0;

    /* Return if AFD already exists in frame */
    if( check_active_non_display_data( raw_frame, USER_DATA_AFD ) )
        return 0;

    tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+2) * sizeof(*raw_frame->user_data) );
    if( !tmp2 )
        goto fail;

    raw_frame->user_data = tmp2;
    user_data = &raw_frame->user_data[raw_frame->num_user_data];
    raw_frame->num_user_data += 2;

    /* Read AFD */
    user_data->len = 1;
    user_data->type = USER_DATA_AFD;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len );
    if( !user_data->data )
        goto fail;

    user_data->data[0] = READ_8( line[0] );

    /* Skip two reserved words */
    line += 2;
    user_data++;

    /* Read Bar Data */
    user_data->len = 5;
    user_data->type = USER_DATA_BAR_DATA;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len );
    if( !user_data->data )
        goto fail;

    for( int i = 0; i < user_data->len; i++)
        user_data->data[i] = READ_8( line[i] );

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

static int parse_dvb_scte_vbi( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                               uint16_t *line, int line_number, int len )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_anc_vbi_t *anc_vbi;
    int i, data_unit_id;

    /* Skip DC word */
    line++;

    data_unit_id = READ_8( line[1] );

    /* TODO: decide what we should do with these rare cases. Do we place in DVB-VBI or put in user-data? */
    if( data_unit_id == DATA_UNIT_ID_CEA_608 || data_unit_id == DATA_UNIT_ID_VITC )
        return 0;

    for( i = 0; data_indentifier_table[i][0] != -1; i++ )
    {
        if( data_unit_id == data_indentifier_table[i][0] )
            break;
    }

    /* TODO: allow user-defined modes */
    if( data_indentifier_table[i][0] == -1 )
        return 0;

    /* FIXME: disabled for the time being */
    return 0;

    if( non_display_data->probe )
    {
        /* Don't duplicate VBI streams */
        /* TODO: mention existence of multiple lines of VBI? */
        if( check_probed_non_display_data( non_display_data, data_indentifier_table[i][1] ) )
            return 0;

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = data_indentifier_table[i][1];
        frame_data->source = VANC_DVB_SCTE_VBI;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = line_number;
        frame_data->location = USER_DATA_LOCATION_DVB_STREAM;

        return 0;
    }

    /* TODO: verify line number of VBI */

    anc_vbi = &non_display_data->anc_vbi[non_display_data->num_anc_vbi++];
    anc_vbi->identifier = READ_8( line[0] );
    anc_vbi->unit_id = data_unit_id;
    anc_vbi->len = READ_8( line[1] );
    anc_vbi->data = malloc( anc_vbi->len );
    if( !anc_vbi->data )
        goto fail;

    line += 2;

    for( i = 0; i < anc_vbi->len; i++ )
        anc_vbi->data[i] = READ_8( line[i] );

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

static int parse_cdp( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                      uint16_t *line, int line_number, int len )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;

    /* Skip DC word */
    line++;

    if( non_display_data->probe )
    {
        if( check_probed_non_display_data( non_display_data, CAPTIONS_CEA_708 ) )
            return 0;

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = CAPTIONS_CEA_708;
        frame_data->source = VANC_GENERIC;
        frame_data->num_lines = 0;
        frame_data->lines[frame_data->num_lines++] = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        return 0;
    }

    /* Return if user didn't select CEA-708 */
    if( !check_user_selected_non_display_data( h, CAPTIONS_CEA_708, USER_DATA_LOCATION_FRAME ) )
        return 0;

    /* Return if there is already CEA-708 data in the frame */
    if( check_active_non_display_data( raw_frame, USER_DATA_CEA_708_CDP ) )
        return 0;

    tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
    if( !tmp2 )
        goto fail;

    raw_frame->user_data = tmp2;
    user_data = &raw_frame->user_data[raw_frame->num_user_data++];
    user_data->len = len;
    user_data->type = USER_DATA_CEA_708_CDP;
    user_data->source = VANC_GENERIC;
    user_data->data = malloc( user_data->len );
    if( !user_data->data )
        goto fail;

    for( int i = 0; i < user_data->len; i++ )
        user_data->data[i] = READ_8( line[i] );

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

#if 0
static int parse_cea_608( uint16_t *line )
{

}
#endif

int parse_vanc_line( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                     uint16_t *line, int width, int line_number )
{
    int i = 0, j;
    uint16_t vanc_checksum, *pkt_start;

    /* VANC can be in luma or chroma */
    width <<= 1;

    /* The smallest VANC data length is 7 words long (ADF + SDID + DID + DC + CS)
     * TODO: optimise this */
    while( i < width - 7 )
    {
        if( line[i] <= 0x04 && line[i+1] >= 0x3fb && line[i+2] >= 0x3fb )
        {
            i += 3;
            pkt_start = &line[i];
            int len = READ_8( pkt_start[2] );
            vanc_checksum = 0;

            if( (len+2) > (width - i - 1) )
            {
                syslog( LOG_ERR, "VANC packet length too large on line %i \n", line_number );
                break;
            }

            /* Checksum includes DC, DID and SDID/DBN */
            for( j = 0; j < len+3; j++ )
            {
                vanc_checksum += pkt_start[j] & 0x1ff;
                vanc_checksum &= 0x1ff;
            }

            vanc_checksum |= (~vanc_checksum & 0x100) << 1;

            if( pkt_start[j] == vanc_checksum )
            {
                /* Pass the DC word to the parsing function because some parsers may want to sanity check the length */
                switch ( get_vanc_type( READ_8( pkt_start[0] ), READ_8( pkt_start[1] ) ) )
                {
                    case MISC_AFD:
                        parse_afd( h, non_display_data, raw_frame, &pkt_start[2], line_number, len );
                        break;
                    case VANC_DVB_SCTE_VBI:
                        parse_dvb_scte_vbi( h, non_display_data, raw_frame, &pkt_start[2], line_number, len );
                        break;
                    case CAPTIONS_CEA_708:
                        parse_cdp( h, non_display_data, raw_frame, &pkt_start[2], line_number, len );
                        break;
                    default:
                        break;
                }
            }
            else
                syslog( LOG_ERR, "Invalid VANC checksum on line %i \n", line_number );

            /* skip DID, DBN/SDID, user data words and checksum */
            i += 2 + len + 1;
        }
        else
            i++;
    }

    /* FIXME: should we probe more frames? */
    if( non_display_data->probe )
        non_display_data->has_probed = 1;

    return 0;
}
