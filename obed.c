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

#define OBE_CONTROL_VERSION 1

static char *ident = "obed0";

enum obe_quality_metric_e
{
    QUALITY_METRIC_PSY,
    QUALITY_METRIC_SSIM,
    QUALITY_METRIC_PSNR
};

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

    for( int i = 0; i < d.output.num_outputs; i++ )
    {
        if( d.output.outputs[i].target )
        {
            free( d.output.outputs[i].target );
            d.output.outputs[i].target = NULL;
        }
    }
    free( d.output.outputs );

    memset( &d, 0, sizeof(d) );
    running = 0;
}

static int add_teletext( obe_output_stream_t *output_stream, Obed__AncillaryOpts *ancillary_opts_in )
{
    output_stream->ts_opts.num_teletexts = 1;
    output_stream->ts_opts.teletext_opts = calloc( 1, sizeof(*output_stream->ts_opts.teletext_opts) );
    if( !output_stream->ts_opts.teletext_opts )
        return -1;

    obe_teletext_opts_t *ttx_opts = &output_stream->ts_opts.teletext_opts[0];
    /* Sanitised by SNMP */
    strcpy( ttx_opts->dvb_teletext_lang_code, ancillary_opts_in->ttx_lang_code );
    ttx_opts->dvb_teletext_type = ancillary_opts_in->ttx_type;
    ttx_opts->dvb_teletext_magazine_number = ancillary_opts_in->ttx_mag_number;
    ttx_opts->dvb_teletext_page_number = ancillary_opts_in->ttx_page_number;

    return 0;
}

static void obed__encoder_config( Obed__EncoderConfig_Service     *service,
                                  const Obed__EncoderControl      *encoder_control,
                                  Obed__EncoderResponse_Closure   closure,
                                  void                            *closure_data )
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    int has_dvb_vbi = 0;
    int i = 0;

    if( encoder_control->control_version == OBE_CONTROL_VERSION )
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
            Obed__MuxOpts *mux_opts_in = encoder_control->mux_opts;
            obe_mux_opts_t *mux_opts = &d.mux_opts;

            d.h = obe_setup( (const char *)ident );
            if( !d.h )
                goto fail;

            input_opts_out->input_type = input_opts_in->input_device;
            input_opts_out->card_idx = input_opts_in->card_idx;
            input_opts_out->video_format = input_opts_in->video_format;

            printf("\n %i \n", input_opts_out->input_type );

            if( obe_probe_device( d.h, &d.input, &d.program ) < 0 )
            {
                // TODO: syslog error message
                goto fail;
            }

            if( !d.program.num_streams )
                goto fail;

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
                for( i = 0; i < d.program.num_streams; i++ )
                {
                    if( d.program.streams[i].stream_format == VBI_RAW )
                    {
                        d.num_output_streams += 1;
                        break;
                    }
                }
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

            if( video_opts_in->quality_metric )
            {
                if( video_opts_in->quality_metric == QUALITY_METRIC_SSIM )
                {
                    video_stream->avc_param.rc.i_aq_mode = X264_AQ_AUTOVARIANCE;
                    video_stream->avc_param.analyse.b_psy = 0;
                }
                else
                {
                    video_stream->avc_param.rc.i_aq_mode = X264_AQ_NONE;
                    video_stream->avc_param.analyse.b_psy = 0;
                }
            }
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
                if( audio_opts_in->format == 0 )
                    audio_stream->stream_format = AUDIO_MP2;
                else
                {
                    /* AAC */
                    audio_stream->stream_format = AUDIO_AAC;
                    /* XXX: note the enums */
                    audio_stream->aac_opts.aac_profile = audio_opts_in->format-1;
                    audio_stream->aac_opts.latm_output = audio_opts_in->aac_encap;
                }
                audio_stream->ts_opts.pid = audio_opts_in->pid;

                audio_stream->channel_layout = channel_layouts[audio_opts_in->channel_map];
                audio_stream->bitrate = audio_opts_in->bitrate;
                audio_stream->sdi_audio_pair = audio_opts_in->sdi_pair;
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
                obe_dvb_vbi_opts_t *vbi_opts = &dvb_vbi_stream->dvb_vbi_opts;

                dvb_vbi_stream->ts_opts.pid = ancillary_opts_in->dvb_vbi_pid;
                vbi_opts->ttx = ancillary_opts_in->dvb_vbi_ttx;
                vbi_opts->inverted_ttx = ancillary_opts_in->dvb_vbi_inverted_ttx;
                vbi_opts->vps = ancillary_opts_in->dvb_vbi_vps;
                vbi_opts->wss = ancillary_opts_in->dvb_vbi_wss;

                /* Only one teletext supported */
                if( vbi_opts->ttx )
                {
                    if( add_teletext( dvb_vbi_stream, ancillary_opts_in ) < 0 )
                        goto fail;
                }

                i++;
            }

            if( encoder_control->ancillary_opts->dvb_ttx_enabled )
            {
                obe_output_stream_t *dvb_ttx_stream = &d.output_streams[i];
                /* Only one teletext supported */
                if( add_teletext( dvb_ttx_stream, ancillary_opts_in ) < 0 )
                    goto fail;
                i++;
            }

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

            d.output.num_outputs = encoder_control->n_output_opts;
            d.output.outputs = calloc( d.output.num_outputs, sizeof(*d.output.outputs) );
            if( !d.output.outputs )
                goto fail;

            for( int j = 0; j < d.output.num_outputs; j++ )
            {
                Obed__OutputOpts *output_opts_in = encoder_control->output_opts[j];
                obe_output_dest_t *output_dst = &d.output.outputs[j];
                char tmp[500];
                char tmp2[20];

                output_dst->type = output_opts_in->method;
                strcpy( tmp, "udp://" );
                strcat( tmp, output_opts_in->ip_address );
                snprintf( tmp2, sizeof(tmp2), ":%d?", output_opts_in->port );
                strcat( tmp, tmp2 );
                if( output_opts_in->ttl )
                {
                    snprintf( tmp2, sizeof(tmp2), "ttl=%d&", output_opts_in->ttl );
                    strcat( tmp, tmp2 );
                }
                if( output_opts_in->miface )
                {
                    snprintf( tmp2, sizeof(tmp2), "miface=%s", output_opts_in->miface );
                    strcat( tmp, tmp2 );
                }
                output_dst->target = malloc( strlen( tmp ) + 1 );
                if( output_dst->target )
                    goto fail;
                strcpy( output_dst->target, tmp );
            }

            printf("\n end \n");
            //start encoder
            //running = 1;
        }
    }

    result.encoder_response = malloc( 3 );
    strcpy( result.encoder_response, "OK" );
    closure( &result, closure_data );

    return;

fail:
    result.encoder_response = malloc( 5 );
    strcpy( result.encoder_response, "FAIL" );
    closure( &result, closure_data );
    return;
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
