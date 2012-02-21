/*****************************************************************************
 * cc.c: caption encapsulation
 *****************************************************************************
 * Copyright (C) 2010-2011 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@ob-encoder.com>
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
#include "common/bitstream.h"
#include "common/bs_read.h"

#define CDP_IDENTIFIER 0x9669

enum cdp_section_id_e
{
    CDP_TC_SECTION_ID = 0x71,
    CDP_CC_DATA_SECTION_ID,
    CDP_CC_SVC_INFO_SECTION_ID,
    CDP_FOOTER_SECTION_ID,
};

typedef struct
{
    int timebase_den;
    int timebase_num;
    int mod;
    int cc_count;
    int frame_doubling;
} obe_cc_timecode_t;

/* PAL framerates with closed captions are unlikely */
obe_cc_timecode_t cc_timecode[] =
{
    { 24,    1,    24, 25, 0 },
    { 24000, 1001, 24, 25, 0 },
    { 25,    1,    25, 24, 0 },
    { 30000, 1001, 30, 20, 0 },
    { 30,    1,    30, 20, 0 },
    { 50,    1,    25, 12, 1 },
    { 60000, 1001, 30, 10, 1 },
    { 60,    1,    30, 10, 1 },
    { 0, 0, 0, 0 }
};

static void write_bytes( bs_t *s, uint8_t *bytes, int length )
{
    bs_flush( s );
    uint8_t *p_start = s->p_start;

    memcpy( s->p, bytes, length );
    s->p += length;

    bs_init( s, s->p, s->p_end - s->p );
    s->p_start = p_start;
}

static void write_invalid( bs_t *s )
{
    bs_write1( s, 0 );     // cc_valid
    bs_write( s, 2, 2 );   // cc_type
    bs_write( s, 8, 0 );   // c_data_1
    bs_write( s, 8, 0 );   // c_data_2
}

static void write_itu_t_codes( bs_t *s )
{
    const int country_code      = 0xb5;
    const int provider_code     = 0x31;

    bs_write( s,  8, country_code );  // itu_t_t35_country_code
    bs_write( s, 16, provider_code ); // itu_t_t35_provider_code
}

/* TODO: factor shared code out from 608 and 708 */
int write_608_cc( obe_user_data_t *user_data, obe_raw_frame_t *raw_frame )
{
    bs_t q, r;
    uint8_t temp[1000];
    uint8_t temp2[500];
    const char *user_identifier = "GA94";
    const int data_type_code    = 0x03;
    int cc_count                = 0;
    const int echostar_captions = 0;

    /* TODO: when MPEG-2 is added make this do the right thing */
    /* FIXME: enable echostar captions and add more types */

    for( int i = 0; cc_timecode[i].timebase_num != 0; i++ )
    {
        if( raw_frame->timebase_num == cc_timecode[i].timebase_num && raw_frame->timebase_den == cc_timecode[i].timebase_den )
            cc_count = cc_timecode[i].cc_count;
    }

    if( !cc_count )
    {
        syslog( LOG_ERR, "[cc]: Unsupported framerate for captions\n" );
        return -1;
    }

    bs_init( &r, temp, 1000 );

    /* N.B MPEG-4 only */
    write_itu_t_codes( &r );

    if( !echostar_captions )
    {
        for( int i = 0; i < 4; i++ )
            bs_write( &r, 8, user_identifier[i] ); // user_identifier
    }

    bs_write( &r, 8, data_type_code ); // user_data_type_code

    bs_init( &q, temp2, 500 );

    // user_data_type_structure (echostar)
    // cc_data
    bs_write1( &q, 1 );     // reserved
    bs_write1( &q, 1 );     // process_cc_data_flag
    bs_write1( &q, 0 );     // zero_bit / additional_data_flag
    bs_write( &q, 5, cc_count ); // cc_count
    bs_write( &q, 8, 0xff ); // reserved

    for( int i = 0; i < cc_count; i++ )
    {
        bs_write1( &q, 1 );     // one_bit
        bs_write( &q, 4, 0xf ); // reserved

        if( i <= 1 && user_data->len >= (i+1)*2 )
        {
            bs_write1( &q, 1 );   // cc_valid
            bs_write( &q, 2, i ); // cc_type
            bs_write( &q, 8, user_data->data[2*i] );   // c_data_1
            bs_write( &q, 8, user_data->data[2*i+1] ); // c_data_2
        }
        else /* nothing to write so maintain a constant bitrate */
            write_invalid( &q );
    }

    bs_write( &q, 8, 0xff ); // marker_bits
    bs_flush( &q );

    if( echostar_captions )
    {
        // ATSC1_data
        bs_write( &r, 8, bs_pos( &q ) >> 3 ); // user_data_code_length
    }

    write_bytes( &r, temp2, bs_pos( &q ) >> 3 );

    bs_flush( &r );

    user_data->type = USER_DATA_AVC_REGISTERED_ITU_T35;
    user_data->len = bs_pos( &r ) >> 3;

    free( user_data->data );

    user_data->data = malloc( user_data->len );
    if( !user_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    memcpy( user_data->data, temp, user_data->len );

    return 0;
}

static int write_708_cc( obe_user_data_t *user_data, uint8_t *start, int cc_count )
{
    bs_t s;
    uint8_t temp[1000];
    const char *user_identifier = "GA94";
    const int data_type_code    = 0x03;

    /* TODO: when MPEG-2 is added make this do the right thing */
    /* FIXME: enable echostar captions and add more types */

    bs_init( &s, temp, 1000 );

    /* N.B MPEG-4 only */
    write_itu_t_codes( &s );

    for( int i = 0; i < 4; i++ )
        bs_write( &s, 8, user_identifier[i] ); // user_identifier

    bs_write( &s, 8, data_type_code ); // user_data_type_code

    // user_data_type_structure (echostar)
    // cc_data
    bs_write1( &s, 1 );     // reserved
    bs_write1( &s, 1 );     // process_cc_data_flag
    bs_write1( &s, 0 );     // zero_bit / additional_data_flag
    bs_write( &s, 5, cc_count ); // cc_count
    bs_write( &s, 8, 0xff ); // reserved

    write_bytes( &s, start, cc_count*3 );

    bs_write( &s, 8, 0xff ); // marker_bits
    bs_flush( &s );

    user_data->type = USER_DATA_AVC_REGISTERED_ITU_T35;
    user_data->len = bs_pos( &s ) >> 3;

    free( user_data->data );

    user_data->data = malloc( user_data->len );
    if( !user_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    memcpy( user_data->data, temp, user_data->len );

    return 0;
}

int write_cdp( obe_user_data_t *user_data )
{
    uint8_t *start = NULL;
    int cc_count = 0;
    bs_read_t s;
    bs_read_init( &s, user_data->data, user_data->len );

    // cdp_header
    if( bs_read( &s, 16 ) != CDP_IDENTIFIER )
        return 0;

    bs_skip( &s, 8 ); // cdp_length
    bs_skip( &s, 4 ); // cdp_frame_rate
    bs_skip( &s, 4 ); // reserved
    bs_skip( &s, 1 ); // time_code_present
    bs_skip( &s, 1 ); // ccdata_present
    bs_skip( &s, 1 ); // svcinfo_present
    bs_skip( &s, 1 ); // svc_info_start
    bs_skip( &s, 1 ); // svc_info_change
    bs_skip( &s, 1 ); // svc_info_complete

    /* caption_service_active seemingly unreliable */
    bs_skip( &s, 1 ); // caption_service_active
    bs_skip( &s, 1 ); // reserved
    bs_skip( &s, 16 ); // cdp_hdr_sequence_cntr

    while( !bs_read_eof( &s ) )
    {
        uint8_t section_id = bs_read( &s, 8 );
        if( section_id == CDP_TC_SECTION_ID )
        {
            /* Is this timecode guaranteed to match VITC? */
            bs_skip( &s, 2 ); // reserved
            bs_skip( &s, 2 ); // tc_10hrs
            bs_skip( &s, 4 ); // tc_1hrs
            bs_skip( &s, 1 ); // reserved
            bs_skip( &s, 3 ); // tc_10min
            bs_skip( &s, 4 ); // tc_1min
            bs_skip( &s, 1 ); // tc_field_flag
            bs_skip( &s, 3 ); // tc_10sec
            bs_skip( &s, 4 ); // tc_1sec
            bs_skip( &s, 1 ); // drop_frame_flag
            bs_skip( &s, 1 ); // zero
            bs_skip( &s, 2 ); // tc_10fr
            bs_skip( &s, 4 ); // tc_1fr
        }
        else if( section_id == CDP_CC_DATA_SECTION_ID )
        {
            cc_count = bs_read( &s, 8 ) & 0x1f;
            start = &user_data->data[bs_read_pos( &s ) / 8];
            for( int i = 0; i < cc_count; i++ )
            {
                bs_skip( &s, 5 ); // marker_bits
                bs_skip( &s, 1 ); // cc_valid
                bs_skip( &s, 2 ); // cc_type
                bs_skip( &s, 8 ); // cc_data_1
                bs_skip( &s, 8 ); // cc_data_2
            }
        }
        else if( section_id == CDP_CC_SVC_INFO_SECTION_ID )
        {
            /* TODO: pass this to muxer when user requests */
            bs_skip( &s, 1 ); // reserved
            bs_skip( &s, 1 ); // svc_info_start
            bs_skip( &s, 1 ); // svc_info_change
            bs_skip( &s, 1 ); // svc_info_complete
            int svc_count = bs_read( &s, 4 );
            for( int i = 0; i < svc_count; i++ )
            {
                bs_skip( &s, 1 ); // reserved
                bs_skip( &s, 1 ); // csn_size
                bs_skip( &s, 6 ); // caption_service_number (note: csn_size branch in spec)
                bs_skip( &s, 8 ); // svc_data_byte_1
                bs_skip( &s, 8 ); // svc_data_byte_2
                bs_skip( &s, 8 ); // svc_data_byte_3
                bs_skip( &s, 8 ); // svc_data_byte_4
                bs_skip( &s, 8 ); // svc_data_byte_5
                bs_skip( &s, 8 ); // svc_data_byte_6
            }
        }
        else if( section_id == CDP_FOOTER_SECTION_ID )
        {
            /* TODO: use these for packet verification? */
            bs_skip( &s, 16 ); // cdp_ftr_sequence_cntr
            bs_skip( &s, 8 );  // packet_checksum
            break;
        }
        else // future_section
        {
            int future_len = bs_read( &s, 8 );
            for( int i = 0; i < future_len; i++ )
                bs_skip( &s, 8 );
        }
    }

    if( !cc_count )
        return 0;

    if( write_708_cc( user_data, start, cc_count ) < 0 )
        return -1;

    return 0;
}
