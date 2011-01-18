/*****************************************************************************
 * obe.c: open broadcast encoder functions
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
#include "input/input.h"

void *obe_start_main_thread( void *ptr );

obe_t *obe_setup( void )
{
    obe_t *h = calloc( 1, sizeof(*h) );
    if( !h )
    {
        fprintf( stderr, "Malloc failed" );
        return NULL;
    }

    return h;
}



int obe_populate_encoder_params( obe_t *h, int input_stream_id, x264_param_t *param )
{
    // TODO populate these properly
    int fps_num = 1;
    int fps_den = 25;

    int width = 1920;
    int height = 1080;

    x264_param_default( param );

    param->b_vfr_input = 0;
    param->b_pic_struct = 1;
    param->i_fps_num = fps_num;
    param->i_fps_den = fps_den;

    param->vui.i_overscan = 2;

    if( ( fps_num == 25 || fps_num == 50 ) && fps_den == 1 )
    {
        param->vui.i_vidformat = 1; // PAL
        param->vui.i_colorprim = 5; // BT.470-2 bg
        param->vui.i_transfer  = 5; // BT.470-2 bg
        param->vui.i_colmatrix = 5; // BT.470-2 bg
    }
    else if( ( fps_num == 30000 || fps_num == 60000 ) && fps_num == 1001 )
    {
        param->vui.i_vidformat = 2; // NTSC
        param->vui.i_colorprim = 6; // BT.601-6
        param->vui.i_transfer  = 6; // BT.601-6
        param->vui.i_colmatrix = 6; // BT.601-6
    }
    else
    {
        param->vui.i_vidformat = 5; // undefined
        param->vui.i_colorprim = 2; // undefined
        param->vui.i_transfer  = 2; // undefined
        param->vui.i_colmatrix = 2; // undefined
    }

    /* Change to BT.709 for HD resolutions */
    if( width >= 1280 && height >= 720 )
    {
        param->vui.i_colorprim = 1;
        param->vui.i_transfer  = 1;
        param->vui.i_colmatrix = 1;
    }

    param->sc.f_speed = 1.0;
    param->b_aud = 1;
    param->i_nal_hrd = X264_NAL_HRD_FAKE_VBR;

    return 0;
}

void obe_close( obe_t *h )
{
    free( h );
}
