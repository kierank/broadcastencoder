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

int write_cc( obe_user_data_t *user_data, obe_int_input_stream_t *input_stream )
{
    bs_t q, r;
    uint8_t temp[1000];
    uint8_t temp2[500];
    const int country_code      = 0xb5;
    const int provider_code     = 0x31;
    const char *user_identifier = "GA94";
    const int data_type_code    = 0x03;
    int cc_count          = 0;
    const int echostar_captions = 0;

    /* TODO: when MPEG-2 is added make this do the right thing */
    /* FIXME: enable echostar captions and add more types */

    for( int i = 0; cc_timecode[i].timebase_num != 0; i++ )
    {
        if( input_stream->timebase_num == cc_timecode[i].timebase_num && input_stream->timebase_den == cc_timecode[i].timebase_den )
            cc_count = cc_timecode[i].cc_count;
    }

    if( !cc_count )
    {
        syslog( LOG_ERR, "[cc]: Unsupported framerate for captions\n" );
        return -1;
    }

    bs_init( &r, temp, 1000 );

    bs_write( &r,  8, country_code );  // itu_t_t35_country_code
    bs_write( &r, 16, provider_code ); // itu_t_t35_provider_code

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

        if( i <= 1 )
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
