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
#include "vbi.h"

#define DVB_VBI_DATA_IDENTIFIER 0x10
#define DVB_VBI_MAX_SIZE 65536

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
    vbi_sliced *sliced;
    obe_user_data_t *tmp, *user_data;

    sliced = malloc( 100 * sizeof(*sliced) );
    if( !sliced )
        goto fail;

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
                non_display_data->frame_data[0].type = MISC_WSS;
            else
                non_display_data->frame_data[0].type = USER_DATA_CEA_608;

            non_display_data->frame_data[0].source = VBI_RAW;
            non_display_data->frame_data[0].line_number = sliced[0].line;
        }

        /* FIXME: should we probe more frames? */
        non_display_data->has_probed = 1;
    }
    else
    {
        /* FIXME: deal with more VBI services */
        if( non_display_data->vbi_decoder.scanning == 625 && decoded_lines == 1 )
        {
            buf_size = DVB_VBI_MAX_SIZE;
            dvb_buf = malloc( buf_size );
            if( !dvb_buf )
                goto fail;

            dvb_buf[0] = DVB_VBI_DATA_IDENTIFIER;
            buf_ptr = &dvb_buf[1];
            buf_size--;

	    if( !vbi_dvb_multiplex_sliced( &buf_ptr, &buf_size, (const vbi_sliced **)&sliced, &decoded_lines,
                                           VBI_SLICED_WSS_625, DVB_VBI_DATA_IDENTIFIER, FALSE ) )
            {
                free( dvb_buf );
                goto fail;
            }

            /* The input device will add the stream_id */
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

    return 0;

fail:
    if( sliced )
        free( sliced );

    syslog( LOG_ERR, "Malloc failed\n" );
    return -1;
}
