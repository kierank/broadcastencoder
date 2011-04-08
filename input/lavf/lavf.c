/*****************************************************************************
 * lavf.c: libavformat input functions
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
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* TODO: add support for choosing programs */
#define MAX_PROGRAMS 1
/* FIXME: arbitrary number */
#define NUM_FRAMES_TO_SEARCH 20

/* FIXME: make 302M work */

#define obe_free_packet( pkt )\
{\
    av_free_packet( pkt );\
    av_init_packet( pkt );\
}

typedef struct
{
    int stream_id;
    int lavf_stream_idx;
} lavf_stream_lut;

struct lavf_status
{
    AVFormatContext **lavf;
    int status;
};

static lavf_stream_lut *find_stream_id( lavf_stream_lut *cur_lut, int lavf_stream_idx )
{
    int i = 0;

    while( cur_lut[i].stream_id != -1 && cur_lut[i].lavf_stream_idx != lavf_stream_idx )
        i++;

    return &cur_lut[i];
}

static lavf_stream_lut *find_lavf_stream_idx( lavf_stream_lut *cur_lut, int stream_id )
{
    int i = 0;

    while( cur_lut[i].stream_id != -1 && cur_lut[i].stream_id != stream_id )
        i++;

    return &cur_lut[i];
}

static obe_output_stream_t *get_stream( obe_input_params_t *input, int stream_id )
{
    for( int i = 0; i < input->num_output_streams; i++ )
    {
        if( input->output_streams[i].stream_id == stream_id )
            return &input->output_streams[i];
    }

    return NULL;
}

static void close_input( void *ptr )
{
    struct lavf_status *status = ptr;

    if( status->status )
        av_close_input_file( *status->lavf );
}

static int obe_get_buffer( AVCodecContext *codec, AVFrame *pic )
{
    int w = codec->width;
    int h = codec->height;
    int stride[4];

    avcodec_align_dimensions2( codec, &w, &h, stride );

    /* Only EDGE_EMU codecs are used */
    if( av_image_alloc( pic->data, pic->linesize, w, h, codec->pix_fmt, 16 ) < 0 )
        return -1;

    pic->age    = 256*256*256*64; /* FIXME is there a correct value for this? */
    pic->type   = FF_BUFFER_TYPE_USER;
    pic->reordered_opaque = codec->reordered_opaque;
    pic->pkt_pts = codec->pkt ? codec->pkt->pts : AV_NOPTS_VALUE;

    return 0;
}

static void obe_release_buffer( AVCodecContext *codec, AVFrame *pic )
{
     av_freep( &pic->data[0] );
     memset( pic->data, 0, sizeof(pic->data) );
}

static void release_video_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->img.plane[0] );
}

static void release_other_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->data );
}

static void release_frame( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;

     /* TODO: free user-data */

     free( raw_frame );
}

/* FFmpeg shouldn't call this */
static int obe_reget_buffer( AVCodecContext *codec, AVFrame *pic )
{
    if( pic->data[0] == NULL )
    {
        pic->buffer_hints |= FF_BUFFER_HINTS_READABLE;
        return codec->get_buffer( codec, pic );
    }

    pic->reordered_opaque = codec->reordered_opaque;
    pic->pkt_pts = codec->pkt ? codec->pkt->pts : AV_NOPTS_VALUE;

    return 0;
}

/* TODO: share some more code between probe_stream and open_stream */
void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = ptr;
    obe_t *h = probe_ctx->h;
    char *location = probe_ctx->location;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int num_streams = 0, frame_idx = 0, have_video = 0,
    lavf_idx = 0, ts_id = 0, program_num = 0, pmt_pid = 0, pcr_pid = 0;
    struct lavf_status status = {0};
    obe_device_t *device;
    lavf_stream_lut *stream_lut, *out_lut;

    AVFormatContext *lavf;
    AVProgram *cur_program;
    AVStream *stream;
    AVCodecContext *codec;
    AVMetadataTag *lang = NULL;
    AVCodec *dec;

    status.lavf = &lavf;

    AVPacket pkt;
    AVFrame frame;
    avcodec_get_frame_defaults( &frame );

    AVSubtitle subtitle;

    av_register_all();
    av_log_set_level( AV_LOG_QUIET );
    avcodec_init();

    pthread_cleanup_push( close_input, (void*)&status );

    if( av_open_input_file( &lavf, location, NULL, 0, NULL ) < 0 )
        return (void*)-1;

    if( av_find_stream_info( lavf ) < 0 )
        return (void*)-1;

    status.status = 1;

    for( int i = 0; i < lavf->nb_programs && i < MAX_PROGRAMS; i++ )
    {
        cur_program = lavf->programs[i];
        ts_id = lavf->ts_id;
        program_num = cur_program->program_num;
        pmt_pid = cur_program->pmt_pid;
        pcr_pid = cur_program->pcr_pid;

        // TODO SDT

        for( int j = 0; j < cur_program->nb_stream_indexes && j < MAX_STREAMS; j++ )
        {
            int idx = cur_program->stream_index[j];
            stream = lavf->streams[idx];
            codec = stream->codec;

            if( codec->codec_type == CODEC_TYPE_VIDEO || codec->codec_type == CODEC_TYPE_AUDIO ||
                codec->codec_type == CODEC_TYPE_SUBTITLE )
            {
                dec = avcodec_find_decoder( codec->codec_id );

                /* ignore video streams which don't support custom allocators */
                if( codec->codec_type == CODEC_TYPE_VIDEO && !( dec->capabilities & CODEC_CAP_DR1 ) )
                    continue;

                if( codec->codec_id != CODEC_ID_DVB_TELETEXT && avcodec_open( codec, dec ) < 0 )
                    continue;

                /* FIXME: write filter chain to allow 422 input and 10-bit input */
                if( codec->codec_type == CODEC_TYPE_VIDEO && !(codec->pix_fmt == PIX_FMT_YUV420P || codec->pix_fmt == PIX_FMT_YUVJ420P) )
                    continue;

                /* FIXME: ignore vfr streams for now */
                if( codec->codec_id == CODEC_ID_H264 && !codec->h264_fixed_frame_rate )
                    continue;

                streams[num_streams] = calloc( 1, sizeof(obe_int_input_stream_t) );
                if( !streams[num_streams] )
                    goto fail;

                pthread_mutex_lock( &h->device_list_mutex );
                streams[num_streams]->stream_id = h->cur_stream_id++;
                pthread_mutex_unlock( &h->device_list_mutex );

                streams[num_streams]->lavf_stream_idx = idx;

                /* container timebase */
                streams[num_streams]->transport_timebase_num = stream->time_base.num;
                streams[num_streams]->transport_timebase_den = stream->time_base.den;

                /* codec timebase */
                streams[num_streams]->timebase_num = codec->time_base.num;
                streams[num_streams]->timebase_den = codec->time_base.den / codec->ticks_per_frame;

                lang = av_metadata_get( lavf->streams[idx]->metadata, "language", NULL, AV_METADATA_IGNORE_SUFFIX );

                if( lang && strlen( lang->value ) >= 3 )
                {
                    memcpy( streams[num_streams]->lang_code, lang->value, 3 );
                    streams[num_streams]->lang_code[3] = 0;
                }

                streams[num_streams]->pid = stream->id;
                streams[num_streams]->has_stream_identifier = stream->has_stream_identifier;
                streams[num_streams]->stream_identifier = stream->stream_identifier;

                if( codec->codec_type == CODEC_TYPE_VIDEO )
                {
                    streams[num_streams]->stream_type = STREAM_TYPE_VIDEO;
                    if( codec->codec_id == CODEC_ID_H264 )
                        streams[num_streams]->stream_format = VIDEO_AVC;
                    else if( codec->codec_id == CODEC_ID_MPEG2VIDEO )
                        streams[num_streams]->stream_format = VIDEO_MPEG2;
                }
                else if( codec->codec_type == CODEC_TYPE_AUDIO )
                {
                    streams[num_streams]->stream_type = STREAM_TYPE_AUDIO;

                    if( codec->codec_id == CODEC_ID_AC3 )
                        streams[num_streams]->stream_format = AUDIO_AC_3;
#if 0
                    else if( codec->codec_id == CODEC_ID_EAC3 )
                        streams[num_streams]->stream_format = AUDIO_E_AC_3;
#endif
                    else if( codec->codec_id == CODEC_ID_MP3 )
                        streams[num_streams]->stream_format = AUDIO_MP2;
                    else if( codec->codec_id == CODEC_ID_AAC || codec->codec_id == CODEC_ID_AAC_LATM )
                    {
                        streams[num_streams]->stream_format = AUDIO_AAC;
                        streams[num_streams]->is_latm = codec->codec_id == CODEC_ID_AAC_LATM;
                        /* From the descriptor, if available */
                        streams[num_streams]->aac_profile_and_level = codec->aac_profile_and_level;
                        streams[num_streams]->aac_type = codec->aac_type;
                    }
                }
                else if( codec->codec_id == CODEC_ID_DVB_SUBTITLE && codec->extradata )
                {
                    streams[num_streams]->stream_type = STREAM_TYPE_SUBTITLE;
                    streams[num_streams]->stream_format = SUBTITLES_DVB;
                    /* The descriptor sometimes doesn't signal DDS so we need to probe it later */
                    streams[num_streams]->dvb_subtitling_type = codec->dvb_subtitling_type;
                    streams[num_streams]->composition_page_id = (codec->extradata[0] << 8) | codec->extradata[1];
                    streams[num_streams]->ancillary_page_id   = (codec->extradata[2] << 8) | codec->extradata[3];
                }
                else if( codec->codec_id == CODEC_ID_DVB_TELETEXT )
                {
                    streams[num_streams]->stream_type = STREAM_TYPE_MISC;
                    streams[num_streams]->stream_format = MISC_TELETEXT;
                    streams[num_streams]->dvb_teletext_type = codec->dvb_teletext_type;
                    streams[num_streams]->dvb_teletext_magazine_number = codec->dvb_teletext_magazine_number;
                    streams[num_streams]->dvb_teletext_page_number = codec->dvb_teletext_page_number;
                }
                num_streams++;
            }
        }
    }

    if( !num_streams )
        return NULL;

    for( int i = 0; i < num_streams; i++ )
    {
        if( streams[i]->stream_type == STREAM_TYPE_VIDEO )
            have_video = 1;
    }

    if( !have_video )
        goto fail;

    stream_lut = malloc( (num_streams+1) * sizeof(lavf_stream_lut) );
    if( !stream_lut )
        goto fail;

    for( int i = 0; i < num_streams; i++ )
    {
        stream_lut[i].stream_id = streams[i]->stream_id;
        stream_lut[i].lavf_stream_idx = streams[i]->lavf_stream_idx;
    }

    stream_lut[num_streams].stream_id = stream_lut[num_streams].lavf_stream_idx = -1;

    av_init_packet( &pkt );

    /* decode some packets to get correct information */
    while( frame_idx < num_streams * NUM_FRAMES_TO_SEARCH )
    {
        int finished = 0;
        int ret = 0;

        ret = av_read_frame( lavf, &pkt );
        codec = lavf->streams[pkt.stream_index]->codec;

        /* TODO: handle 302M */
        out_lut = find_stream_id( stream_lut, pkt.stream_index );
        if( out_lut->stream_id != -1 )
        {
            if( codec->codec_type == CODEC_TYPE_VIDEO )
            {
                if( ret < 0 )
                    pkt.size = 0;

                codec->reordered_opaque = pkt.pts;
                avcodec_decode_video2( codec, &frame, &finished, &pkt );
                if( finished )
                    frame_idx++;
            }
            else if( codec->codec_id == CODEC_ID_DVB_SUBTITLE )
            {
                avcodec_decode_subtitle2( codec, &subtitle, &finished, &pkt );
                if( finished )
                    frame_idx++;
            }
            else if( ret >= 0 )
                finished = 1;
        }
        obe_free_packet( &pkt );
    }

    /* update streams */
    for( int i = 0; i < num_streams; i++ )
    {
        lavf_idx = streams[i]->lavf_stream_idx;
        codec = lavf->streams[lavf_idx]->codec;

        streams[i]->bitrate = codec->bit_rate;

        if( codec->codec_type == CODEC_TYPE_VIDEO )
        {
            streams[i]->width  = codec->width;
            streams[i]->height = codec->height;
            streams[i]->csp    = codec->pix_fmt;
            streams[i]->interlaced = frame.interlaced_frame;
            streams[i]->tff = frame.top_field_first;
            streams[i]->sar_num = codec->sample_aspect_ratio.num;
            streams[i]->sar_den = codec->sample_aspect_ratio.den;
        }
        else if( codec->codec_type == CODEC_TYPE_AUDIO )
        {
            streams[i]->channel_layout = codec->channel_layout;
            streams[i]->sample_format = codec->sample_fmt;
            streams[i]->sample_rate = codec->sample_rate;
        }
        else if( codec->codec_id == CODEC_ID_DVB_SUBTITLE )
            streams[i]->has_dds = codec->dvb_sub_has_dds;

        // FIXME
        if( codec )
            avcodec_close( codec );
    }

    /* create device */
    device = new_device();

    if( !device )
        goto fail;

    device->num_input_streams = num_streams;
    memcpy( device->streams, streams, num_streams * sizeof(obe_int_input_stream_t**) );
    device->location = location;
    device->device_type = INPUT_URL;
    device->ts_id = ts_id;
    device->program_num = program_num;
    device->pmt_pid = pmt_pid;
    device->pcr_pid = pcr_pid;

    pthread_mutex_lock( &h->device_list_mutex );
    h->devices[h->num_devices++] = device;
    pthread_mutex_unlock( &h->device_list_mutex );

    pthread_cleanup_pop( 1 );

    return NULL;

fail:
    fprintf( stderr, "malloc failed \n" );

    for( int k = 0; k < num_streams; k++ )
        free( streams[k] );

    return NULL;
}

void *open_input( void *ptr )
{
    obe_input_params_t *input = ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    lavf_stream_lut *stream_lut, *out_lut;
    obe_output_stream_t *cur_stream;
    obe_coded_frame_t *coded_frame;
    obe_raw_frame_t *raw_frame;
    obe_encoder_t *encoder;
    struct lavf_status status = {0};

    int width;
    int height;
    int stride[4];

    AVFormatContext *lavf;
    AVCodecContext *codec;
    AVCodec *dec;
    AVPacket pkt;
    AVFrame frame;

    av_register_all();
    av_log_set_level( AV_LOG_QUIET );

    status.lavf = &lavf;

    pthread_cleanup_push( close_input, (void*)&status );

    if( av_open_input_file( &lavf, device->location, NULL, 0, NULL ) < 0 )
        return (void*)-1;

    if( av_find_stream_info( lavf ) < 0 )
        return (void*)-1;

    status.status = 1;

    stream_lut = calloc( 1, (input->num_output_streams+1) * sizeof(lavf_stream_lut) );
    if( !stream_lut )
        return (void*)-1;

    pthread_mutex_lock( &device->device_mutex );
    for( int i = 0; i < input->num_output_streams; i++ )
    {
        for( int j = 0; j < device->num_input_streams; j++ )
        {
            if( device->streams[j]->stream_id == input->output_streams[i].stream_id )
            {
                stream_lut[i].stream_id = device->streams[j]->stream_id;
                stream_lut[i].lavf_stream_idx = device->streams[j]->lavf_stream_idx;
            }
        }
    }
    pthread_mutex_unlock( &device->device_mutex );

    stream_lut[input->num_output_streams].stream_id = stream_lut[input->num_output_streams].lavf_stream_idx = -1;

    /* We assume the lavf stream_index will be the same along with all the metadata */

    /* Loop through streams and open decoder if necessary */
    for( int i = 0; i < input->num_output_streams; i++ )
    {
        if( input->output_streams[i].stream_action == STREAM_ENCODE )
        {
            out_lut = find_lavf_stream_idx( stream_lut, input->output_streams[i].stream_id );
            // TODO check fail
            codec = lavf->streams[out_lut->lavf_stream_idx]->codec;
            dec = avcodec_find_decoder( codec->codec_id );

            if( codec->codec_type == CODEC_TYPE_VIDEO )
            {
#if 0
                codec->get_buffer = obe_get_buffer;
                codec->release_buffer = obe_release_buffer;
                codec->reget_buffer = obe_reget_buffer;
#endif
                codec->flags |= CODEC_FLAG_EMU_EDGE;
            }

            if( avcodec_open( codec, dec ) < 0 )
                return NULL; // TODO cleanup

            /* Wait for encoder to be ready */
            encoder = get_encoder( h, input->output_streams[i].stream_id );
            pthread_mutex_lock( &encoder->encoder_mutex );
            if( !encoder->is_ready )
                pthread_cond_wait( &encoder->encoder_cv, &encoder->encoder_mutex );
            pthread_mutex_unlock( &encoder->encoder_mutex );
        }
    }

    av_init_packet( &pkt );

    while( 1 )
    {
        int finished = 0;
        int ret = 0;

        ret = av_read_frame( lavf, &pkt );
        if( ret < 0 )
            break;
        codec = lavf->streams[pkt.stream_index]->codec;
        out_lut = find_stream_id( stream_lut, pkt.stream_index );

        /* TODO: handle wraparound */

        if( out_lut->stream_id != -1 )
        {
            cur_stream = get_stream( input, out_lut->stream_id );
            if( !cur_stream )
                continue; /* shouldn't happen */
            if( cur_stream->stream_action == STREAM_PASSTHROUGH )
            {
                /* FFmpeg removes the DVB subtitle header */
                if( codec->codec_id == CODEC_ID_DVB_SUBTITLE )
                {
                    coded_frame = new_coded_frame( out_lut->stream_id, pkt.size+3 );
                    coded_frame->data[0] = 0x20;
                    coded_frame->data[1] = 0x00;
                    memcpy( coded_frame->data+2, pkt.data, pkt.size );
                    coded_frame->data[pkt.size+2] = 0xff;
                }
                else
                {
                    coded_frame = new_coded_frame( out_lut->stream_id, pkt.size );
                    if( !coded_frame )
                    {
                        syslog( LOG_ERR, "Malloc failed\n" );
                        obe_free_packet( &pkt );
                        break;
                    }
                    memcpy( coded_frame->data, pkt.data, pkt.size );
                }
                coded_frame->stream_id = out_lut->stream_id;
                coded_frame->pts = pkt.pts;
                add_to_mux_queue( h, coded_frame );
            }
            else
            {
                if( codec->codec_type == CODEC_TYPE_VIDEO )
                {
                    avcodec_get_frame_defaults( &frame );
                    codec->reordered_opaque = pkt.pts;

                    ret = avcodec_decode_video2( codec, &frame, &finished, &pkt );
                    if( finished )
                    {
                        raw_frame = new_raw_frame();
                        if( !raw_frame )
                        {
                            syslog( LOG_ERR, "Malloc failed\n" );
                            obe_free_packet( &pkt );
                            break;
                        }
                        raw_frame->stream_id = out_lut->stream_id;
                        raw_frame->img.csp = codec->pix_fmt;

                        /* full_range_flag is almost always wrong so ignore it */
                        if( raw_frame->img.csp == PIX_FMT_YUVJ420P )
                            raw_frame->img.csp = PIX_FMT_YUV420P;

                        raw_frame->img.width = width = codec->width;
                        raw_frame->img.height = height = codec->height;

                        /* FIXME: get rid of this ugly memcpy */
                        avcodec_align_dimensions2( codec, &width, &height, stride );
                        if( av_image_alloc( raw_frame->img.plane, raw_frame->img.stride, width, height, codec->pix_fmt, 16 ) < 0 )
                        {
                            syslog( LOG_ERR, "Malloc failed\n" );
                            obe_free_packet( &pkt );
                            break;
                        }
                        av_image_copy( raw_frame->img.plane, raw_frame->img.stride, (const uint8_t**)&frame.data,
                                       frame.linesize, codec->pix_fmt, width, height );

                        raw_frame->release_data = release_video_data;
                        raw_frame->release_frame = release_frame;

                        raw_frame->pts = 0;
                        if( codec->has_b_frames && frame.reordered_opaque != AV_NOPTS_VALUE )
                            raw_frame->pts = frame.reordered_opaque;
                        else if( pkt.dts != AV_NOPTS_VALUE )
                            raw_frame->pts = pkt.dts;
 
                        add_to_encode_queue( h, raw_frame );

                        /* TODO: ancillary data */
                        /* TODO: SAR changes */
                    }
                }
#if 0
                else if( codec->codec_type == CODEC_TYPE_AUDIO )
                {

                }
#endif
                obe_free_packet( &pkt );
            }
        }

        obe_free_packet( &pkt );
    }

    for( int i = 0; i < input->num_output_streams; i++ )
    {
        out_lut = find_lavf_stream_idx( stream_lut, input->output_streams[i].stream_id );
        codec = lavf->streams[out_lut->lavf_stream_idx]->codec;

        if( codec )
            avcodec_close( codec );
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_input_func_t lavf_input = { probe_stream, open_input };
