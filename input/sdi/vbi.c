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

int setup_vbi_parser( vbi_raw_decoder *vbi_decoder_ctx, int ntsc, int vanc )
{
    int ret;

    vbi_raw_decoder_init( vbi_decoder_ctx );

    vbi_decoder_ctx->sampling_format = VBI_PIXFMT_YUV420;
    vbi_decoder_ctx->sampling_rate   = 13.5e6;
    vbi_decoder_ctx->bytes_per_line  = 720 * 2;
    vbi_decoder_ctx->offset          = 9.5e-6 * 13.5e6; // FIXME correct value for NTSC

    vbi_decoder_ctx->interlaced      = TRUE;
    vbi_decoder_ctx->synchronous     = TRUE;

    if( ntsc == 0 )
    {
        vbi_decoder_ctx->scanning        = 625;

        // FIXME PAL VBI

        if( vanc )
            ret = vbi_raw_decoder_add_services( vbi_decoder_ctx, VBI_SLICED_WSS_625, 2 ); // FIXME
        else
            ret = vbi_raw_decoder_add_services( vbi_decoder_ctx, VBI_SLICED_WSS_625, 2 );
    }
    else
    {
        vbi_decoder_ctx->scanning        = 525;
        vbi_decoder_ctx->start[0]        = 21;
        vbi_decoder_ctx->count[0]        = 1;
        vbi_decoder_ctx->start[1]        = 284;
        vbi_decoder_ctx->count[1]        = 1;

        ret = vbi_raw_decoder_add_services( vbi_decoder_ctx, VBI_SLICED_CAPTION_525, 2 );
    }

    if( !ret )
        return -1;

    return 0;
}

void destroy_vbi_parser( vbi_raw_decoder *vbi_decoder_ctx )
{
    vbi_raw_decoder_destroy( vbi_decoder_ctx );
}
