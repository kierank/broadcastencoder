/*****************************************************************************
 * vbi.c: OBE VBI parsing functions
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
#include "sdi.h"
#include "vbi.h"
#include <libavutil/common.h>
#include "common/bitstream.h"

#define VIDEO_INDEX_CRC_POLY 0x1d
#define VIDEO_INDEX_CRC_POLY_BROKEN 0x1c

#define DVB_VBI_MAXIMUM_SIZE      65536
#define DVB_VBI_UNIT_SIZE         44
#define DVB_VBI_PES_HEADER_SIZE   45

#define DVB_VBI_DATA_IDENTIFIER   0x10
#define SCTE_VBI_DATA_IDENTIFIER  0x99

#define TTX_BLOCK_LEN             42
#define VPS_BLOCK_LEN             13
#define NABTS_BLOCK_LEN           33

#define TTX_FRAMING_CODE          0xe4
#define TTX_INVERTED_FRAMING_CODE 0x1b
#define NABTS_FRAMING_CODE        0xe7

#define REVERSE(x) av_reverse[(x)]

const static int vbi_type_tab[][2] =
{
    { VBI_SLICED_TELETEXT_B,         MISC_TELETEXT },
    { VBI_SLICED_TELETEXT_C_625,     VBI_NABTS },
    /* It's not worth failing to build for something as obscure as inverted teletext */
#ifdef VBI_SLICED_TELETEXT_INVERTED
    { VBI_SLICED_TELETEXT_INVERTED,  MISC_TELETEXT_INVERTED },
#endif
    { VBI_SLICED_WSS_625,            MISC_WSS },
    { VBI_SLICED_VPS,                MISC_VPS },
    { VBI_SLICED_CAPTION_525,        CAPTIONS_CEA_608 },
    { VBI_SLICED_NABTS,              VBI_NABTS },
    { -1, -1 }
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

static void set_teletext_flags( obe_sdi_non_display_data_t *non_display_data )
{
    non_display_data->has_ttx_frame = non_display_data->teletext_location != TELETEXT_LOCATION_DVB_VBI;
    non_display_data->has_vbi_frame |= non_display_data->teletext_location != TELETEXT_LOCATION_DVB_TTX;
}

static void set_wss_flags( obe_sdi_non_display_data_t *non_display_data )
{
    non_display_data->has_vbi_frame |= non_display_data->wss_output != WSS_OUTPUT_AFD;
}

static int get_vbi_type( int sliced_type )
{
    for( int i = 0; vbi_type_tab[i][0] != -1; i++ )
    {
        if( vbi_type_tab[i][0] & sliced_type )
            return vbi_type_tab[i][1];
    }

    return -1;
}

int setup_vbi_parser( obe_sdi_non_display_data_t *non_display_data )
{
    int ret, services;

    non_display_data->vbi_decoder.sampling_format = VBI_PIXFMT_UYVY;
    non_display_data->vbi_decoder.sampling_rate   = 13.5e6;
    non_display_data->vbi_decoder.bytes_per_line  = 720 * 2;
    non_display_data->vbi_decoder.interlaced      = TRUE;
    non_display_data->vbi_decoder.synchronous     = TRUE;

    if( non_display_data->ntsc == 0 )
    {
        /* FIXME: can Teletext System D be encapsulated in DVB-VBI? */
        services = VBI_SLICED_VPS | VBI_SLICED_WSS_625 | VBI_SLICED_CAPTION_625 | VBI_SLICED_TELETEXT_B | VBI_SLICED_TELETEXT_C_625;

        non_display_data->vbi_decoder.offset      = 128;
        non_display_data->vbi_decoder.scanning    = 625;
        ret = vbi_raw_decoder_add_services( &non_display_data->vbi_decoder, services, 2 );
    }
    else
    {
        /* TODO: Japanese WSS, CEA-2020 and ISO/IEC 61880-1 formats */
        services = VBI_SLICED_NABTS | VBI_SLICED_CAPTION_525;

        non_display_data->vbi_decoder.offset      = 118;
        non_display_data->vbi_decoder.scanning    = 525;
        ret = vbi_raw_decoder_add_services( &non_display_data->vbi_decoder, services, 2 );
    }

    if( !ret )
        return -1;

    /* Video index information has two supported polynomials, 0x1d is from the spec, 0x1c is from broken devices */
    ret = av_crc_init( non_display_data->crc, 0, 8, VIDEO_INDEX_CRC_POLY, sizeof(non_display_data->crc) );
    ret = av_crc_init( non_display_data->crc_broken, 0, 8, VIDEO_INDEX_CRC_POLY_BROKEN, sizeof(non_display_data->crc_broken) );
    if( ret < 0 )
    {
        fprintf( stderr, "Could not setup video index information crc \n" );
        return -1;
    }

    return 0;
}

#define REMOVE_LINES( num_lines ) \
    memmove( &sliced[i], &sliced[i+(num_lines)], (decoded_lines-i-(num_lines)) * sizeof(vbi_sliced) ); \
    if( decoded_lines >= num_lines )  \
        decoded_lines -= num_lines; \
    i--; \

int decode_vbi( obe_sdi_non_display_data_t *non_display_data, uint8_t *lines, obe_raw_frame_t *raw_frame )
{
    unsigned int decoded_lines; /* unsigned for libzvbi */
    vbi_sliced *sliced;
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;
    int j, vbi_type, found;

    sliced = non_display_data->vbi_slices;
    memset( sliced, 0, sizeof(non_display_data->vbi_slices) );
    decoded_lines = vbi_raw_decode( &non_display_data->vbi_decoder, lines, sliced );

    /* Remove from the queue if unsupported */
    for( int i = 0; i < decoded_lines; i++ )
    {
        /* Bizzarely, libzvbi can use VBI_SLICED_CAPTION_525, VBI_SLICED_CAPTION_525_F1 or VBI_SLICED_CAPTION_525_F2
         * whenever it likes */

        /* TODO: Is Field 2 on its own legal? */
        if( (sliced[i].id & VBI_SLICED_CAPTION_525) && (sliced[i+1].id & VBI_SLICED_CAPTION_525) )
            i++; /* skip field two */
        else
        {
            vbi_type = get_vbi_type( sliced[i].id );

            if( vbi_type == -1 )
            {
                REMOVE_LINES( 1 );
            }
        }
    }

    if( !decoded_lines )
        return 0;

    if( non_display_data->probe )
    {
        for( int i = 0; i < decoded_lines; i++ )
        {
            found = 0;
            vbi_type = get_vbi_type( sliced[i].id );

            /* Don't duplicate services */
            if( check_probed_non_display_data( non_display_data, vbi_type ) )
            {
                /* FIXME: deal with ANC VBI */
                frame_data = NULL; /* shut up gcc */
                for( j = 0; j < non_display_data->num_frame_data; j++ )
                {
                    frame_data = &non_display_data->frame_data[j];
                    if( frame_data->type == vbi_type )
                        break;
                }

                frame_data->lines[frame_data->num_lines++]= sliced[i].line;
                found = 1;
            }

            /* AFD is a superset of WSS so don't duplicate it */
            if( vbi_type == MISC_WSS && check_probed_non_display_data( non_display_data, MISC_AFD ) )
            {
                found = 1;
                break;
            }

            if( found )
                continue;

            if( vbi_type == MISC_TELETEXT )
                set_teletext_flags( non_display_data );
            else if( vbi_type == MISC_WSS )
                set_wss_flags( non_display_data );
            else if( vbi_type != CAPTIONS_CEA_608 )
                non_display_data->has_vbi_frame = 1;

            if( vbi_type != MISC_WSS || ( vbi_type == MISC_WSS && non_display_data->wss_output != WSS_OUTPUT_AFD ) )
            {
                tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
                if( !tmp )
                    goto fail;

                non_display_data->frame_data = tmp;
                frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
                frame_data->type = vbi_type;
                frame_data->source = VBI_RAW;
                frame_data->num_lines = 0;
                frame_data->lines[frame_data->num_lines++] = sliced[i].line;
                frame_data->location = get_non_display_location( frame_data->type );
            }

            /* WSS is converted to AFD so tell the user this */
            if( vbi_type == MISC_WSS && ( non_display_data->wss_output != WSS_OUTPUT_DVB_VBI ) )
            {
                tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
                if( !tmp )
                    goto fail;

                non_display_data->frame_data = tmp;
                frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
                frame_data->type = MISC_AFD;
                frame_data->source = MISC_WSS;
                frame_data->num_lines = 0;
                frame_data->lines[frame_data->num_lines++] = sliced[i].line;
                frame_data->location = USER_DATA_LOCATION_FRAME;
            }
        }

        /* FIXME: should we probe more frames? */
        non_display_data->has_probed = 1;
    }
    else
    {
        /* When not probing extract all data that goes in MPEG user-data leaving just
         * the array sliced[] containing data which goes in DVB-VBI */

        for( int i = 0; i < decoded_lines; i++ )
        {
            found = 0;
            /* Deal with the special cases first
             * TODO: factor out some of this code */
            if( sliced[i].id & VBI_SLICED_CAPTION_525 )
            {
                /* Don't duplicate caption data that already exists */
                found = check_active_non_display_data( raw_frame, USER_DATA_CEA_608 ) ||
                        check_active_non_display_data( raw_frame, USER_DATA_CEA_708_CDP );

                int num_lines = 1 + !!(sliced[i+1].id & VBI_SLICED_CAPTION_525);

                /* Attach the caption data to the frame's user data */
                if( !found )
                {
                    tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
                    if( !tmp2 )
                        goto fail;

                    raw_frame->user_data = tmp2;
                    user_data = &raw_frame->user_data[raw_frame->num_user_data++];

                    user_data->len  = num_lines * 2;
                    user_data->data = malloc( user_data->len );
                    if( !user_data->data )
                        goto fail;

                    user_data->type = USER_DATA_CEA_608;
                    user_data->source = VBI_RAW;

                    /* Field 1 and Field 2 */
                    memcpy( &user_data->data[0], sliced[i].data, 2 );
                    if( num_lines > 1 )
                        memcpy( &user_data->data[2], sliced[i+1].data, 2 );
                }

                /* Remove caption fields from the VBI list */
                REMOVE_LINES( num_lines );
            }
            else if( sliced[i].id == VBI_SLICED_WSS_625 )
            {
                /* Check the appropriate streams were probed */
                if( non_display_data->wss_output != WSS_OUTPUT_AFD )
                    found |= !non_display_data_was_probed( non_display_data->device, MISC_WSS, VBI_RAW, sliced[i].line );
                else
                    found |= !non_display_data_was_probed( non_display_data->device, MISC_AFD, MISC_WSS, sliced[i].line );

                /* Don't duplicate AFD data from VII or VANC */
                found |= check_active_non_display_data( raw_frame, USER_DATA_AFD );

                if( !found && non_display_data->wss_output != WSS_OUTPUT_DVB_VBI )
                {
                    /* Attach the WSS data to the frame's user data to be converted later to AFD */
                    tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
                    if( !tmp2 )
                        goto fail;

                    raw_frame->user_data = tmp2;
                    user_data = &raw_frame->user_data[raw_frame->num_user_data++];

                    user_data->len = 1;
                    user_data->data = malloc( user_data->len );
                    if( !user_data->data )
                        goto fail;

                    user_data->data[0] = sliced[i].data[0] & 0x7;
                    user_data->type = USER_DATA_WSS;
                    user_data->source = VBI_RAW;
                }

                if( found || non_display_data->wss_output == WSS_OUTPUT_AFD )
                {
                    REMOVE_LINES( 1 );
                }
                else
                    set_wss_flags( non_display_data );
            }
            else
            {
                vbi_type = get_vbi_type( sliced[i].id );

                if( !non_display_data_was_probed( non_display_data->device, vbi_type, VBI_RAW, sliced[i].line ) )
                {
                    REMOVE_LINES( 1 );
                }
                else
                {
                    if( sliced[i].id & VBI_SLICED_TELETEXT_B )
                        set_teletext_flags( non_display_data );
                    else
                        non_display_data->has_vbi_frame = 1;
                }
            }
        }
        non_display_data->num_vbi = decoded_lines;
    }

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

int decode_video_index_information( obe_sdi_non_display_data_t *non_display_data, uint16_t *line, obe_raw_frame_t *raw_frame, int line_number )
{
    /* Video index information is only in the chroma samples */
    uint8_t data[90] = {0};
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;

    for( int i = 0; i < 90; i++ )
    {
        data[i] |= (line[0]  == 0x204) << 0;
        data[i] |= (line[2]  == 0x204) << 1;
        data[i] |= (line[4]  == 0x204) << 2;
        data[i] |= (line[6]  == 0x204) << 3;
        data[i] |= (line[8]  == 0x204) << 4;
        data[i] |= (line[10] == 0x204) << 5;
        data[i] |= (line[12] == 0x204) << 6;
        data[i] |= (line[14] == 0x204) << 7;
        line += 16;
    }

    if( data[0] == 0 && data[1] == 0 && data[2] == 0 )
        return 0;

    /* Check the CRC of the first three bytes (aka. octets) */
    if( av_crc( non_display_data->crc, 0, data, 3 ) == data[3] || av_crc( non_display_data->crc_broken, 0, data, 3 ) == data[3] )
    {
        /* We only care about AFD */
        if( non_display_data->probe )
        {
            /* TODO: output both AFD sources and let user choose */
            if( check_probed_non_display_data( non_display_data, MISC_AFD ) )
                return 0;

            tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
            if( !tmp )
                goto fail;

            non_display_data->frame_data = tmp;
            frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
            frame_data->location = USER_DATA_LOCATION_FRAME;
            frame_data->type = MISC_AFD;
            frame_data->source = VBI_VIDEO_INDEX;
            frame_data->num_lines = 0;
            frame_data->lines[frame_data->num_lines++] = line_number;
        }
        else
        {
            /* TODO: follow user instructions and/or fallback */
            if( check_active_non_display_data( raw_frame, USER_DATA_AFD ) )
                return 0;

            tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
            if( !tmp2 )
                goto fail;

            raw_frame->user_data = tmp2;
            user_data = &raw_frame->user_data[raw_frame->num_user_data++];
            user_data->data = malloc( 1 );
            if( !user_data->data )
                goto fail;

            user_data->len  = 1;
            user_data->type = USER_DATA_AFD;
            user_data->source = VBI_VIDEO_INDEX;
            /* Set bit two in order to mimic the data from VANC */
            user_data->data[0] = (data[0] & 0x78) | (1 << 2);
        }
    }

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}

static void write_header_byte( bs_t *s, int smpte_line, int ntsc )
{
    int analogue_line, field, format;

    format = ntsc == 0 ? INPUT_VIDEO_FORMAT_PAL : INPUT_VIDEO_FORMAT_NTSC;
    obe_convert_smpte_to_analogue( format, smpte_line, &analogue_line, &field );

    bs_write( s, 2, 0x3 ); // reserved_future_use
    bs_write( s, 1, field == 1 ); // field_parity
    bs_write( s, 5, analogue_line ); // line_offset
}

static void write_generic_field( bs_t *s, uint8_t *data, int type )
{
    int len;

    if( type == VBI_SLICED_VPS )
        len = VPS_BLOCK_LEN;

    for( int i = 0; i < len; i++ )
        bs_write( s, 8, REVERSE( data[i] ) );
}

static void write_ttx_field( bs_t *s, uint8_t *data, int type )
{
    if( type == VBI_NABTS )
    {
        bs_write( s, 8, NABTS_FRAMING_CODE ); // framing_code
        for( int i = 0; i < NABTS_BLOCK_LEN; i++ )
            bs_write( s, 8, REVERSE( data[i] ) );
    }
    else
    {
        bs_write( s, 8, type == MISC_TELETEXT ? TTX_FRAMING_CODE : TTX_INVERTED_FRAMING_CODE ); // framing_code
        for( int i = 0; i < TTX_BLOCK_LEN; i++ )
            bs_write( s, 8, REVERSE( data[i] ) );
    }
}

static void write_wss_field( bs_t *s, uint8_t *data )
{
    bs_write( s, 8, REVERSE( data[0] ) );
    bs_write( s, 6, REVERSE( data[1] ) >> 2 );
    bs_write( s, 2, 0x3 ); // reserved_future_use
}

static void write_stuffing( bs_t *s )
{
    int count;

    if( ((bs_pos( s ) / 8) + DVB_VBI_PES_HEADER_SIZE) % 184 )
    {
        bs_write( s, 8, 0xff ); // data_unit_id
        /* add one to account for the length byte */
        count = (((bs_pos( s ) / 8) + DVB_VBI_PES_HEADER_SIZE + 1) % 184);

        if( count )
            count = 184 - count;
        bs_write( s, 8, count );    // data_unit_length
        for( int i = 0; i < count; i++ )
            bs_write( s, 8, 0xff ); // stuffing_byte
    }
}

static int encapsulate_dvb_vbi( obe_sdi_non_display_data_t *non_display_data )
{
    bs_t s, t;
    int type = 0, j, skip, identifier, data_unit_id = 0, stuffing;
    uint8_t tmp[100];
    non_display_data->dvb_vbi_frame = new_coded_frame( 0, DVB_VBI_MAXIMUM_SIZE );
    if( !non_display_data->dvb_vbi_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    bs_init( &s, non_display_data->dvb_vbi_frame->data, DVB_VBI_MAXIMUM_SIZE );

    // PES_data_field
    bs_write( &s, 8, DVB_VBI_DATA_IDENTIFIER ); // data_identifier (FIXME let user choose or passthrough from vanc)

    /* TODO: allow user to set priority between VANC VBI and VBI
     * We currently prioritise VANC over VBI */
    for( int i = 0; i < non_display_data->num_anc_vbi; i++ )
    {
#if 0
        /* Ancillary VBI is already pre-packed */
        bs_write( &s, 8, non_display_data->anc_vbi[i].unit_id ); // data_unit_id
        bs_write( &s, 8, non_display_data->anc_vbi[i].len );     // data_unit_length
        write_bytes( &s, non_display_data->anc_vbi[i].data, non_display_data->anc_vbi[i].len );
#endif
        free( non_display_data->anc_vbi[i].data );
    }

    /* Don't duplicate VBI data from VANC */
    for( int i = 0; i < non_display_data->num_vbi; i++ )
    {
        skip = identifier = 0;
        type = get_vbi_type( non_display_data->vbi_slices[i].id );

        for( j = 0; j < non_display_data->num_anc_vbi; j++ )
        {
            if( non_display_data->anc_vbi[j].identifier == type )
                skip = 1;
        }

        /* Don't write teletext stream in DVB-VBI unless the user has requested it */
        if( type == MISC_TELETEXT && non_display_data->teletext_location == TELETEXT_LOCATION_DVB_TTX )
            skip = 1;

        if( skip )
            continue;

        for( j = 0; data_indentifier_table[j][0] != -1; j++ )
        {
            if( type == data_indentifier_table[j][1] )
                data_unit_id = data_indentifier_table[j][0];
        }

        /* TODO: allow user to choose SUB or NON-SUB teletext */
        bs_write( &s, 8, data_unit_id ); // data_unit_id
        bs_flush( &s );

        /* Write data unit to temporary buffer */
        bs_init( &t, tmp, 100 );
        write_header_byte( &t, non_display_data->vbi_slices[i].line, non_display_data->vbi_decoder.scanning == 525 );

        if( type == MISC_TELETEXT || type == MISC_TELETEXT_INVERTED || type == VBI_NABTS )
            write_ttx_field( &t, non_display_data->vbi_slices[i].data, type );
        else if( type == MISC_VPS )
            write_generic_field( &t, non_display_data->vbi_slices[i].data, type );
        else if( type == MISC_WSS )
            write_wss_field( &t, non_display_data->vbi_slices[i].data );

        /* Some DVB data units require stuffing */
        if( identifier >= 0x10 && identifier <= 0x1f )
        {
            stuffing = DVB_VBI_UNIT_SIZE - (bs_pos( &t ) / 8);
            for( int k = 0; k < stuffing; k++ )
                bs_write( &t, 8, 0xff ); // stuffing_byte
        }

        bs_flush( &t );

        bs_write( &s, 8, bs_pos( &t ) / 8 ); // data_unit_length
        write_bytes( &s, tmp, bs_pos( &t ) / 8 );
    }

    /* Stuffing bytes */
    write_stuffing( &s );
    bs_flush( &s );

    non_display_data->dvb_vbi_frame->len = bs_pos( &s ) / 8;

    return 0;
}

static int encapsulate_dvb_ttx( obe_sdi_non_display_data_t *non_display_data )
{
    bs_t s;

    non_display_data->dvb_ttx_frame = new_coded_frame( 0, DVB_VBI_MAXIMUM_SIZE );
    if( !non_display_data->dvb_ttx_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    bs_init( &s, non_display_data->dvb_ttx_frame->data, DVB_VBI_MAXIMUM_SIZE );

    // PES_data_field
    bs_write( &s, 8, DVB_VBI_DATA_IDENTIFIER ); // data_identifier (FIXME let user choose or passthrough from vanc)

    for( int i = 0; i < non_display_data->num_vbi; i++ )
    {
        /* TODO: Teletext from VANC */
        /* Teletext B is the only kind of Teletext allowed in a DVB-TTX stream */
        if( non_display_data->vbi_slices[i].id & VBI_SLICED_TELETEXT_B )
        {
            /* TODO: allow user to choose SUB or NON-SUB teletext */
            bs_write( &s, 8, DATA_UNIT_ID_EBU_TTX_NON_SUB ); // data_unit_id
            bs_write( &s, 8, DVB_VBI_UNIT_SIZE ); // data_unit_length
            write_header_byte( &s, non_display_data->vbi_slices[i].line, non_display_data->vbi_decoder.scanning == 525 );
            write_ttx_field( &s, non_display_data->vbi_slices[i].data, MISC_TELETEXT );
        }
    }

    /* Stuffing bytes */
    write_stuffing( &s );
    bs_flush( &s );

    non_display_data->dvb_ttx_frame->len = bs_pos( &s ) / 8;

    return 0;
}

int send_vbi_and_ttx( obe_t *h, obe_sdi_non_display_data_t *non_display_parser, obe_device_t *device, int64_t pts )
{
    int stream_id;

    /* Send any DVB-VBI frames */
    if( non_display_parser->has_vbi_frame )
    {
        stream_id = -1;
        // FIXME when we make streams selectable
        for( int i = 0; i < device->num_input_streams; i++ )
        {
            if( device->streams[i]->stream_format == VBI_RAW )
                stream_id = device->streams[i]->stream_id;
        }

        if( stream_id >= 0 )
        {
            if( encapsulate_dvb_vbi( non_display_parser ) < 0 )
                return -1;

            non_display_parser->dvb_vbi_frame->stream_id = stream_id;
            non_display_parser->dvb_vbi_frame->pts = pts;

            if( add_to_mux_queue( h, non_display_parser->dvb_vbi_frame ) < 0 )
            {
                destroy_coded_frame( non_display_parser->dvb_ttx_frame );
                return -1;
            }
        }
        non_display_parser->dvb_vbi_frame = NULL;
        non_display_parser->has_vbi_frame = 0;
    }

    /* Send any DVB-TTX frames */
    if( non_display_parser->has_ttx_frame )
    {
        stream_id = -1;
        // FIXME when we make streams selectable
        for( int i = 0; i < device->num_input_streams; i++ )
        {
            if( device->streams[i]->stream_format == MISC_TELETEXT )
                stream_id = device->streams[i]->stream_id;
        }

        if( stream_id >= 0 )
        {
            if( encapsulate_dvb_ttx( non_display_parser ) < 0 )
                return -1;

            non_display_parser->dvb_ttx_frame->stream_id = stream_id;
            non_display_parser->dvb_ttx_frame->pts = pts;

            if( add_to_mux_queue( h, non_display_parser->dvb_ttx_frame ) < 0 )
            {
                destroy_coded_frame( non_display_parser->dvb_ttx_frame );
                return -1;
            }
        }
        non_display_parser->dvb_ttx_frame = NULL;
        non_display_parser->has_ttx_frame = 0;
    }

    return 0;
}
