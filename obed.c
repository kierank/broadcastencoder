/*****************************************************************************
 * obe.d: Open Broadcast Encoder daemon
 *****************************************************************************
 * Copyright (C) 2013 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Some code originates from the x264 project
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#include "config.h"

#include <signal.h>
#define _GNU_SOURCE

#include "obe.h"
#include <google/protobuf-c/protobuf-c-rpc.h>
#include "proto/obed.pb-c.h"

#define CONTROL_VERSION 1

typedef struct
{
    obe_t *h;
    obe_input_t input;
    obe_input_program_t program;
    int num_output_streams;
    obe_output_stream_t *output_streams;
    obe_mux_opts_t mux_opts;
    obe_output_opts_t output;
    int avc_profile;
} obed_ctx_t;

obed_ctx_t d;

/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;
static char *line_read = NULL;

static int running = 0;
static int system_type_value = OBE_SYSTEM_TYPE_GENERIC;

/* Video */
static const char * const avc_profiles[] = { "main", "high" };

/* Audio */
const static int formats[] = { AUDIO_MP2, AUDIO_AAC };

const static uint64_t channel_layouts[] =
{
    AV_CH_LAYOUT_MONO,
    AV_CH_LAYOUT_STEREO,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_5POINT1_BACK,
};

/* server options */
static volatile int keep_running;

static void stop_server( int a )
{
    keep_running = 0;
}

static void stop_encode( void )
{
    obe_close( d.h );
    d.h = NULL;

    if( d.input.location )
    {
        free( d.input.location );
        d.input.location = NULL;
    }

    if( d.mux_opts.service_name )
    {
        free( d.mux_opts.service_name );
        d.mux_opts.service_name = NULL;
    }

    if( d.mux_opts.provider_name )
    {
        free( d.mux_opts.provider_name );
        d.mux_opts.provider_name = NULL;
    }

    if( d.output_streams )
    {
        free( d.output_streams );
        d.output_streams = NULL;
    }

    if( d.output.target )
    {
        free( d.output.target );
        d.output.target = NULL;
    }

    memset( &d, 0, sizeof(d) );
    running = 0;
}

static void obed__encoder_config( Obed__EncoderConfig_Service     *service,
                                  const Obed__EncoderControl      *encoder_control,
                                  Obed__EncoderResponse_Closure   closure,
                                  void                            *closure_data )
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    int has_dvb_vbi = 0;
    int i = 0;

    if( encoder_control->control_version == 1 )
    {
        if( running == 1 )
            stop_encode();

        if( encoder_control->input_opts )
        {
            /* only messages with all options are legal currently */
            Obed__InputOpts *input_opts_in = encoder_control->input_opts;
            obe_input_t *input_opts_out = &d.input;
            Obed__VideoOpts *video_opts_in = encoder_control->video_opts;
            obe_output_stream_t *video_stream = &d.output_streams[0];
            Obed__AncillaryOpts *ancillary_opts_in = encoder_control->ancillary_opts;
            Obed__MuxOpts *ancillary_mux_opts_in = encoder_control->mux_opts;
            obe_mux_opts_t *mux_opts = &d.mux_opts;

            input_opts_out->input_type = input_opts_in->input_device;
            input_opts_out->card_idx = input_opts_in->card_idx;
            input_opts_out->video_format = input_opts_in->video_format;

            if( obe_probe_device( d.h, &d.input, &d.program ) < 0 )
            {
                // TODO: syslog error message
                goto fail;
            }

            /* Add video stream */
            d.num_output_streams++;
            /* Add audio tracks */
            d.num_output_streams += encoder_control->n_audio_opts;
            /* Add DVB-TTX PID */
            d.num_output_streams += encoder_control->ancillary_opts->dvb_ttx_enabled;

            /* Add DVB-VBI PID
             * Special case because if DVB-VBI wasn't detected during probe we can't
             * do anything about it */
            if( encoder_control->ancillary_opts->dvb_vbi_enabled )
            {
                // FIXME check input streams
                d.num_output_streams += 1;
            }

            d.output_streams = calloc( d.num_output_streams, sizeof(*d.output_streams) );
            if( !d.output_streams )
                goto fail;

            /* Setup video stream */
            video_stream->input_stream_id = 0;
            obe_populate_avc_encoder_params( d.h, video_stream->input_stream_id, &video_stream->avc_param );
            d.avc_profile = video_opts_in->profile;

            /* Mux options */
            video_stream->stream_action = STREAM_ENCODE;
            video_stream->stream_format = VIDEO_AVC;
            video_stream->ts_opts.pid = video_opts_in->pid;

            /* Compression options */
            video_stream->avc_param.rc.i_vbv_max_bitrate = video_opts_in->bitrate;
            video_stream->avc_param.rc.i_vbv_buffer_size = video_opts_in->vbv_bufsize;
            video_stream->avc_param.rc.i_bitrate         = video_opts_in->bitrate;
            video_stream->avc_param.i_keyint_max        = video_opts_in->keyint;
            video_stream->avc_param.rc.i_lookahead      = video_opts_in->lookahead;
            video_stream->avc_param.i_bframe            = video_opts_in->bframes;
            video_stream->avc_param.i_frame_reference   = video_opts_in->max_refs;
            video_stream->avc_param.i_frame_reference   = video_opts_in->max_refs;
            video_stream->avc_param.i_frame_packing     = video_opts_in->frame_packing;
            video_stream->avc_param.i_width             = video_opts_in->width;
            video_stream->is_wide                       = video_opts_in->aspect_ratio;

            // TODO PSNR/SSIM Quality metric
            // FIXME decide on threads

            /* Video frame ancillary data */
            video_stream->video_anc.afd                 = video_opts_in->afd_passthrough;
            video_stream->video_anc.wss_to_afd          = video_opts_in->wss_to_afd;
            video_stream->video_anc.cea_608             = ancillary_opts_in->cea_608;
            video_stream->video_anc.cea_708             = ancillary_opts_in->cea_708;

            /* Turn on the 3DTV mux option automatically */
            if( video_stream->avc_param.i_frame_packing >= 0 )
                d.mux_opts.is_3dtv = 1;

            /* Setup video profile */
            if( d.avc_profile >= 0 )
                x264_param_apply_profile( &video_stream->avc_param, "high" );

            /* Setup audio streams */
            for( i = 1; i <= encoder_control->n_audio_opts; i++ )
            {
                int j = i-1;
                Obed__AudioOpts *audio_opts_in = encoder_control->audio_opts[j];
                obe_output_stream_t *audio_stream = &d.output_streams[i];

                audio_stream->stream_action = STREAM_ENCODE;
                // FIXME deal with HE-AAC
                audio_stream->stream_format = formats[audio_opts_in->format];
                audio_stream->ts_opts.pid = audio_opts_in->pid;

                audio_stream->channel_layout = channel_layouts[audio_opts_in->channel_map];
                audio_stream->bitrate = audio_opts_in->bitrate;
                audio_stream->sdi_audio_pair = audio_opts_in->sdi_pair;
                audio_stream->aac_opts.latm_output = audio_opts_in->aac_encap;
                audio_stream->mp2_mode = audio_opts_in->mp2_mode;
                audio_stream->mono_channel = audio_opts_in->mono_channel;
                // FIXME deal with reference level
                //audio_stream->reference_level = audio_opts_in->reference_level;
                /* SNMP will have sanitised this */
                audio_stream->ts_opts.write_lang_code = 1;
                strcpy( audio_stream->ts_opts.lang_code, audio_opts_in->lang_code );
            }

            if( has_dvb_vbi )
            {
                obe_output_stream_t *dvb_vbi_stream = &d.output_streams[i];

                i++;
            }

            if( encoder_control->ancillary_opts->dvb_ttx_enabled )
            {
                obe_output_stream_t *dvb_ttx_stream = &d.output_streams[i];

                i++;
            }

            Obed__MuxOpts *mux_opts_in = encoder_control->mux_opts;
            mux_opts->ts_muxrate = mux_opts_in->muxrate;
            mux_opts->ts_type = mux_opts_in->ts_type;
            mux_opts->cbr = mux_opts_in->null_packets;
            mux_opts->pcr_pid = mux_opts_in->pcr_pid;
            mux_opts->pmt_pid = mux_opts_in->pmt_pid;
            mux_opts->program_num = mux_opts_in->program_num;
            mux_opts->ts_id = mux_opts_in->ts_id;
            mux_opts->pat_period = mux_opts_in->pat_period;
            mux_opts->pcr_period = mux_opts_in->pcr_period;
            mux_opts->service_name = malloc( strlen( mux_opts_in->service_name ) + 1 );
            strcpy( mux_opts->service_name, mux_opts_in->service_name );
            mux_opts->provider_name = malloc( strlen( mux_opts_in->provider_name ) + 1 );
            strcpy( mux_opts->provider_name, mux_opts_in->provider_name );


        }

    }

    result.encoder_response = malloc( 3 );
    strcpy( result.encoder_response, "OK" );
    closure( &result, closure_data );

    return;

fail:
    closure( NULL, closure_data );
}

static Obed__EncoderConfig_Service encoder_config = OBED__ENCODER_CONFIG__INIT(obed__);

int main( int argc, char **argv )
{
    ProtobufC_RPC_Server *server;
    ProtobufC_RPC_AddressType address_type = 0;
    const char *name = "/tmp/obesocket";

    /* Signal handlers */
    keep_running = 1;
    signal( SIGTERM, stop_server );
    signal( SIGINT, stop_server );

    server = protobuf_c_rpc_server_new( address_type, name, (ProtobufCService *) &encoder_config, NULL );

    while( keep_running )
    {
        /* FIXME valgrind issues */
        protobuf_c_dispatch_run( protobuf_c_dispatch_default() );
    }

    protobuf_c_rpc_server_destroy( server, 0 );

    return 0;
}
