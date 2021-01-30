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

#include <bitstream/scte/35.h>
#include <bitstream/scte/104.h>

#define SDP_IDENT1 0x51
#define SDP_IDENT2 0x15
#define SDP_DATA_WORDS 40

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

        non_display_data->has_ttx_frame = 1;

        return 0;
    }

    non_display_data->has_ttx_frame = !!get_output_stream_by_format( h, MISC_TELETEXT );

    if( !non_display_data->has_ttx_frame )
        return 0;

    /* TODO: verify line number of VBI */
    anc_vbi = &non_display_data->anc_vbi[non_display_data->num_anc_vbi++];
    anc_vbi->identifier = READ_8( line[0] );
    anc_vbi->unit_id = data_unit_id;
    anc_vbi->len = READ_8( line[2] );
    anc_vbi->data = malloc( anc_vbi->len );
    if( !anc_vbi->data )
        goto fail;

    line += 3;

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

static void read_op47_structure_b( vbi_sliced *vbi_slice, uint16_t *line, uint8_t dw )
{
    int parity = dw >> 7;
    int line_analogue = dw & 0x1f;
    int line_smpte = 0;

    obe_convert_analogue_to_smpte( INPUT_VIDEO_FORMAT_PAL, line_analogue, parity == 0 ? 2 : 1, &line_smpte );
    vbi_slice->id = VBI_SLICED_TELETEXT_B;
    vbi_slice->line = line_smpte;

    /* skip run in codes and framing code */
    line += 3;

    for( int i = 0; i < SDP_DATA_WORDS+2; i++ )
        vbi_slice->data[i] = READ_8( line[i] ); // MRAG x2 + data_block
}

static int parse_op47_sdp( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                            uint16_t *line, int line_number, int len )
{
    uint8_t sdp_cs = 0, dw[5];
    obe_int_frame_data_t *tmp, *frame_data;

    /* Skip DC word */
    line++;

    if( READ_8( line[0] ) == SDP_IDENT1 && READ_8( line[1] ) == SDP_IDENT2 )
    {
        uint8_t sdp_len = READ_8( line[2] );
        if( sdp_len > len )
        {
            syslog( LOG_ERR, "Invalid OP47 SDP length on line %i \n", line_number );
            return -1;
        }

        /* Calculate checksum */
        for( int i = 0; i < len; i++ )
            sdp_cs += READ_8( line[i] );

        if( sdp_cs )
        {
            syslog( LOG_ERR, "Invalid OP47 SDP checksum on line %i \n", line_number );
            return -1;
        }

        non_display_data->has_ttx_frame = 1;

        /* Consider the SDP acceptable if it has a valid checksum */
        if( non_display_data->probe )
        {
            if( check_probed_non_display_data( non_display_data, MISC_TELETEXT ) )
                return 0;

            tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
            if( !tmp )
                return -1;

            non_display_data->frame_data = tmp;

            frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
            frame_data->type = MISC_TELETEXT;
            frame_data->source = VANC_OP47_SDP;
            frame_data->num_lines = 0;
            frame_data->lines[frame_data->num_lines++] = line_number;
            frame_data->location = USER_DATA_LOCATION_DVB_STREAM;

            return 0;
        }

        if( !get_output_stream_by_format( h, MISC_TELETEXT ) )
            return 0;

        line += 3; // skip identifier and sdp length
        if( READ_8( line[0] ) == 0x2 )
        {

            line++; // skip format code

            for( int i = 0; i < 5; i++ )
                dw[i] = READ_8( line[i] );

            line += 5;

            for( int i = 0; i < 5; i++ )
            {
                if( dw[i] )
                {
                    read_op47_structure_b( &non_display_data->vbi_slices[non_display_data->num_vbi++], line, dw[i] );
                    line += SDP_DATA_WORDS+5;
                }
            }

            if( READ_8( line[0] ) != 0x74 )
                syslog( LOG_ERR, "Invalid OP47 footer on line %i \n", line_number );
        }
    }

    return 0;
}

int decode_scte104( obe_sdi_non_display_data_t *non_display_data, uint8_t *scte104 )
{
    int num_ops = scte104m_get_num_ops( scte104 );
    uint8_t *scte35 = NULL, *scte35_start = NULL, *scte35_desc = NULL;
    int desc_len = 0, size = 0;
    int64_t mod = (int64_t)1 << 33;

    non_display_data->scte35_frame = new_coded_frame( 0, PSI_MAX_SIZE + PSI_HEADER_SIZE );
    if( !non_display_data->scte35_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    scte35 = scte35_start = non_display_data->scte35_frame->data;

    for( int i = 0; i < num_ops; i++ )
    {
        uint8_t *op = scte104m_get_op( scte104, i );

        if( scte104o_get_opid( op ) == SCTE104_OPID_SPLICE_NULL )
        {
            scte35_null_init( scte35 );
            scte35_set_pts_adjustment(scte35, 0);
        }
        else if( scte104o_get_opid( op ) == SCTE104_OPID_SPLICE )
        {
            op = scte104o_get_data( op );
            uint8_t insert_type = scte104srd_get_insert_type( op );
            uint32_t event_id = scte104srd_get_event_id( op );
            uint16_t unique_program_id = scte104srd_get_unique_program_id( op );
            uint64_t pre_roll_time = scte104srd_get_pre_roll_time( op );
            uint64_t duration = scte104srd_get_break_duration( op );
            uint8_t avail_num = scte104srd_get_avail_num( op );
            uint8_t avails_expected = scte104srd_get_avails_expected( op );
            uint8_t auto_return = scte104srd_get_auto_return( op );
            uint8_t splice_immediate_flag = insert_type == SCTE104SRD_START_IMMEDIATE || insert_type == SCTE104SRD_END_IMMEDIATE;

            if( insert_type == SCTE104SRD_RESERVED )
                continue;

            if( insert_type != SCTE104SRD_CANCEL )
            {
                size += SCTE35_INSERT_HEADER2_SIZE + SCTE35_INSERT_FOOTER_SIZE;

                if( (insert_type == SCTE104SRD_START_NORMAL || insert_type == SCTE104SRD_END_NORMAL) &&
                    pre_roll_time > 0 )
                {
                    splice_immediate_flag = 0;
                    size += SCTE35_SPLICE_TIME_TIME_SIZE + SCTE35_SPLICE_TIME_HEADER_SIZE;
                }

                if( insert_type == SCTE104SRD_START_NORMAL || insert_type == SCTE104SRD_START_IMMEDIATE )
                    size += SCTE35_BREAK_DURATION_HEADER_SIZE;
                else
                    duration = 0;
            }

            scte35_insert_init( scte35, size );
            scte35_set_pts_adjustment(scte35, 0);
            scte35_insert_set_cancel( scte35, insert_type == SCTE104SRD_CANCEL );
            scte35_insert_set_event_id( scte35, event_id );
            if( insert_type != SCTE104SRD_CANCEL )
            {
                const int oon = insert_type == SCTE104SRD_START_NORMAL || insert_type == SCTE104SRD_START_IMMEDIATE;
                scte35_insert_set_out_of_network( scte35, oon );
                scte35_insert_set_program_splice( scte35, 1 );
                scte35_insert_set_duration( scte35, !!duration );
                scte35_insert_set_splice_immediate( scte35, splice_immediate_flag );

                if( !splice_immediate_flag )
                {
                    uint8_t *splice_time = scte35_insert_get_splice_time( scte35 );

                    scte35_splice_time_init( splice_time );
                    scte35_splice_time_set_time_specified( splice_time, 1 );
                    scte35_splice_time_set_pts_time( splice_time, (pre_roll_time * 90) % mod );
                }

                if( duration )
                {
                    uint8_t *break_duration = scte35_insert_get_break_duration( scte35 );
                    scte35_break_duration_init( break_duration );
                    scte35_break_duration_set_auto_return( break_duration, auto_return );
                    scte35_break_duration_set_duration( break_duration, (duration * 9000) % mod );
                }

                scte35_insert_set_unique_program_id( scte35, unique_program_id );
                scte35_insert_set_avail_num( scte35, avail_num );
                scte35_insert_set_avails_expected( scte35, avails_expected );

                syslog(LOG_WARNING, "[SCTE-104] Splice Insert. %s, Preroll (90kHz) %"PRIu64" Duration (90kHz) %"PRIu64"", oon ? "Out" : "In", (pre_roll_time * 90) % mod, (duration * 9000) % mod);
            }
        }
        else if( scte104o_get_opid( op ) == SCTE104_OPID_TIME_SIGNAL )
        {
            op = scte104o_get_data( op );
            uint16_t pre_roll_time = scte104ts_get_pre_roll_time( op );

            scte35_time_signal_init( scte35, SCTE35_SPLICE_TIME_TIME_SIZE );
            scte35 = scte35_time_signal_get_splice_time( scte35 );
            scte35_splice_time_init( scte35 );
            scte35_splice_time_set_time_specified( scte35, 1 );
            scte35_splice_time_set_pts_time( scte35, (pre_roll_time * 90) % mod );
            syslog(LOG_WARNING, "[SCTE-104] Time signal. Pre Roll %"PRIu64" (90kHz)", (pre_roll_time * 90) % mod);
        }
        else if( scte104o_get_opid( op ) == SCTE104_OPID_INSERT_SD )
        {
            op = scte104o_get_data( op );
            uint32_t segmentation_event_id = scte104isd_get_segmentation_event_id( op );
            uint8_t segmentation_event_cancel_indicator = scte104isd_get_segmentation_event_cancel_indicator( op );
            uint16_t duration = scte104isd_get_duration( op );
            uint8_t segmentation_upid_type = scte104isd_get_segmentation_upid_type( op );
            uint8_t segmentation_upid_length = scte104isd_get_segmentation_upid_length( op );
            uint8_t *segmentation_upid = scte104isd_get_segmentation_upid( op );

            op = segmentation_upid + segmentation_upid_length;
            uint8_t segment_type_id = scte104isd_get_segment_type_id( op );
            uint8_t segment_num = scte104isd_get_segment_num( op );
            uint8_t segments_expected = scte104isd_get_segments_expected( op );
            uint8_t duration_extension_frames = scte104isd_get_duration_extension_frames( op );
            uint8_t delivery_not_restricted_flag = scte104isd_get_delivery_not_restricted_flag( op );
            uint8_t web_delivery_allowed_flag = scte104isd_get_web_delivery_allowed_flag( op );
            uint8_t no_regional_blackout_flag = scte104isd_get_no_regional_blackout_flag( op );
            uint8_t archive_allowed_flag = scte104isd_get_archive_allowed_flag( op );
            uint8_t device_restrictions = scte104isd_get_device_restrictions( op );
            uint8_t insert_sub_segment_info = scte104isd_get_insert_sub_segment_info( op );
            uint8_t sub_segment_num = scte104isd_get_sub_segment_num( op );
            uint8_t sub_segments_expected = scte104isd_get_sub_segment_num( op );

            if( !scte35_desc )
                scte35_desc = scte35_get_descl( scte35_start );

            uint8_t *scte35_desc_start = scte35_desc;

            scte35sd_init( scte35_desc );
            scte35sd_set_splice_descriptor_tag( scte35_desc, SCTE35_SD_TAG );
            scte35sd_set_identifier( scte35_desc, SCTE35_SD_IDENTIFIER );
            scte35sd_set_segmentation_event_id( scte35_desc, segmentation_event_id );
            scte35sd_set_segmentation_event_cancel_indicator( scte35_desc, segmentation_event_cancel_indicator);
            if( segmentation_event_cancel_indicator == 0 )
            {
                scte35sd_set_program_segmentation_flag( scte35_desc, 1);
                scte35sd_set_segmentation_duration_flag( scte35_desc, !!duration );
                scte35sd_set_delivery_not_restricted_flag( scte35_desc, delivery_not_restricted_flag );
                scte35sd_init_delivery_not_restricted( scte35_desc );
                if( delivery_not_restricted_flag == 0 )
                {
                    scte35sd_set_web_delivery_allowed_flag( scte35_desc, web_delivery_allowed_flag );
                    scte35sd_set_no_regional_blackout_flag( scte35_desc, no_regional_blackout_flag );
                    scte35sd_set_archive_allowed_flag( scte35_desc, archive_allowed_flag );
                    scte35sd_set_device_restrictions( scte35_desc, device_restrictions );
                }

                /* program_segmentation_flag == 1 for this message */
                scte35_desc += SCTE35_SD_HEADER_SIZE + 1;

                if( duration )
                {
                    /* XXX: Assumes 29.97fps */
                    scte35sd_set_segmentation_duration( scte35_desc, duration * 90000 + duration_extension_frames * 3003 );
                    scte35_desc += 5;
                }
                scte35sd_set_segmentation_upid_type( scte35_desc, segmentation_upid_type );
                scte35sd_set_segmentation_upid_length( scte35_desc, segmentation_upid_length );
                memcpy( scte35_desc + 2, segmentation_upid, segmentation_upid_length );
                scte35_desc += 2 + segmentation_upid_length;
                scte35sd_set_segmentation_type_id( scte35_desc, segment_type_id );
                scte35sd_set_segment_num( scte35_desc, segment_num );
                scte35sd_set_segments_expected( scte35_desc, segments_expected );
                scte35_desc += 3;

                if( insert_sub_segment_info )
                {
                    scte35sd_set_sub_segment_num( scte35_desc, sub_segment_num );
                    scte35sd_set_sub_segments_expected( scte35_desc, sub_segments_expected );
                    scte35_desc += 2;
                }

                syslog(LOG_WARNING, "[SCTE-104] Insert Segmentation Descriptor. Duration %u (90kHz)", duration * 90000 + duration_extension_frames * 3003);
            }
            scte35sd_set_descriptor_length( scte35_desc_start, scte35_desc - scte35_desc_start - 2 );

            desc_len += scte35_desc - scte35_desc_start;
        }
    }

    scte35_set_desclength( scte35_start, desc_len );
    psi_set_length( scte35_start, desc_len + scte35_get_descl( scte35_start ) + PSI_CRC_SIZE - scte35_start - PSI_HEADER_SIZE );
    psi_set_crc( scte35_start );
    non_display_data->scte35_frame->len = psi_get_length( scte35_start ) + PSI_HEADER_SIZE;

    return 0;
}

static int parse_scte104_vanc( obe_t *h, obe_sdi_non_display_data_t *non_display_data, obe_raw_frame_t *raw_frame,
                               uint16_t *line, int line_number, int len )
{
    uint8_t scte104[1920];
    int dc;

    dc = READ_8( line[0] );
    line++;

    if( !get_output_stream_by_format( h, MISC_SCTE35 ) )
        return 0;

    /* Only single line VANC supported */
    if( READ_8( line[0] ) == 0x8 )
    {
        line++;
        for( int i = 0; i < dc; i++ )
            scte104[i] = READ_8( line[i] );

        if( scte104m_validate( scte104 ) && scte104_get_opid( scte104 ) == SCTE104_OPID_MULTIPLE )
        {
            if( decode_scte104( non_display_data, scte104 ) < 0 )
                return -1;
        }
    }

    return 0;
}

/* FIXME: duplicated with netmap */
static int encap_custom_vanc( obe_t *h, obe_sdi_non_display_data_t *non_display_data, uint16_t *line, int line_number, int len,
                              uint8_t did, uint8_t sdid, int offset )
{
    int dc;

    dc = READ_8( line[0] );
    line++;

    obe_output_stream_t *output_stream = get_output_stream_by_format( h, ANC_RAW );
    if( !output_stream )
        return 0;

    obe_coded_frame_t *anc_frame = non_display_data->anc_frame;
    if( !anc_frame )
    {
        non_display_data->anc_frame = anc_frame = new_coded_frame( output_stream->output_stream_id, 65536 );
        if( !anc_frame )
            return -1;

        anc_frame->len = 0;
    }

    bs_t s;
    bs_init(&s, &anc_frame->data[anc_frame->len], 65536 - anc_frame->len);

    bs_write(&s, 6, 0);
    bs_write(&s, 1, 1); /* FIXME: SD */
    bs_write(&s, 11, line_number);
    bs_write(&s, 12, offset);
    bs_write(&s, 10, did);
    bs_write(&s, 10, sdid);
    bs_write(&s, 10, dc);

    for (int i = 0; i < (dc & 0xff) + 1 /* CS */; i++) {
        bs_write(&s, 10, line[i]);
    }

    int pos = bs_pos(&s) & 7;
    if (pos)
        bs_write(&s, 8 - pos, 0xff);

    bs_flush(&s);

    anc_frame->len += bs_pos(&s) / 8;

    return 0;
}

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
        if( (line[i] >> 2) == 0 && (line[i+1] >> 2) == 0xff && (line[i+2] >> 2) == 0xff )
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
                uint8_t did, sdid;
                did = READ_8( pkt_start[0] );
                sdid = READ_8( pkt_start[1] );
                /* Pass the DC word to the parsing function because some parsers may want to sanity check the length */
                switch ( get_vanc_type( did, sdid ) )
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
                    case VANC_OP47_SDP:
                        parse_op47_sdp( h, non_display_data, raw_frame, &pkt_start[2], line_number, len );
                        break;
                    case VANC_SCTE_104:
                        parse_scte104_vanc( h, non_display_data, raw_frame, &pkt_start[2], line_number, len );
                        break;
                    default:
                        encap_custom_vanc( h, non_display_data, &pkt_start[2], line_number, len, did, sdid, i );
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

int send_scte35( obe_t *h, obe_sdi_non_display_data_t *non_display_parser, int64_t pts, int64_t duration )
{
    obe_output_stream_t *output_stream;

    if( non_display_parser->scte35_frame )
    {
        output_stream = get_output_stream_by_format( h, MISC_SCTE35 );

        if( output_stream && output_stream->output_stream_id >= 0 )
        {
            non_display_parser->scte35_frame->output_stream_id = output_stream->output_stream_id;
            non_display_parser->scte35_frame->pts = pts;
            non_display_parser->scte35_frame->duration = duration;

            if( add_to_queue( &h->mux_queue, &non_display_parser->scte35_frame->uchain ) < 0 )
            {
                destroy_coded_frame( non_display_parser->scte35_frame );
                return -1;
            }
        }

        non_display_parser->scte35_frame = NULL;
    }

    return 0;
}

int send_anc( obe_t *h, obe_sdi_non_display_data_t *non_display_parser, int64_t pts, int64_t duration )
{
    obe_output_stream_t *output_stream;

    if( non_display_parser->anc_frame )
    {
        output_stream = get_output_stream_by_format( h, ANC_RAW );

        if( output_stream && output_stream->output_stream_id >= 0 )
        {
            non_display_parser->anc_frame->output_stream_id = output_stream->output_stream_id;
            non_display_parser->anc_frame->pts = pts;
            non_display_parser->anc_frame->duration = duration;

            if( add_to_queue( &h->mux_queue, &non_display_parser->anc_frame->uchain ) < 0 )
            {
                destroy_coded_frame( non_display_parser->anc_frame );
                return -1;
            }
        }

        non_display_parser->anc_frame = NULL;
    }

    return 0;
}