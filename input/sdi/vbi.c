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

int setup_vbi_parser( vbi_raw_decoder *vbi_decoder_ctx, int ntsc )
{
    int ret;

    vbi_raw_decoder_init( vbi_decoder_ctx );

    vbi_decoder_ctx->sampling_format = VBI_PIXFMT_UYVY;
    vbi_decoder_ctx->sampling_rate   = 13.5e6;
    vbi_decoder_ctx->bytes_per_line  = 720 * 2;
    vbi_decoder_ctx->interlaced      = TRUE;
    vbi_decoder_ctx->synchronous     = TRUE;

    if( ntsc == 0 )
    {
        vbi_decoder_ctx->scanning    = 625;
        vbi_decoder_ctx->offset      = 128;

        // FIXME PAL VBI

        ret = vbi_raw_decoder_add_services( vbi_decoder_ctx, VBI_SLICED_WSS_625, 2 );
    }
    else
    {
        vbi_decoder_ctx->scanning    = 525;
        vbi_decoder_ctx->offset      = 118;
        vbi_decoder_ctx->start[0]    = 21;
        vbi_decoder_ctx->count[0]    = 1;
        vbi_decoder_ctx->start[1]    = 284;
        vbi_decoder_ctx->count[1]    = 1;

        ret = vbi_raw_decoder_add_services( vbi_decoder_ctx, VBI_SLICED_CAPTION_525, 2 );
    }

    if( !ret )
        return -1;

    return 0;
}

int decode_vbi( vbi_raw_decoder *vbi_decoder_ctx, uint8_t *lines, int probe, obe_raw_frame_t *raw_frame )
{
    int decoded_lines;
    vbi_sliced sliced[2];
    obe_user_data_t *tmp, *user_data;

    decoded_lines = vbi_raw_decode( vbi_decoder_ctx, lines, sliced );

    if( probe )
    {

    }
    else
    {
        /* FIXME: deal with more NTSC VBI services */
        if( vbi_decoder_ctx->scanning == 525 && decoded_lines == 2 )
        {
            tmp = realloc( raw_frame->user_data, (raw_frame->num_user_data+1) * sizeof(*raw_frame->user_data) );
            if( !tmp )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            raw_frame->user_data = tmp;
	    user_data = &raw_frame->user_data[raw_frame->num_user_data];
            user_data->data = malloc( 4 );
            if( !user_data->data )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            user_data->len  = 4;
            user_data->type = USER_DATA_CEA_608;
            memcpy( &user_data->data[0], sliced[0].data, 2 );
            memcpy( &user_data->data[2], sliced[1].data, 2 );
            raw_frame->num_user_data++;
        }
    }

    return 0;
}
