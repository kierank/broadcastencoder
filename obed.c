/*****************************************************************************
 * obed.c: Open Broadcast Encoder daemon
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
#include <syslog.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h> /* htonl */

#include <nacl/crypto_sign.h>

#include "config.h"

#include <signal.h>
#define _GNU_SOURCE

#include "obe.h"
#ifdef PROTOBUF_C_1_0
#include <protobuf-c-rpc/protobuf-c-rpc.h>
# define protobuf_c_dispatch_run protobuf_c_rpc_dispatch_run
# define protobuf_c_dispatch_default protobuf_c_rpc_dispatch_default
# define protobuf_c_dispatch_add_idle protobuf_c_rpc_dispatch_add_idle
# define protobuf_c_dispatch_destroy_default protobuf_c_rpc_dispatch_destroy_default
# define protobuf_c_dispatch_dispatch protobuf_c_rpc_dispatch_dispatch

# define ProtobufCDispatch ProtobufCRPCDispatch
# define ProtobufC_FDNotify ProtobufC_RPC_FDNotify

# define PROTOBUF_C_EVENT_WRITABLE PROTOBUF_C_RPC_EVENT_WRITABLE
# define PROTOBUF_C_EVENT_READABLE PROTOBUF_C_RPC_EVENT_READABLE
#else
#include <google/protobuf-c/protobuf-c-rpc.h>
#endif

#include "proto/obed.pb-c.h"

#define OBE_CONTROL_VERSION 1

static char socket_name[100];
static char ident[100];

#define MAX_OUTPUTS 8

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

static int running = 0;
int encoder_id = 0;
static int64_t auth_time;
static int auth;

/* Video */
const static uint64_t channel_layouts[] =
{
    AV_CH_LAYOUT_MONO,
    AV_CH_LAYOUT_STEREO,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_5POINT1_BACK,
};

const static int video_formats[] =
{
    INPUT_VIDEO_FORMAT_PAL,
    INPUT_VIDEO_FORMAT_NTSC,
    INPUT_VIDEO_FORMAT_720P_50,
    INPUT_VIDEO_FORMAT_720P_5994,
    INPUT_VIDEO_FORMAT_1080I_50,
    INPUT_VIDEO_FORMAT_1080I_5994,
    INPUT_VIDEO_FORMAT_1080P_2398,
    INPUT_VIDEO_FORMAT_1080P_24,
    INPUT_VIDEO_FORMAT_1080P_25,
    INPUT_VIDEO_FORMAT_720P_60,
    INPUT_VIDEO_FORMAT_1080P_50,
    INPUT_VIDEO_FORMAT_1080P_5994,
    INPUT_VIDEO_FORMAT_1080P_60,
    INPUT_VIDEO_FORMAT_1080P_2997,
    INPUT_VIDEO_FORMAT_1080P_30,
    -1,
};

static int get_return_format( int video_format )
{
    int i = 0;
    for( ; video_formats[i] != -1; i++ )
    {
        if( video_formats[i] == video_format )
            break;
    }

    return i;
}

const static int s302m_bit_depths[] =
{
    16,
    20,
    24
};

/* Note: Agent avoids using Linsys card */
enum obed_input_type_e
{
    OBED_INPUT_DECKLINK = 1,
    OBED_INPUT_LINSYS, /* unused */
    OBED_INPUT_BARS,
    OBED_INPUT_2022_6,
    OBED_INPUT_2022_7,
    OBED_INPUT_2110,
    OBED_INPUT_2110_DASH_7,
    OBED_INPUT_OBE_SDI
};

static int is_2110( int input_device )
{
    return input_device == OBED_INPUT_2110   ||
           input_device == OBED_INPUT_2110_DASH_7;
}

static int is_netmap( int input_device )
{
    return input_device == OBED_INPUT_2022_6 ||
           input_device == OBED_INPUT_2022_7 ||
           is_2110( input_device ) ||
           input_device == OBED_INPUT_OBE_SDI;
}

static struct in_addr intf_addr(const char *intf)
{
    struct in_addr addr = {0};

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) < 0) {
        perror("getifaddrs");
        return addr;
    }

    for (struct ifaddrs *ifap = ifa; ifap; ifap = ifap->ifa_next) {
        if (!strncmp(ifap->ifa_name, intf, IFNAMSIZ)) {
            if (ifap->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ifap->ifa_addr;
                addr = sin->sin_addr;
                break;
            }
        }
    }

    freeifaddrs(ifa);

    return addr;
}

/* server options */
static volatile int keep_running;

static void stop_server( int a )
{
    keep_running = 0;
}

/* Authentication options */
#ifdef C100
static const unsigned char pk[] = { 0x7c, 0x59, 0x1a, 0xb3, 0xb9, 0xe2, 0x9d, 0xfc,
                                    0xdb, 0x54, 0x7f, 0xf4, 0x09, 0xc9, 0xc6, 0x46,
                                    0xaa, 0x92, 0xcf, 0xe2, 0xde, 0x6e, 0x70, 0xc6,
                                    0x09, 0x61, 0x8f, 0x86, 0x6d, 0x22, 0x7b, 0x74 };
#else
static const unsigned char pk[] = { 0x99, 0x29, 0xfc, 0x17, 0x90, 0x96, 0xb3, 0xe1,
                                    0xb2, 0xe1, 0x93, 0x93, 0x76, 0x7e, 0x65, 0x30,
                                    0xda, 0xa,  0x49, 0x92, 0xe8, 0xc9, 0x7e, 0x54,
                                    0xd0, 0xe4, 0x81, 0x3a, 0x46, 0xc3,  0xc, 0xb7 };
#endif

static void stop_encode( void )
{
    obe_close( d.h );
    d.h = NULL;

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

static void obed__encoder_config( Obed__EncoderCommunicate_Service *service,
                                  const Obed__EncoderControl      *encoder_control,
                                  Obed__EncoderResponse_Closure   closure,
                                  void                            *closure_data )
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    int has_dvb_vbi = 0, has_dvb_ttx = 0;
    int i = 0;

    if( encoder_control->control_version == OBE_CONTROL_VERSION )
    {
        /* Don't do anything if the client is not authenticated */
        if( !auth )
            return;

        if( running == 1 )
            stop_encode();

        if( !strcmp( encoder_control->encoder_action, "START" ) )
        {
            /* only messages with all options are legal currently */
            Obed__InputOpts *input_opts_in = encoder_control->input_opts;
            obe_input_t *input_opts_out = &d.input;
            Obed__VideoOpts *video_opts_in = encoder_control->video_opts;

            d.h = obe_setup( (const char *)ident );
            if( !d.h )
                goto fail;

            if( is_netmap( input_opts_in->input_device ) )
            {
                input_opts_out->input_type = INPUT_DEVICE_NETMAP;
                if( input_opts_in->input_device == OBED_INPUT_OBE_SDI )
                {
                    snprintf( input_opts_out->netmap_uri, sizeof(input_opts_out->netmap_uri), "/dev/pcie_sdi%u", input_opts_in->card_idx );
                }
                else
                {
                    snprintf( input_opts_out->netmap_uri, sizeof(input_opts_out->netmap_uri), "netmap:obe" "%u" "_path1}0+netmap:obe" "%u" "_path2}0", encoder_id, encoder_id );
                }

                if( is_2110( input_opts_in->input_device ) )
                {
                    if( input_opts_in->path1_ptp_nic )
                        snprintf( input_opts_out->ptp_nic, sizeof(input_opts_out->ptp_nic), "%s|%s", input_opts_in->path1_ptp_nic, input_opts_in->path2_ptp_nic ? input_opts_in->path2_ptp_nic : "" );
                    strncpy( input_opts_out->netmap_mode, "rfc4175", sizeof(input_opts_out->netmap_mode) );
                    struct in_addr path1_addr = intf_addr(input_opts_in->path1_ptp_nic);
                    struct in_addr path2_addr = intf_addr(input_opts_in->path2_ptp_nic);
                    char tmp[17], tmp2[17];

                    /* inet_ntoa writes to a static buffer so can't use it twice */
                    strncpy(tmp, inet_ntoa(path1_addr), sizeof(tmp));
                    strncpy(tmp2, inet_ntoa(path2_addr), sizeof(tmp));

                    for( i = 0; i < encoder_control->n_audio_input_opts; i++ )
                    {
                        char tmp3[150];
                        Obed__AudioInputOpts *audio_input_opts = encoder_control->audio_input_opts[i];

                        if( i == 0 )
                            input_opts_out->netmap_audio_channels = audio_input_opts->channels;
                        else
                            strncat( input_opts_out->netmap_audio, ";", 1 );

                        snprintf( tmp3, sizeof(tmp3), "%s@%s:%u/ifaddr=%s|%s@%s:%u/ifaddr=%s",
                                  audio_input_opts->path1_source_ip_address ? audio_input_opts->path1_source_ip_address : "",
                                  audio_input_opts->path1_ip_address ? audio_input_opts->path1_ip_address : "",
                                  audio_input_opts->path1_port,
                                  tmp,
                                  audio_input_opts->path2_source_ip_address ? audio_input_opts->path2_source_ip_address : "",
                                  audio_input_opts->path2_ip_address ? audio_input_opts->path2_ip_address : "",
                                  audio_input_opts->path2_port,
                                  tmp2 );
                        strncat( input_opts_out->netmap_audio, tmp3, sizeof(tmp3) ); // max 8 channels
                    }
                }
            }
            else
                input_opts_out->input_type = input_opts_in->input_device;

            input_opts_out->card_idx = input_opts_in->card_idx;
            input_opts_out->video_format = video_formats[input_opts_in->video_format];
            if( input_opts_in->bars_line1 )
                strncpy( input_opts_out->bars_line1, input_opts_in->bars_line1, sizeof(input_opts_out->bars_line1) );
            if( input_opts_in->bars_line2 )
                strncpy( input_opts_out->bars_line2, input_opts_in->bars_line2, sizeof(input_opts_out->bars_line2) );
            if( input_opts_in->bars_line3 )
                strncpy( input_opts_out->bars_line3, input_opts_in->bars_line3, sizeof(input_opts_out->bars_line3) );
            if( input_opts_in->bars_line4 )
                strncpy( input_opts_out->bars_line4, input_opts_in->bars_line4, sizeof(input_opts_out->bars_line4) );
            input_opts_out->picture_on_loss = input_opts_in->picture_on_signal_loss;
            input_opts_out->downscale = input_opts_in->has_sd_downscale && input_opts_in->sd_downscale;
            if( input_opts_in->has_bars_clapper )
                input_opts_out->bars_beep = input_opts_in->bars_clapper;
            if( input_opts_in->has_bars_clapper_interval )
                input_opts_out->bars_beep_interval = input_opts_in->bars_clapper_interval;

            int bit_depth = OBE_BIT_DEPTH_10;
            if( input_opts_in->has_sd_downscale && input_opts_in->sd_downscale )
                bit_depth = OBE_BIT_DEPTH_8;

            if( video_opts_in->latency == 1 )
            {
                if( obe_set_config( d.h, OBE_SYSTEM_TYPE_LOWEST_LATENCY, bit_depth ) < 0 )
                {
                    syslog( LOG_ERR, "Error setting latency" );
                    goto fail;
                }
            }
            else if( video_opts_in->latency == 2 || video_opts_in->latency == 3 )
            {
                if( obe_set_config( d.h, OBE_SYSTEM_TYPE_LOW_LATENCY, bit_depth ) < 0 )
                {
                    syslog( LOG_ERR, "Error setting latency" );
                    goto fail;
                }
            }

            if( obe_autoconf_device( d.h, &d.input, &d.program ) < 0 )
            {
                syslog( LOG_ERR, "Input device could not be opened" );
                goto fail;
            }

            obe_output_stream_t *video_stream = &d.output_streams[0];
            Obed__AncillaryOpts *ancillary_opts_in = encoder_control->ancillary_opts;
            Obed__MuxOpts *mux_opts_in = encoder_control->mux_opts;
            obe_mux_opts_t *mux_opts = &d.mux_opts;

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
                        has_dvb_vbi = 1;
                        break;
                    }
                }
            }

            /* Add SCTE-35 PID */
            d.num_output_streams += encoder_control->ancillary_opts->has_scte35_enabled && encoder_control->ancillary_opts->scte35_enabled;

            /* Add SMPTE-2038 PID */
            d.num_output_streams += encoder_control->ancillary_opts->has_smpte2038_enabled && encoder_control->ancillary_opts->smpte2038_enabled;

            d.output_streams = calloc( d.num_output_streams, sizeof(*d.output_streams) );
            if( !d.output_streams )
                goto fail;

            video_stream = &d.output_streams[0];

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
            if( video_opts_in->width )
                video_stream->avc_param.i_width         = video_opts_in->width;
            video_stream->is_wide                       = video_opts_in->aspect_ratio;

            if( input_opts_in->has_sd_downscale && input_opts_in->sd_downscale )
            {
                if( input_opts_out->video_format == INPUT_VIDEO_FORMAT_1080P_50 )
                {
                    video_stream->avc_param.i_width = 1280;
                    video_stream->avc_param.i_height = 720;
                }
                else
                {
                    video_stream->avc_param.i_width = 720;
                    video_stream->avc_param.i_height = 576;
                }
            }

#ifdef C100
            if( video_opts_in->latency == 0 )
            {
                /* Reduce CPU usage in C-100 */
                video_stream->avc_param.sc.max_preset = 1;
            }
            else
#endif
            if( video_opts_in->latency == 1 || video_opts_in->latency == 2 ||
                     video_opts_in->latency == 3 )
            {
                if( video_opts_in->latency == 1 || video_opts_in->latency == 2 )
                    video_stream->avc_param.b_intra_refresh = 1;
            }

            if( video_opts_in->has_threads )
                video_stream->avc_param.i_threads = video_opts_in->threads;
            else
                video_stream->avc_param.i_threads = 8;

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

            /* Video frame ancillary data */
            video_stream->video_anc.afd                 = video_opts_in->afd_passthrough;
            video_stream->video_anc.wss_to_afd          = video_opts_in->wss_to_afd;
            video_stream->video_anc.cea_608             = ancillary_opts_in->cea_608;
            video_stream->video_anc.cea_708             = ancillary_opts_in->cea_708;

            video_stream->avc_param.i_csp = X264_CSP_I420;
            /* Setup video profile */
            if( d.avc_profile == 0 )
                x264_param_apply_profile( &video_stream->avc_param, "main" );
            else if( d.avc_profile == 1 )
                x264_param_apply_profile( &video_stream->avc_param, "high" );
            else if( d.avc_profile == 2 )
            {
                video_stream->avc_param.i_csp = X264_CSP_I422;
                x264_param_apply_profile( &video_stream->avc_param, "high422" );
            }

            video_stream->avc_param.i_nal_hrd = X264_NAL_HRD_FAKE_VBR;
            if( video_opts_in->has_filler && video_opts_in->filler )
            {
                video_stream->avc_param.i_nal_hrd = X264_NAL_HRD_FAKE_CBR;
                video_stream->avc_param.rc.b_filler = 0; // FIXME, turning this on causes mux issues but does it matter?
            }

            if( video_opts_in->has_flip )
                video_stream->flip = video_opts_in->flip;

            /* HOTFIX: Disable adaptive bframes and one bframe maximum for p50/60 */
            if( video_stream->avc_param.i_fps_num == 50 ||
                video_stream->avc_param.i_fps_num == 60000 ||
                video_stream->avc_param.i_fps_num == 60 )
            {
                video_stream->avc_param.i_bframe_adaptive = 0;
                video_stream->avc_param.i_bframe = video_stream->avc_param.i_bframe ? 1 : 0;
            }

            /* Setup audio streams */
            for( i = 1; i <= encoder_control->n_audio_opts; i++ )
            {
                int j = i-1;
                Obed__AudioOpts *audio_opts_in = encoder_control->audio_opts[j];
                obe_output_stream_t *audio_stream = &d.output_streams[i];

                audio_stream->input_stream_id = 1;
                audio_stream->output_stream_id = i;
                audio_stream->stream_action = STREAM_ENCODE;
                if( audio_opts_in->format == 0 )
                    audio_stream->stream_format = AUDIO_MP2;
                else if( audio_opts_in->format == 1 )
                {
                    audio_stream->stream_format = AUDIO_AAC;
                    audio_stream->aac_opts.aac_profile = 0;
                    audio_stream->aac_opts.latm_output = audio_opts_in->aac_encap;
                }
                else if( audio_opts_in->format == 2 )
                    audio_stream->stream_format = AUDIO_OPUS;
                else if( audio_opts_in->format == 3 )
                    audio_stream->stream_format = AUDIO_S302M;
#ifndef C100
                else
                {
                    audio_stream->stream_format = AUDIO_AC_3;
                    audio_stream->stream_action = STREAM_PASSTHROUGH;
                }
#endif
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
                if( audio_opts_in->has_s302m_bit_depth )
                    audio_stream->bit_depth = s302m_bit_depths[audio_opts_in->s302m_bit_depth];

                if( audio_opts_in->has_s302m_pairs )
                    audio_stream->num_pairs = audio_opts_in->s302m_pairs;
            }

            if( has_dvb_vbi )
            {
                obe_output_stream_t *dvb_vbi_stream = &d.output_streams[i];
                dvb_vbi_stream->input_stream_id = 2;
                dvb_vbi_stream->output_stream_id = i;
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

            has_dvb_ttx = encoder_control->ancillary_opts->dvb_ttx_enabled;
            if( has_dvb_ttx )
            {
                obe_output_stream_t *dvb_ttx_stream = &d.output_streams[i];
                dvb_ttx_stream->stream_format = MISC_TELETEXT;
                dvb_ttx_stream->input_stream_id = 2;
                dvb_ttx_stream->output_stream_id = i;

                dvb_ttx_stream->ts_opts.pid = ancillary_opts_in->dvb_ttx_pid;
                /* Only one teletext supported */
                if( add_teletext( dvb_ttx_stream, ancillary_opts_in ) < 0 )
                    goto fail;
                i++;
            }

            if( encoder_control->ancillary_opts->has_scte35_enabled && encoder_control->ancillary_opts->scte35_enabled )
            {
                obe_output_stream_t *scte35_stream = &d.output_streams[i];
                scte35_stream->stream_format = MISC_SCTE35;
                scte35_stream->input_stream_id = 3;
                scte35_stream->output_stream_id = i;

                scte35_stream->ts_opts.pid = ancillary_opts_in->scte35_pid;

                if( encoder_control->ancillary_opts->has_scte_tcp_enabled && encoder_control->ancillary_opts->scte_tcp_enabled &&
                    encoder_control->ancillary_opts->scte_tcp_address )
                {
                    strncpy( scte35_stream->scte_tcp_address, encoder_control->ancillary_opts->scte_tcp_address, sizeof(scte35_stream->scte_tcp_address) );
                }

                i++;
            }

            if( encoder_control->ancillary_opts->has_smpte2038_enabled && encoder_control->ancillary_opts->smpte2038_enabled )
            {
                obe_output_stream_t *smpte2038_stream = &d.output_streams[i];
                smpte2038_stream->stream_format = ANC_RAW;
                smpte2038_stream->input_stream_id = 4;
                smpte2038_stream->output_stream_id = i;

                smpte2038_stream->ts_opts.pid = ancillary_opts_in->smpte2038_pid;

                i++;
            }

            if( encoder_control->ancillary_opts->has_timecode && encoder_control->ancillary_opts->timecode )
            {
                if( input_opts_out->video_format == INPUT_VIDEO_FORMAT_PAL || input_opts_out->video_format == INPUT_VIDEO_FORMAT_NTSC )
                    input_opts_out->tc_source = TC_SOURCE_VITC;
                else
                    input_opts_out->tc_source = TC_SOURCE_RP188;
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
                    snprintf( tmp2, sizeof(tmp2), "iface=%s&", output_opts_in->miface );
                    strcat( tmp, tmp2 );
                }
                if( output_opts_in->tos )
                {
                    snprintf( tmp2, sizeof(tmp2), "tos=%i&", output_opts_in->tos );
                    strcat( tmp, tmp2 );
                }

                if (output_opts_in->has_arq_buffer)
                    output_dst->arq_latency = output_opts_in->arq_buffer;

                output_dst->target = strdup( tmp );
                if( !output_dst->target )
                    goto fail;
                if( output_opts_in->fec_type > 0 )
                {
                    /* Offset by 1 */
                    output_dst->fec_type = output_opts_in->fec_type-1;
                    output_dst->fec_columns = output_opts_in->fec_columns;
                    output_dst->fec_rows = output_opts_in->fec_rows;
                }
                else
                {
                    output_dst->fec_columns = 0;
                    output_dst->fec_rows = 0;
                }

                if( output_opts_in->has_dup_delay && output_opts_in->dup_delay > 0 )
                {
                    output_dst->dup_delay = output_opts_in->dup_delay;
                }
            }

            obe_setup_streams( d.h, d.output_streams, d.num_output_streams );
            obe_setup_muxer( d.h, &d.mux_opts );
            obe_setup_output( d.h, &d.output );
            if( obe_start( d.h ) < 0 )
                goto fail;

            running = 1;
            printf("Encoding started \n");
        }
    }

    result.encoder_id = encoder_id;
    result.encoder_response = malloc( 3 );
    strcpy( result.encoder_response, "OK" );
    closure( &result, closure_data );
    free( result.encoder_response );

    return;

fail:
    result.encoder_id = encoder_id;
    result.encoder_response = malloc( 5 );
    strcpy( result.encoder_response, "FAIL" );
    closure( &result, closure_data );
    free( result.encoder_response );

    return;
}

static void obed__encoder_status(Obed__EncoderCommunicate_Service *service,
                                 const Obed__EncoderControl *input,
                                 Obed__EncoderStatusResponse_Closure closure,
                                 void *closure_data)
{
    Obed__EncoderStatusResponse result = OBED__ENCODER_STATUS_RESPONSE__INIT;
    obe_input_status_t input_status = {0};
    int arq_status[MAX_OUTPUTS];
    int32_t arq_status32[MAX_OUTPUTS];

    obe_input_status( d.h, &input_status );

    result.encoder_id = encoder_id;
    result.status_version = 1;
    result.has_input_active = 1;
    result.input_active = input_status.active;
    result.has_detected_video_format = 1;
    result.detected_video_format = input_status.active ? get_return_format( input_status.detected_video_format ) : -1;

    obe_get_arq_status( d.h, arq_status );

    result.n_arq_status = d.output.num_outputs;
    for( int i = 0; i < d.output.num_outputs; i++ )
        arq_status32[i] = arq_status[i];

    result.arq_status = arq_status32;

    closure( &result, closure_data );
}

static void obed__encoder_format(Obed__EncoderCommunicate_Service *service,
                                 const Obed__EncoderControl *input,
                                 Obed__EncoderFormatResponse_Closure closure,
                                 void *closure_data)
{
    Obed__EncoderFormatResponse result = OBED__ENCODER_FORMAT_RESPONSE__INIT;
    Obed__InputOpts *input_opts_in = input->input_opts;

    result.has_video_format = 1;
    result.video_format = -1;
    if( running )
    {
        result.video_format = get_return_format( d.program.streams[0].video_format );
    }
    else
    {
        obe_input_t *input_opts_out = &d.input;

        d.h = obe_setup( (const char *)ident );
        if( d.h )
        {
            input_opts_out->input_type = input_opts_in->input_device;
            input_opts_out->card_idx = input_opts_in->card_idx;
            input_opts_out->video_format = INPUT_VIDEO_FORMAT_AUTODETECT;

            int ret = obe_probe_device( d.h, &d.input, &d.program );

            if( !ret && d.program.num_streams )
            {
                result.video_format = get_return_format( d.program.streams[0].video_format );
            }

            stop_encode();
        }
    }

    result.encoder_id = encoder_id;
    result.format_version = 1;

    closure( &result, closure_data );
}

static void obed__encoder_auth1(Obed__EncoderCommunicate_Service *service,
                                const Obed__EncoderAuthenticate *input,
                                Obed__EncoderResponse_Closure closure,
                                void *closure_data)
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    struct timeval tv;

    gettimeofday( &tv, NULL );

    result.has_encoder_time = 1;
    auth_time = result.encoder_time = tv.tv_sec + rand();

    result.encoder_response = malloc( 3 );
    strcpy( result.encoder_response, "OK" );

    result.encoder_id = encoder_id;

    closure( &result, closure_data );
    free( result.encoder_response );
}

static void obed__encoder_auth2(Obed__EncoderCommunicate_Service *service,
                                const Obed__EncoderAuthenticate *input,
                                Obed__EncoderResponse_Closure closure,
                                void *closure_data)
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    char tmp[100];
    uint8_t tmp2[100];
    int ret;
    unsigned long long mlen;

    if( !input->has_signed_message )
    {
        result.encoder_response = malloc( 5 );
        strcpy( result.encoder_response, "FAIL" );
        goto end;
    }

    /* Run cryptographic check */
    ret = crypto_sign_open( tmp2, &mlen, input->signed_message.data, input->signed_message.len, pk );
    snprintf( tmp, sizeof(tmp), "AUTH%"PRIi64"", auth_time );

    if( ret < 0 || memcmp( tmp, tmp2, mlen ) )
    {
        result.encoder_response = malloc( 5 );
        strcpy( result.encoder_response, "FAIL" );
    }
    else
    {
        auth = 1;
        result.encoder_response = malloc( 3 );
        strcpy( result.encoder_response, "OK" );
    }

    result.encoder_id = encoder_id;
end:

    closure( &result, closure_data );
    free( result.encoder_response );
}

static void obed__encoder_bitrate_reconfig(Obed__EncoderCommunicate_Service *service,
                                           const Obed__EncoderBitrateControl *input,
                                           Obed__EncoderResponse_Closure closure,
                                           void *closure_data)
{
    Obed__EncoderResponse result = OBED__ENCODER_RESPONSE__INIT;
    obe_output_stream_t *video_stream = &d.output_streams[0];

    if( running )
    {
        video_stream->avc_param.rc.i_vbv_max_bitrate = video_stream->avc_param.rc.i_bitrate = input->bitrate;
        obe_update_stream( d.h, video_stream );
    }

    result.encoder_response = malloc( 3 );
    strcpy( result.encoder_response, "OK" );
    result.encoder_id = encoder_id;

    closure( &result, closure_data );
    free( result.encoder_response );
}

static Obed__EncoderCommunicate_Service encoder_communicate = OBED__ENCODER_COMMUNICATE__INIT(obed__);

int main( int argc, char **argv )
{
    ProtobufC_RPC_Server *server;
    ProtobufC_RPC_AddressType address_type = 0;
    const char *socket_name_format = "/tmp/obesocket%i";
    const char *ident_format = "obed%i";

    if( argc != 2 )
    {
        fprintf( stderr, "Error: Needs encoder id\n" );
        return -1;
    }

    encoder_id = atoi( argv[1] );
    snprintf( socket_name, sizeof(socket_name), socket_name_format, encoder_id );
    snprintf( ident, sizeof(ident), ident_format, encoder_id );

    /* Signal handlers */
    keep_running = 1;
    signal( SIGTERM, stop_server );
    signal( SIGINT, stop_server );
    // TODO SIGPIPE

    server = protobuf_c_rpc_server_new( address_type, socket_name, (ProtobufCService *) &encoder_communicate, NULL );
    fprintf( stderr, "RPC server activated\n" );

    while( keep_running )
    {
        /* FIXME valgrind issues */
        protobuf_c_dispatch_run( protobuf_c_dispatch_default() );
    }

    protobuf_c_rpc_server_destroy( server, 0 );

    return 0;
}
