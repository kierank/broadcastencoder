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

#define VIDEO_INDEX_CRC_POLY 0x1D
#define VIDEO_INDEX_CRC_POLY_BROKEN 0x1C

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

static int parse_afd( obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame, uint16_t *line, int line_number )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;

    /* Skip DC word
     * FIXME: should we skip a packet with the wrong length */
    line++;

    /* TODO: make Bar Data optional */

    /* AFD is duplicated on the second field so skip it if we've already detected it */
    if( non_display_data->probe )
    {
        for( int i = 0; i < non_display_data->num_frame_data; i++ )
        {
            if( non_display_data->frame_data[i].type == MISC_AFD )
                return 0;
        }

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+2) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data];
        non_display_data->num_frame_data += 2;

        /* AFD */
        frame_data->type = MISC_AFD;
        frame_data->source = VANC_GENERIC;
        frame_data->line_number = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        /* Bar data */
        frame_data++;
        frame_data->type = MISC_BAR_DATA;
        frame_data->source = VANC_GENERIC;
        frame_data->line_number = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        return 0;
    }

    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        if( raw_frame->user_data[i].type == USER_DATA_AFD )
            return 0;
    }

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

    /* Skip two reserved lines */
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

static int parse_dvb_scte_vbi( obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame, uint16_t *line, int line_number )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_anc_vbi_t *anc_vbi;
    int i, data_unit_id;

    int len = READ_8( line[0] );

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

    if( non_display_data->probe )
    {
        /* Don't duplicate VBI streams */
        for( int i = 0; i < non_display_data->num_frame_data; i++ )
        {
            if( non_display_data->frame_data[i].type == data_indentifier_table[i][1] )
                return 0;
        }

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = data_indentifier_table[i][1];
        frame_data->source = VANC_DVB_SCTE_VBI;
        frame_data->line_number = line_number;
        frame_data->location = USER_DATA_LOCATION_DVB_STREAM;

        return 0;
    }

    /* TODO: pass through data identifier */
    line++;

    anc_vbi = &non_display_data->anc_vbi[non_display_data->num_anc_vbi++];
    anc_vbi->len = len - 3;
    anc_vbi->unit_id = data_unit_id;
    anc_vbi->data = malloc( anc_vbi->len );
    if( !anc_vbi->data )
        goto fail;

    for( i = 0; i < anc_vbi->len; i++ )
        anc_vbi->data[i] = READ_8( line[i] );

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

static int parse_cdp( obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame, uint16_t *line, int line_number )
{
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;
    int len = READ_8( line[0] );

    /* Skip DC word */
    line++;

    /* Don't duplicate caption data */
    if( non_display_data->probe )
    {
        for( int i = 0; i < non_display_data->num_frame_data; i++ )
        {
            if( non_display_data->frame_data[i].type == CAPTIONS_CEA_708 )
                return 0;
        }

        tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
        if( !tmp )
            goto fail;

        non_display_data->frame_data = tmp;

        frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
        frame_data->type = CAPTIONS_CEA_708;
        frame_data->source = VANC_GENERIC;
        frame_data->line_number = line_number;
        frame_data->location = USER_DATA_LOCATION_FRAME;

        return 0;
    }

    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        if( raw_frame->user_data[i].type == USER_DATA_CEA_708_CDP )
            return 0;
    }

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

int parse_vanc_line( obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame, uint16_t *line, int width,
                     int line_number )
{
    int i = 0;

    /* VANC can be in luma or chroma */
    width <<= 1;

    /* TODO: optimise this */
    while( i < width - 5 )
    {
        if( line[i] <= 0x03 && (line[i+1] & 0x3fc) == 0x3fc && (line[i+2] & 0x3fc) == 0x3fc )
        {
            i += 3;
            switch ( get_vanc_type( READ_8( line[i] ), READ_8( line[i+1] ) ) )
            {
                case MISC_AFD:
                    parse_afd( non_display_data, raw_frame, &line[i+2], line_number );
                    break;
                case VANC_DVB_SCTE_VBI:
                    parse_dvb_scte_vbi( non_display_data, raw_frame, &line[i+2], line_number );
                    break;
                case CAPTIONS_CEA_708:
                    parse_cdp( non_display_data, raw_frame, &line[i+2], line_number );
                    break;
                default:
                    break;
            }
            /* skip user data words and checksum */
            i += READ_8( line[i+2] ) + 1;
        }
        else
            i++;
    }

    /* FIXME: should we probe more frames? */
    if( non_display_data->probe )
        non_display_data->has_probed = 1;

    return 0;
}
