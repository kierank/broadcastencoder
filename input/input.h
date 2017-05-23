/*****************************************************************************
 * input.h : OBE input headers
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

#ifndef OBE_INPUT_H
#define OBE_INPUT_H

typedef struct
{
    void* (*probe_input)( void *ptr );
    void* (*autoconf_input)( void *ptr );
    void* (*open_input)( void *ptr );
} obe_input_func_t;

typedef struct
{
    obe_t *h;
    obe_input_t user_opts;
} obe_input_probe_t;

typedef struct
{
    obe_t *h;
    obe_device_t *device;
    int num_output_streams;
    obe_output_stream_t *output_streams;
    int audio_samples;
} obe_input_params_t;

//extern const obe_input_func_t lavf_input;
#if HAVE_DECKLINK
extern const obe_input_func_t decklink_input;
#endif
extern const obe_input_func_t linsys_sdi_input;
extern const obe_input_func_t bars_input;
extern const obe_input_func_t netmap_input;

#endif
