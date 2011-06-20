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

#define DVB_VBI_DATA_IDENTIFIER  0x10
#define SCTE_VBI_DATA_IDENTIFIER 0x99
/* libzvbi input buffer must be mod 46... */
#define DVB_VBI_MAX_SIZE 69967
#define VIDEO_INDEX_CRC_POLY 0x1d
#define VIDEO_INDEX_CRC_POLY_BROKEN 0x1c

#define DVB_VBI_MAXIMUM_SIZE      65536

#define DVB_VBI_DATA_IDENTIFIER   0x10
#define SCTE_VBI_DATA_IDENTIFIER  0x99

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

int setup_vbi_parser( obe_sdi_non_display_data_t *non_display_data, int ntsc )
{
    int ret;

    vbi_raw_decoder_init( &non_display_data->vbi_decoder );

    non_display_data->vbi_decoder.sampling_format = VBI_PIXFMT_UYVY;
    non_display_data->vbi_decoder.sampling_rate   = 13.5e6;
    non_display_data->vbi_decoder.bytes_per_line  = 720 * 2;
    non_display_data->vbi_decoder.interlaced      = TRUE;
    non_display_data->vbi_decoder.synchronous     = TRUE;

    if( ntsc == 0 )
    {
        /* FIXME: teletext */
        non_display_data->vbi_decoder.scanning    = 625;
        non_display_data->vbi_decoder.offset      = 128;
        non_display_data->vbi_decoder.interlaced  = FALSE;
        non_display_data->vbi_decoder.start[0]    = 23;
        non_display_data->vbi_decoder.count[0]    = 1;

        ret = vbi_raw_decoder_add_services( &non_display_data->vbi_decoder, VBI_SLICED_WSS_625, 2 );
    }
    else
    {
        /* FIXME: other */
        non_display_data->vbi_decoder.scanning    = 525;
        non_display_data->vbi_decoder.offset      = 118;
        non_display_data->vbi_decoder.start[0]    = 21;
        non_display_data->vbi_decoder.count[0]    = 1;
        non_display_data->vbi_decoder.start[1]    = 284;
        non_display_data->vbi_decoder.count[1]    = 1;

        ret = vbi_raw_decoder_add_services( &non_display_data->vbi_decoder, VBI_SLICED_CAPTION_525, 2 );
    }

    if( !ret )
        return -1;

    return 0;
}

int decode_vbi( obe_sdi_non_display_data_t *non_display_data, uint8_t *lines, obe_raw_frame_t *raw_frame )
{
    unsigned int decoded_lines, buf_size; /* unsigned for libzvbi */
    uint8_t *dvb_buf, *buf_ptr;
    vbi_sliced *sliced, *sliced_tmp;
    obe_user_data_t *tmp, *user_data;

    sliced = malloc( 100 * sizeof(*sliced) );
    if( !sliced )
        goto fail;

    sliced_tmp = sliced;
    decoded_lines = vbi_raw_decode( &non_display_data->vbi_decoder, lines, sliced );

    if( non_display_data->probe )
    {
        if( decoded_lines )
        {
            non_display_data->num_frame_data = 1;
            non_display_data->frame_data = calloc( 1, sizeof(*non_display_data->frame_data) );
            if( !non_display_data->frame_data )
                goto fail;

            /* FIXME: many other formats, including mixed separate stream and frame */
            /* For now all PAL VBI goes as a separate stream and all NTSC goes in the frame */
            if( non_display_data->vbi_decoder.scanning == 625 )
            {
                non_display_data->frame_data[0].type = MISC_WSS;
                non_display_data->frame_data[0].location = USER_DATA_LOCATION_DVB_STREAM;
            }
            else
            {
                non_display_data->frame_data[0].type = CAPTIONS_CEA_608;
                non_display_data->frame_data[0].location = USER_DATA_LOCATION_FRAME;
            }

            non_display_data->frame_data[0].source = VBI_RAW;
            non_display_data->frame_data[0].line_number = sliced[0].line;
        }

        /* FIXME: should we probe more frames? */
        non_display_data->has_probed = 1;
    }
    else
    {
        /* FIXME: if WSS suddenly appears in the stream this will leak */
        /* FIXME: deal with more VBI services */
        if( non_display_data->vbi_decoder.scanning == 625 && decoded_lines == 1 )
        {
            /* Overallocate a little */
            buf_size = DVB_VBI_MAX_SIZE;
            dvb_buf = malloc( buf_size );
            if( !dvb_buf )
                goto fail;

            dvb_buf[0] = DVB_VBI_DATA_IDENTIFIER;
            buf_ptr = &dvb_buf[1];
            buf_size--;

            if( vbi_dvb_multiplex_sliced( &buf_ptr, &buf_size, (const vbi_sliced **)&sliced_tmp, &decoded_lines,
                                           VBI_SLICED_WSS_625, DVB_VBI_DATA_IDENTIFIER, FALSE ) == FALSE )
            {
                free( dvb_buf );
                goto fail;
            }

            /* The input device will add the stream_id and pts */
            non_display_data->dvb_frame = new_coded_frame( 0, DVB_VBI_MAX_SIZE - buf_size );
            if( !non_display_data->dvb_frame )
                goto fail;

            non_display_data->dvb_frame->data = dvb_buf;
        }
        else if( non_display_data->vbi_decoder.scanning == 525 && decoded_lines == 2 )
        {
            tmp = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
            if( !tmp )
                goto fail;

            raw_frame->user_data = tmp;
            user_data = &raw_frame->user_data[raw_frame->num_user_data];
            user_data->data = malloc( 4 );
            if( !user_data->data )
                goto fail;

            user_data->len  = 4;
            user_data->type = USER_DATA_CEA_608;
            user_data->source = VBI_RAW;

            /* Field 1 and Field 2 */
            memcpy( &user_data->data[0], sliced[0].data, 2 );
            memcpy( &user_data->data[2], sliced[1].data, 2 );
            raw_frame->num_user_data++;
        }
    }

    free( sliced );

    return 0;

fail:
    if( sliced )
        free( sliced );

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

    /* Check the CRC of the first three octets */
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
            user_data->data[0] = (data[0] & 0x78) | (1 << 2);
        }
    }

    return 0;

fail:
    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}
