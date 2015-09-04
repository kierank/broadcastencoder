/*****************************************************************************
 * ts.c: ts muxing functions
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
#include "mux/mux.h"
#include <libmpegts.h>

#define MIN_PID 0x30
#define MAX_PID 0x1fff

static const int mpegts_stream_info[][3] =
{
    { VIDEO_AVC,   LIBMPEGTS_VIDEO_AVC,      LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_MPEG2, LIBMPEGTS_VIDEO_MPEG2,    LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { AUDIO_MP2,   LIBMPEGTS_AUDIO_MPEG1,    LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { AUDIO_AC_3,  LIBMPEGTS_AUDIO_AC3,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_E_AC_3,  LIBMPEGTS_AUDIO_EAC3,   LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_S302M,   LIBMPEGTS_AUDIO_302M,   LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_AAC,     LIBMPEGTS_AUDIO_ADTS,   LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { AUDIO_AAC,     LIBMPEGTS_AUDIO_LATM,   LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { AUDIO_OPUS,    LIBMPEGTS_AUDIO_OPUS,   LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { SUBTITLES_DVB, LIBMPEGTS_DVB_SUB,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { MISC_TELETEXT, LIBMPEGTS_DVB_TELETEXT, LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { VBI_RAW,       LIBMPEGTS_DVB_VBI,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { -1, -1, -1 },
};

static const int vbi_service_ids[][2] =
{
    { MISC_TELETEXT, LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_TTX },
    { MISC_WSS,      LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_WSS },
    { MISC_VPS,      LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_VPS },
    { 0, 0 },
};

static const int avc_profiles[][2] =
{
    { 66,  AVC_BASELINE },
    { 77,  AVC_MAIN },
    { 100, AVC_HIGH },
    { 110, AVC_HIGH_10 },
    { 122, AVC_HIGH_422 },
    { 244, AVC_HIGH_444_PRED },
    { 0, 0 },
};

static obe_output_stream_t *get_output_mux_stream( obe_mux_params_t *mux_params, int output_stream_id )
{
    for( int i = 0; i < mux_params->num_output_streams; i++ )
    {
        if( mux_params->output_streams[i].output_stream_id == output_stream_id )
            return &mux_params->output_streams[i];
    }
    return NULL;
}

static void encoder_wait( obe_t *h, int output_stream_id )
{
    /* Wait for encoder to be ready */
    obe_encoder_t *encoder = get_encoder( h, output_stream_id );
    pthread_mutex_lock( &encoder->queue.mutex );
    while( !encoder->is_ready )
        pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );
    pthread_mutex_unlock( &encoder->queue.mutex );
}

void *open_muxer( void *ptr )
{
    obe_mux_params_t *mux_params = ptr;
    obe_t *h = mux_params->h;
    obe_mux_opts_t *mux_opts = &h->mux_opts;
    int cur_pid = MIN_PID;
    int stream_format, video_pid = 0, video_found = 0, width = 0,
    height = 0, has_dds = 0, len = 0, num_frames = 0;
    uint8_t *output;
    int64_t first_video_pts = -1, video_dts, first_video_real_pts = -1;
    int64_t *pcr_list;
    ts_writer_t *w;
    ts_main_t params = {0};
    ts_program_t program = {0};
    ts_stream_t *stream;
    ts_dvb_sub_t subtitles;
    ts_dvb_vbi_t *vbi_services;
    ts_frame_t *frames;
    obe_int_input_stream_t *input_stream;
    obe_output_stream_t *output_stream;
    obe_encoder_t *encoder;
    obe_muxed_data_t *muxed_data;
    obe_coded_frame_t *coded_frame;
    char *service_name = "OBE Service";
    char *provider_name = "Open Broadcast Encoder";

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_RR, &param );

    // TODO sanity check the options

    params.ts_type = mux_opts->ts_type;
    params.cbr = !!mux_opts->cbr;
    params.muxrate = mux_opts->ts_muxrate;

    params.pcr_period = mux_opts->pcr_period;
    params.pat_period = mux_opts->pat_period;

    w = ts_create_writer();

    if( !w )
    {
        fprintf( stderr, "[ts] could not create writer\n" );
        return NULL;
    }

    params.num_programs = 1;
    params.programs = &program;
    program.is_3dtv = !!mux_opts->is_3dtv;
    // TODO more mux opts

    program.streams = calloc( mux_params->num_output_streams, sizeof(*program.streams) );
    if( !program.streams )
    {
        fprintf( stderr, "malloc failed\n" );
        goto end;
    }

    program.num_streams = mux_params->num_output_streams;
    params.ts_id = mux_opts->ts_id ? mux_opts->ts_id : 1;
    program.program_num = mux_opts->program_num ? mux_opts->program_num : 1;
    program.pmt_pid = mux_opts->pmt_pid ? mux_opts->pmt_pid : cur_pid++;
    /* PCR PID is done later once we know the video pid */

    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        output_stream = &mux_params->output_streams[i];
        input_stream = get_input_stream( h, output_stream->input_stream_id);
        stream_format = output_stream->stream_format;

        int j = 0;
        while( mpegts_stream_info[j][0] != -1 && stream_format != mpegts_stream_info[j][0] )
            j++;

        /* OBE does not distinguish between ADTS and LATM but MPEG-TS does */
        if( stream_format == AUDIO_AAC && ( output_stream->stream_action == STREAM_ENCODE && output_stream->aac_opts.latm_output ) )
            j++;

        stream->stream_format = mpegts_stream_info[j][1];
        stream->stream_id = mpegts_stream_info[j][2]; /* Note this is the MPEG-TS stream_id, not the OBE stream_id */

        output_stream->ts_opts.pid = stream->pid = output_stream->ts_opts.pid ? output_stream->ts_opts.pid : cur_pid++;
        if( input_stream->stream_type == STREAM_TYPE_AUDIO )
        {
            stream->write_lang_code = !!strlen( output_stream->ts_opts.lang_code );
            memcpy( stream->lang_code, output_stream->ts_opts.lang_code, 4 );
            stream->audio_type = output_stream->ts_opts.audio_type;
        }
        stream->has_stream_identifier = output_stream->ts_opts.has_stream_identifier;
        stream->stream_identifier = output_stream->ts_opts.stream_identifier;

        if( stream_format == VIDEO_AVC )
        {
            encoder_wait( h, output_stream->output_stream_id );

            width = output_stream->avc_param.i_width;
            height = output_stream->avc_param.i_height;
            video_pid = stream->pid;
        }
        else if( stream_format == AUDIO_MP2 )
            stream->audio_frame_size = (double)MP2_NUM_SAMPLES * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        else if( stream_format == AUDIO_AC_3 )
            stream->audio_frame_size = (double)AC3_NUM_SAMPLES * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        else if( stream_format == AUDIO_E_AC_3 || stream_format == AUDIO_AAC )
        {
            encoder_wait( h, output_stream->output_stream_id );
            encoder = get_encoder( h, output_stream->output_stream_id );
            stream->audio_frame_size = (double)encoder->num_samples * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        }
        else if( stream_format == AUDIO_OPUS )
            stream->audio_frame_size = (double)OPUS_NUM_SAMPLES * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
    }

    /* Video stream isn't guaranteed to be first so populate program parameters here */
    program.pcr_pid = mux_opts->pcr_pid ? mux_opts->pcr_pid : video_pid;

    program.sdt.service_type = height >= 720 ? DVB_SERVICE_TYPE_ADVANCED_CODEC_HD : DVB_SERVICE_TYPE_ADVANCED_CODEC_SD;
    program.sdt.service_name = mux_opts->service_name ? mux_opts->service_name : service_name;
    program.sdt.provider_name = mux_opts->provider_name ? mux_opts->provider_name : provider_name;

    if( ts_setup_transport_stream( w, &params ) < 0 )
    {
        fprintf( stderr, "[ts] Transport stream setup failed\n" );
        goto end;
    }

    if( mux_opts->ts_type == OBE_TS_TYPE_GENERIC || mux_opts->ts_type == OBE_TS_TYPE_DVB )
    {
        if( ts_setup_sdt( w ) < 0 )
        {
            fprintf( stderr, "[ts] SDT setup failed\n" );
            goto end;
        }
    }

    /* setup any streams if necessary */
    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        output_stream = &mux_params->output_streams[i];
        input_stream = get_input_stream( h, output_stream->input_stream_id );
        encoder = get_encoder( h, output_stream->output_stream_id );
        stream_format = output_stream->stream_format;

        if( stream_format == VIDEO_AVC )
        {
            x264_param_t *p_param = &output_stream->avc_param;
            int j = 0;
            while( avc_profiles[j][0] && p_param->i_profile != avc_profiles[j][0] )
                j++;

            if( ts_setup_mpegvideo_stream( w, stream->pid, p_param->i_level_idc, avc_profiles[j][1], 0, 0, 0 ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup video stream\n" );
                goto end;
            }
        }
        else if( stream_format == AUDIO_AAC )
        {
            /* TODO: handle associated switching */
            int profile_and_level, num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

            if( num_channels > 2 )
                profile_and_level = output_stream->aac_opts.aac_profile == AAC_HE_V2 ? LIBMPEGTS_MPEG4_HE_AAC_V2_PROFILE_LEVEL_5 :
                                    output_stream->aac_opts.aac_profile == AAC_HE_V1 ? LIBMPEGTS_MPEG4_HE_AAC_PROFILE_LEVEL_5 :
                                                                                       LIBMPEGTS_MPEG4_AAC_PROFILE_LEVEL_5;
            else
                profile_and_level = output_stream->aac_opts.aac_profile == AAC_HE_V2 ? LIBMPEGTS_MPEG4_HE_AAC_V2_PROFILE_LEVEL_2 :
                                    output_stream->aac_opts.aac_profile == AAC_HE_V1 ? LIBMPEGTS_MPEG4_HE_AAC_PROFILE_LEVEL_2 :
                                                                                       LIBMPEGTS_MPEG4_AAC_PROFILE_LEVEL_2;

            /* T-STD ignores LFE channel */
            if( output_stream->channel_layout & AV_CH_LOW_FREQUENCY )
                num_channels--;

            if( ts_setup_mpeg4_aac_stream( w, stream->pid, profile_and_level, num_channels ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup AAC stream\n" );
                goto end;
            }
        }
        else if( stream_format == AUDIO_OPUS )
        {
            int channel_map = LIBMPEGTS_CHANNEL_CONFIG_STEREO; // FIXME

            if( ts_setup_opus_stream( w, stream->pid, channel_map ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup Opus stream\n" );
                goto end;
            }
        }
        else if( stream_format == SUBTITLES_DVB )
        {
            memcpy( subtitles.lang_code, input_stream->lang_code, 4 );
            subtitles.subtitling_type = input_stream->dvb_subtitling_type;
            subtitles.composition_page_id = input_stream->composition_page_id;
            subtitles.ancillary_page_id = input_stream->ancillary_page_id;
            /* A lot of streams don't have DDS flagged correctly so we assume all HD uses DDS */
            has_dds = width >= 1280 && height >= 720;
            if( ts_setup_dvb_subtitles( w, stream->pid, has_dds, 1, &subtitles ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup DVB Subtitle stream\n" );
                goto end;
            }
        }
        else if( stream_format == VBI_RAW || stream_format == MISC_TELETEXT )
        {
            if( output_stream->ts_opts.num_teletexts )
            {
                if( ts_setup_dvb_teletext( w, stream->pid, output_stream->ts_opts.num_teletexts,
                    (ts_dvb_ttx_t*)output_stream->ts_opts.teletext_opts ) < 0 )
                {
                    fprintf( stderr, "[ts] Could not setup Teletext stream\n" );
                    goto end;
                }
            }

            /* FIXME: let users specify VBI lines */
            if( stream_format == VBI_RAW && input_stream )
            {
                vbi_services = calloc( input_stream->num_frame_data, sizeof(*vbi_services) );
                if( !vbi_services )
                {
                    fprintf( stderr, "malloc failed\n" );
                    goto end;
                }

                for( int j = 0; j < input_stream->num_frame_data; j++ )
                {
                    for( int k = 0; vbi_service_ids[k][0] != 0; k++ )
                    {
                        if( input_stream->frame_data[j].type == vbi_service_ids[k][0] )
                            vbi_services[j].data_service_id = vbi_service_ids[k][1];
                    }

                    /* This check is not strictly necessary */
                    if( !vbi_services[j].data_service_id )
                        goto end;

                    vbi_services[j].num_lines = input_stream->frame_data[j].num_lines;
                    vbi_services[j].lines = malloc( vbi_services[j].num_lines * sizeof(*vbi_services[j].lines) );
                    if( !vbi_services[j].lines )
                    {
                        fprintf( stderr, "malloc failed\n" );
                        goto end;
                    }

                    for( int k = 0; k < input_stream->frame_data[j].num_lines; k++ )
                    {
                        int tmp_line, field;

                        obe_convert_smpte_to_analogue( input_stream->vbi_ntsc ? INPUT_VIDEO_FORMAT_NTSC : INPUT_VIDEO_FORMAT_PAL, input_stream->frame_data[j].lines[k],
                                                       &tmp_line, &field );

                        vbi_services[j].lines[k].field_parity = field == 1 ? 1 : 0;
                        vbi_services[j].lines[k].line_offset = tmp_line;
                    }
                }

                if( ts_setup_dvb_vbi( w, stream->pid, input_stream->num_frame_data, vbi_services ) < 0 )
                {
                    fprintf( stderr, "[ts] Could not setup VBI stream\n" );
                    goto end;
                }

                for( int j = 0; j < input_stream->num_frame_data; j++ )
                    free( vbi_services[j].lines );
                free( vbi_services );
            }
        }
    }

    //FILE *fp = fopen( "test.ts", "wb" );

    while( 1 )
    {
        video_found = 0;
        video_dts = 0;
        struct uchain *uchain, *uchain_tmp;

        pthread_mutex_lock( &h->mux_queue.mutex );

        if( h->cancel_mux_thread )
        {
            pthread_mutex_unlock( &h->mux_queue.mutex );
            goto end;
        }

        if( h->mux_params_update )
        {
            params.muxrate = mux_opts->ts_muxrate;
            ts_update_transport_stream( w, &params );
            h->mux_params_update = 0;
        }

        while( !video_found )
        {
            ulist_foreach( &h->mux_queue.ulist, uchain )
            {
                coded_frame = obe_coded_frame_t_from_uchain( uchain );
                if( coded_frame->is_video )
                {
                    video_found = 1;
                    video_dts = coded_frame->real_dts;
                    /* FIXME: handle case where first_video_pts < coded_frame->real_pts */
                    if( first_video_pts == -1 )
                    {
                        /* Get rid of frames which are too early */
                        first_video_pts = coded_frame->pts;
                        first_video_real_pts = coded_frame->real_pts;
                        remove_early_frames( h, first_video_pts );
                    }
                    break;
                }
            }

            if( !video_found )
                pthread_cond_wait( &h->mux_queue.in_cv, &h->mux_queue.mutex );

            if( h->cancel_mux_thread )
            {
                pthread_mutex_unlock( &h->mux_queue.mutex );
                goto end;
            }
        }

        int mux_queue_size = ulist_depth( &h->mux_queue.ulist );
        frames = calloc( mux_queue_size, sizeof(*frames) );
        if( !frames )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            pthread_mutex_unlock( &h->mux_queue.mutex );
            goto end;
        }

        //printf("\n START - queuelen %i \n", h->mux_queue.size);

        num_frames = 0;
        ulist_delete_foreach( &h->mux_queue.ulist, uchain, uchain_tmp )
        {
            coded_frame = obe_coded_frame_t_from_uchain( uchain );
            output_stream = get_output_mux_stream( mux_params, coded_frame->output_stream_id );
            // FIXME name
            int64_t rescaled_dts = coded_frame->pts - first_video_pts + first_video_real_pts;
            if( coded_frame->is_video )
                rescaled_dts = coded_frame->real_dts;

            //printf("\n stream-id %i ours: %"PRIi64" \n", coded_frame->output_stream_id, coded_frame->pts );

            if( rescaled_dts <= video_dts )
            {
                frames[num_frames].opaque = coded_frame;
                frames[num_frames].size = coded_frame->len;
                frames[num_frames].data = coded_frame->data;
                frames[num_frames].pid = output_stream->ts_opts.pid;
                if( coded_frame->is_video )
                {
                    frames[num_frames].cpb_initial_arrival_time = coded_frame->cpb_initial_arrival_time;
                    frames[num_frames].cpb_final_arrival_time = coded_frame->cpb_final_arrival_time;
                    frames[num_frames].dts = coded_frame->real_dts;
                    frames[num_frames].pts = coded_frame->real_pts;
                }
                else
                {
                    frames[num_frames].dts = coded_frame->pts - first_video_pts + first_video_real_pts;
                    frames[num_frames].pts = coded_frame->pts - first_video_pts + first_video_real_pts;
                    frames[num_frames].duration = coded_frame->duration;
                }

                frames[num_frames].dts /= 300;
                frames[num_frames].pts /= 300;

                //printf("\n pid: %i ours: %"PRIi64" \n", frames[num_frames].pid, frames[num_frames].dts );
                frames[num_frames].random_access = coded_frame->random_access;
                frames[num_frames].priority = coded_frame->priority;
                num_frames++;
                ulist_delete(uchain);
            }
        }

        pthread_mutex_unlock( &h->mux_queue.mutex );

        // TODO figure out last frame
        ts_write_frames( w, frames, num_frames, &output, &len, &pcr_list );

        if( len )
        {
            muxed_data = new_muxed_data( len );
            if( !muxed_data )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            memcpy( muxed_data->data, output, len );
            muxed_data->pcr_list = malloc( (len / 188) * sizeof(int64_t) );
            if( !muxed_data->pcr_list )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                destroy_muxed_data( muxed_data );
                goto end;
            }
            memcpy( muxed_data->pcr_list, pcr_list, (len / 188) * sizeof(int64_t) );
            add_to_queue( &h->mux_smoothing_queue, muxed_data );
        }

        for( int i = 0; i < num_frames; i++ )
            destroy_coded_frame( frames[i].opaque );

        free( frames );
    }

end:
    ts_close_writer( w );

    /* TODO: clean more */

    free( program.streams );
    free( ptr );

    return NULL;

}

const obe_mux_func_t ts_muxer = { open_muxer };
