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
    { VBI_SLICED_TELETEXT_B_L10_625, MISC_TELETEXT },
    { VBI_SLICED_TELETEXT_B_L25_625, MISC_TELETEXT },
    { VBI_SLICED_TELETEXT_C_625,     VBI_NABTS },
    { VBI_SLICED_WSS_625,            MISC_WSS },
    { VBI_SLICED_VPS,                MISC_VPS },
    { VBI_SLICED_CAPTION_525_F1,     CAPTIONS_CEA_608 },
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
        services = VBI_SLICED_VPS | VBI_SLICED_WSS_625 | VBI_SLICED_CAPTION_625; /* | VBI_SLICED_TELETEXT_B | VBI_SLICED_TELETEXT_C_625 | */

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
    };

    return 0;
}

int decode_vbi( obe_sdi_non_display_data_t *non_display_data, uint8_t *lines, obe_raw_frame_t *raw_frame )
{
    unsigned int decoded_lines; /* unsigned for libzvbi */
    vbi_sliced *sliced;
    obe_int_frame_data_t *tmp, *frame_data;
    obe_user_data_t *tmp2, *user_data;
    int j, l, found;

    sliced = non_display_data->vbi_slices;
    decoded_lines = vbi_raw_decode( &non_display_data->vbi_decoder, lines, sliced );

    /* Remove from the queue if unsupported */
    for( int i = 0; i < decoded_lines; i++ )
    {
        int remove_vbi = 0;

        /* TODO: support single field captions. Is Field 2 on its own legal? */
        if( sliced[i].id == VBI_SLICED_CAPTION_525_F2 )
            remove_vbi = 1;
        else if( sliced[i].id == VBI_SLICED_CAPTION_525_F1 )
        {
             if( i == decoded_lines-1 || sliced[i+1].id != VBI_SLICED_CAPTION_525_F2 )
                 remove_vbi = 1;
             else
                 i++; /* Skip field two */
        }
        else
        {
            for( j = 0; vbi_type_tab[j][0] != -1; j++ )
            {
                if( sliced[i].id == vbi_type_tab[j][0] )
                    break;
            }

            if( vbi_type_tab[j][0] == -1 )
                remove_vbi = 1;
        }

        if( remove_vbi )
        {
            memmove( &sliced[i], &sliced[i+1], (decoded_lines-i-1) * sizeof(vbi_sliced) );
            if( decoded_lines )
                decoded_lines--;
        }
    }

    if( !decoded_lines )
        return 0;

    if( non_display_data->probe )
    {
        for( int i = 0; i < decoded_lines; i++ )
        {
            found = 0;
            for( j = 0; vbi_type_tab[j][0] != -1; j++ )
            {
                if( vbi_type_tab[j][0] == sliced[i].id )
                    break;
            }

            /* Don't duplicate services */
            for( int k = 0; k < non_display_data->num_frame_data; k++ )
            {
                /* AFD is a superset of WSS so don't duplicate it */
                if( vbi_type_tab[j][1] == non_display_data->frame_data[k].type ||
                    ( vbi_type_tab[j][1] == MISC_WSS && non_display_data->frame_data[k].type == MISC_AFD ) )
                {
                    found = 1;
                    break;
                }
            }

            if( found )
                continue;

            /* TODO: split up teletext and VBI if user requests it */
            tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
            if( !tmp )
                goto fail;

            non_display_data->frame_data = tmp;
            frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
            frame_data->type = vbi_type_tab[j][1];
            frame_data->source = VBI_RAW;
            frame_data->line_number = sliced[i].line;

            /* Set the appropriate location */
            for( l = 0; non_display_data_locations[l].service != -1; l++ )
            {
                if( frame_data->type == non_display_data_locations[l].service )
                    break;
            }

            frame_data->location = non_display_data_locations[l].location;

            /* WSS is converted to AFD so tell the user this.
             * TODO: make this user-settable */
            if( frame_data->type == MISC_WSS )
            {
                tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
                if( !tmp )
                    goto fail;

                non_display_data->frame_data = tmp;
                frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
                frame_data->type = MISC_AFD;
                frame_data->source = MISC_WSS;
                frame_data->line_number = sliced[i].line;
                frame_data->location = USER_DATA_LOCATION_FRAME;
            }
        }

        /* FIXME: should we probe more frames? */
        non_display_data->has_probed = 1;
    }
    else
    {
        for( int i = 0; i < decoded_lines; i++ )
        {
            /* TODO: factor out some of this code */
            /* TODO: handle other non dvb-vbi formats */
            if( sliced[i].id == VBI_SLICED_CAPTION_525_F1 )
            {
                /* Don't duplicate caption data that already exists */
                found = 0;
                for( j = 0; j < raw_frame->num_user_data; j++ )
                {
                    if( raw_frame->user_data[j].type == USER_DATA_CEA_608 || raw_frame->user_data[j].type == USER_DATA_CEA_708_CDP )
                    {
                        found = 1;
                        break;
                    }
                }

                if( !found )
                {
                    tmp2 = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
                    if( !tmp2 )
                        goto fail;

                    raw_frame->user_data = tmp2;
                    user_data = &raw_frame->user_data[raw_frame->num_user_data++];

                    user_data->len  = 4;
                    user_data->data = malloc( user_data->len );
                    if( !user_data->data )
                        goto fail;

                    user_data->type = USER_DATA_CEA_608;
                    user_data->source = VBI_RAW;

                    /* Field 1 and Field 2 */
                    memcpy( &user_data->data[0], sliced[i].data, 2 );
                    memcpy( &user_data->data[2], sliced[i+1].data, 2 );
                }

                /* Remove caption fields from the VBI list */
                memmove( &sliced[i], &sliced[i+2], (decoded_lines-i-2) * sizeof(vbi_sliced) );
                if( decoded_lines >= 2 )
                    decoded_lines -= 2;
                i--;
            }
            else if( sliced[i].id == VBI_SLICED_WSS_625 )
            {
                /* Don't duplicate AFD data */
                found = 0;
                for( j = 0; j < raw_frame->num_user_data; j++ )
                {
                    if( raw_frame->user_data[j].type == USER_DATA_AFD )
                    {
                        found = 1;
                        break;
                    }
                }

                if( !found )
                {
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
            for( int i = 0; i < non_display_data->num_frame_data; i++ )
            {
               if( non_display_data->frame_data[i].type == MISC_AFD )
                   return 0;
            }

            tmp = realloc( non_display_data->frame_data, (non_display_data->num_frame_data+1) * sizeof(*non_display_data->frame_data) );
            if( !tmp )
                goto fail;

            non_display_data->frame_data = tmp;
            frame_data = &non_display_data->frame_data[non_display_data->num_frame_data++];
            frame_data->location = USER_DATA_LOCATION_FRAME;
            frame_data->type = MISC_AFD;
            frame_data->source = VBI_VIDEO_INDEX;
            frame_data->line_number = line_number;
        }
        else
        {
            /* TODO: follow user instructions and/or fallback */
            for( int i = 0; i < raw_frame->num_user_data; i++ )
            {
                if( raw_frame->user_data[i].type == USER_DATA_AFD )
                    return 0;
            }

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
    if( type == VBI_SLICED_NABTS )
    {
        bs_write( s, 8, NABTS_FRAMING_CODE ); // framing_code
        for( int i = 0; i < NABTS_BLOCK_LEN; i++ )
            bs_write( s, 8, REVERSE( data[i] ) );
    }
    else
    {
        bs_write( s, 8, TTX_FRAMING_CODE ); // framing_code
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

int encapsulate_dvb_vbi( obe_sdi_non_display_data_t *non_display_data )
{
    bs_t s, t;
    int type = 0, j, found, identifier;
    uint8_t tmp[100];
    non_display_data->dvb_frame = new_coded_frame( 0, DVB_VBI_MAXIMUM_SIZE );
    if( !non_display_data->dvb_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    bs_init( &s, non_display_data->dvb_frame->data, DVB_VBI_MAXIMUM_SIZE );

    // PES_data_field
    bs_write( &s, 8, DVB_VBI_DATA_IDENTIFIER ); // data_identifier (FIXME let user choose or passthrough from vanc)

    /* TODO: allow user to set priority between VANC VBI and VBI
     * We currently prioritise VANC over VBI */
    for( int i = 0; i < non_display_data->num_anc_vbi; i++ )
    {
        /* Ancillary VBI is already pre-packed */
        bs_write( &s, 8, non_display_data->anc_vbi[i].unit_id ); // data_unit_id
        bs_write( &s, 8, non_display_data->anc_vbi[i].len );     // data_unit_length
        write_bytes( &s, non_display_data->anc_vbi[i].data, non_display_data->anc_vbi[i].len );
        free( non_display_data->anc_vbi[i].data );
    }

    /* Don't duplicate VBI data from VANC */
    for( int i = 0; i < non_display_data->num_vbi; i++ )
    {
        type = found = identifier = 0;
        for( j = 0; vbi_type_tab[j][0] != -1; j++ )
        {
            if( vbi_type_tab[j][0] == non_display_data->vbi_slices[i].id )
                type = vbi_type_tab[j][1];
        }

        for( j = 0; j < non_display_data->num_anc_vbi; j++ )
        {
            if( non_display_data->anc_vbi[j].identifier == type )
                found = 1;
        }

        if( found )
            continue;

        for( j = 0; data_indentifier_table[j][0] != -1; j++ )
        {
            if( type == data_indentifier_table[j][1] )
                identifier = data_indentifier_table[j][0];
        }

        /* TODO: allow user to choose SUB or NON-SUB teletext */
        bs_write( &s, 8, identifier ); // data_unit_id
        bs_flush( &s );

        /* Write data unit to temporary buffer */
        bs_init( &t, tmp, 100 );
        write_header_byte( &t, non_display_data->vbi_slices[i].line, non_display_data->vbi_decoder.scanning == 525 );

        /* TODO: let the user choose inverted */
        if( type == MISC_TELETEXT || type == VBI_NABTS )
            write_ttx_field( &t, non_display_data->vbi_slices[i].data, type );
        else if( type == MISC_VPS )
            write_generic_field( &t, non_display_data->vbi_slices[i].data, type );
        else if( type == MISC_WSS )
            write_wss_field( &t, non_display_data->vbi_slices[i].data );

        bs_flush( &t );

        bs_write( &s, 8, bs_pos( &t ) / 8 ); // data_unit_length
        write_bytes( &s, tmp, bs_pos( &t ) / 8 );
    }

    /* Stuffing bytes */

    bs_flush( &s );

    non_display_data->dvb_frame->len = bs_pos( &s ) / 8;

    return 0;
}
