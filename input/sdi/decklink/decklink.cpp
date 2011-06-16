/*****************************************************************************
 * decklink.cpp: BlackMagic DeckLink SDI input module
 *****************************************************************************
 * Copyright (C) 2010 Steinar H. Gunderson
 *
 * Authors: Steinar H. Gunderson <steinar+vlc@gunderson.no>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#define __STDC_CONSTANT_MACROS 1

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
}

#include "include/DeckLinkAPI.h"
#include "include/DeckLinkAPIDispatch.cpp"

#define DECKLINK_VANC_LINES 75

/* FIXME: is the first active line consistent among cards? */
const static obe_line_number_t decklink_sd_first_active_line[] =
{
    { INPUT_VIDEO_FORMAT_PAL,   23 },
    { INPUT_VIDEO_FORMAT_NTSC, 283 },
    { -1, -1 },
};

struct obe_to_decklink
{
    int obe_name;
    uint32_t bmd_name;
};

struct obe_to_decklink_video
{
    int obe_name;
    uint32_t bmd_name;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_decklink video_conn_tab[] =
{
    { INPUT_VIDEO_CONNECTION_SDI,         bmdVideoConnectionSDI },
    { INPUT_VIDEO_CONNECTION_HDMI,        bmdVideoConnectionHDMI },
    { INPUT_VIDEO_CONNECTION_OPTICAL_SDI, bmdVideoConnectionOpticalSDI },
    { INPUT_VIDEO_CONNECTION_COMPONENT,   bmdVideoConnectionComponent },
    { INPUT_VIDEO_CONNECTION_COMPOSITE,   bmdVideoConnectionComposite },
    { INPUT_VIDEO_CONNECTION_S_VIDEO,     bmdVideoConnectionSVideo },
    { -1, -1 },
};

const static struct obe_to_decklink audio_conn_tab[] =
{
    { INPUT_AUDIO_EMBEDDED,               bmdAudioConnectionEmbedded },
    { INPUT_AUDIO_AES_EBU,                bmdAudioConnectionAESEBU },
    { INPUT_AUDIO_ANALOGUE,               bmdAudioConnectionAnalog },
    { -1, -1 },
};

const static struct obe_to_decklink_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_PAL,             bmdModePAL,           1,    25 },
    { INPUT_VIDEO_FORMAT_NTSC,            bmdModeNTSC,          1001, 30000 },
    { INPUT_VIDEO_FORMAT_720P_50,         bmdModeHD720p50,      1,    50 },
    { INPUT_VIDEO_FORMAT_720P_5994,       bmdModeHD720p5994,    1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,         bmdModeHD720p60,      1,    60 },
    { INPUT_VIDEO_FORMAT_1080I_50,        bmdModeHD1080i50,     1,    25 },
    { INPUT_VIDEO_FORMAT_1080I_5994,      bmdModeHD1080i5994,   1001, 30000 },
    { INPUT_VIDEO_FORMAT_1080I_60,        bmdModeHD1080i6000,   1,    60 },
    { INPUT_VIDEO_FORMAT_1080P_2398,      bmdModeHD1080p2398,   1001, 24000 },
    { INPUT_VIDEO_FORMAT_1080P_24,        bmdModeHD1080p24,     1,    24 },
    { INPUT_VIDEO_FORMAT_1080P_25,        bmdModeHD1080p25,     1,    25 },
    { INPUT_VIDEO_FORMAT_1080P_2997,      bmdModeHD1080p2997,   1001, 30000 },
    { INPUT_VIDEO_FORMAT_1080P_30,        bmdModeHD1080p30,     1,    30 },
    { INPUT_VIDEO_FORMAT_1080P_50,        bmdModeHD1080p50,     1,    50 },
    { INPUT_VIDEO_FORMAT_1080P_5994,      bmdModeHD1080p5994,   1001, 60000 },
    { INPUT_VIDEO_FORMAT_1080P_60,        bmdModeHD1080p6000,   1,    60 },
    { -1, -1, -1, -1 },
};

class DeckLinkCaptureDelegate;

typedef struct
{
    IDeckLink *p_card;
    IDeckLinkInput *p_input;
    DeckLinkCaptureDelegate *p_delegate;

    /* we need to hold onto the IDeckLinkConfiguration object, or our settings will not apply.
       see section 2.4.15 of the blackmagic decklink sdk documentation. */
    IDeckLinkConfiguration *p_config;

    int      probe_buf_len;
    int32_t  *audio_probe_buf;

    AVCodec         *dec;
    AVCodecContext  *codec;

    /* Ancillary */
    void (*unpack_line) ( uint32_t *src, uint16_t *dst, int width );
    void (*downscale_line) ( uint16_t *src, uint8_t *dst, int lines );
    obe_sdi_non_display_data_t non_display_parser;

    obe_t *h;
} decklink_ctx_t;

typedef struct
{
    decklink_ctx_t decklink_ctx;

    /* Input */
    int card_idx;
    int video_conn;
    int audio_conn;

    int video_format;
    int num_channels;
    int probe;

    /* Output */
    int width;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} decklink_opts_t;

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback
{
public:
    DeckLinkCaptureDelegate( decklink_opts_t *decklink_opts ) : decklink_opts_(decklink_opts)
    {
        pthread_mutex_init( &ref_mutex_, NULL );
        pthread_mutex_lock( &ref_mutex_ );
        ref_ = 1;
        pthread_mutex_unlock( &ref_mutex_ );
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }

    virtual ULONG STDMETHODCALLTYPE AddRef(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = ++ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        return new_ref;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = --ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        if ( new_ref == 0 )
            delete this;
        return new_ref;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags)
    {
        syslog( LOG_WARNING, "Video input format changed" );
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

private:
    pthread_mutex_t ref_mutex_;
    uintptr_t ref_;
    decklink_opts_t *decklink_opts_;
};

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived( IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audioframe )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
    obe_raw_frame_t *raw_frame = NULL;
    AVPacket pkt;
    AVFrame frame;
    void *frame_bytes, *anc_line;
    int finished = 0, ret, bytes, field_one = 0, field_two = 0, num_anc_lines = 0, anc_line_stride,
    lines_read = 0, first_line = 0, last_line = 0;
    uint32_t line, *frame_ptr;
    uint16_t *anc_buf, *anc_buf_pos;
    int anc_lines[DECKLINK_VANC_LINES];
    IDeckLinkVideoFrameAncillary *ancillary;

    if( decklink_ctx->non_display_parser.has_probed )
        return S_OK;

    av_init_packet( &pkt );

    if( videoframe )
    {
        if( videoframe->GetFlags() & bmdFrameHasNoInputSource )
        {
            syslog( LOG_ERR, "No input signal detected" );
            return S_OK;
        }

        const int width = videoframe->GetWidth();
        const int height = videoframe->GetHeight();
        const int stride = videoframe->GetRowBytes();

        videoframe->GetBytes( &frame_bytes );

        /* TODO: support format switching (rare in SDI) */
        int j, k;
        for( j = 0; first_active_line[j].format != -1; j++ )
        {
            if( decklink_opts_->video_format == first_active_line[j].format )
                break;
        }

        if( IS_INTERLACED( decklink_opts_->video_format ) )
        {
            for( k = 0; field_start_lines[k].format != -1; k++ )
            {
                if( decklink_opts_->video_format == field_start_lines[k].format )
                    break;
            }

            field_one = field_start_lines[k].line;
            field_two = field_start_lines[k].field_two;
        }

        videoframe->GetAncillaryData( &ancillary );

        /* NTSC starts on line 4 */
        line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? 4 : 1;

        anc_line_stride = FFALIGN( (width * 2 * sizeof(uint16_t)), 16 );

        /* Overallocate slightly for VANC buffer
         * Some VBI services stray into the active picture so allocate some extra space */
        if( IS_SD( decklink_opts_->video_format ) )
            anc_buf = (uint16_t*)av_malloc( (DECKLINK_VANC_LINES + NUM_ACTIVE_VBI_LINES) * anc_line_stride );
        else
            anc_buf = (uint16_t*)av_malloc( DECKLINK_VANC_LINES * anc_line_stride );

        anc_buf_pos = anc_buf;
        /* FIXME: will the card ever skip lines? */
        while( 1 )
        {
            if( IS_INTERLACED( decklink_opts_->video_format ) )
            {
                if( lines_read & 1 ) /* even line (line numbers start from 1) */
                    line = field_two++;
                else
                    line = field_one++;
            }
            else
                 line++;

            if( line == first_active_line[j].line )
                break;

            /* Some cards have restrictions on what lines can be accessed so try them all */
            if( ancillary->GetBufferForVerticalBlankingLine( line, &anc_line ) == S_OK )
            {
                decklink_ctx->unpack_line( (uint32_t*)anc_line, anc_buf_pos, width );
                anc_buf_pos += anc_line_stride / 2;
                anc_lines[num_anc_lines++] = line;

                if( !first_line )
                    first_line = line;
                last_line = line;
            }

            lines_read++;
        }

        ancillary->Release();

        if( !decklink_opts_->probe )
        {
            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
        }

        anc_buf_pos = anc_buf;
        for( int i = 0; i < num_anc_lines; i++ )
        {
            parse_vanc_line( &decklink_ctx->non_display_parser, raw_frame, anc_buf_pos, width, anc_lines[i] );
            anc_buf_pos += anc_line_stride / 2;
        }

#if 0
        if( IS_SD( decklink_opts_->video_format ) && first_line != last_line )
        {
            // TODO line 20 is missing from NTSC

            /* Add a some VBI lines to the ancillary buffer */
            frame_ptr = (uint32_t*)frame_bytes;
            for( int i = 0; i < NUM_ACTIVE_VBI_LINES; i++ )
            {
                decklink_ctx->unpack_line( frame_ptr, anc_buf_pos, width );
                anc_buf_pos += anc_line_stride / 2;
                frame_ptr += stride / 4;
            }

            // downscale lines and make sure width=stride

            if( decode_vbi( &decklink_ctx->non_display_parser, vbi_buf, raw_frame ) < 0 )
                goto fail;
            av_free( vbi_buf );
        }
#endif

        av_free( anc_buf );

        if( !decklink_opts_->probe )
        {
            decklink_ctx->codec->width = width;
            decklink_ctx->codec->height = height;
            decklink_ctx->codec->custom_stride = stride;

            pkt.data = (uint8_t*)frame_bytes;
            pkt.size = stride * height;

            avcodec_get_frame_defaults( &frame );
            ret = avcodec_decode_video2( decklink_ctx->codec, &frame, &finished, &pkt );
            if( ret < 0 || !finished )
            {
                syslog( LOG_ERR, "[decklink]: Could not decode video frame\n" );
                goto end;
            }

            raw_frame->release_data = obe_release_video_data;
            raw_frame->release_frame = obe_release_frame;

            memcpy( raw_frame->img.stride, frame.linesize, sizeof(raw_frame->img.stride) );
            memcpy( raw_frame->img.plane, frame.data, sizeof(raw_frame->img.plane) );
            raw_frame->img.csp = (int)decklink_ctx->codec->pix_fmt;
            raw_frame->img.planes = av_pix_fmt_descriptors[raw_frame->img.csp].nb_components;
            raw_frame->img.width = width;
            raw_frame->img.height = height;
            if( IS_SD( decklink_opts_->video_format ) )
            {
                raw_frame->img.format     = decklink_opts_->video_format;
                raw_frame->img.first_line = decklink_sd_first_active_line[decklink_opts_->video_format].line;
            }

            BMDTimeValue stream_time, frame_duration;
            videoframe->GetStreamTime( &stream_time, &frame_duration, 90000 );

            raw_frame->pts = stream_time;
            add_to_filter_queue( decklink_ctx->h, raw_frame );

            /* Send any DVB-VBI frames */
            if( decklink_ctx->non_display_parser.dvb_frame )
            {
                decklink_ctx->non_display_parser.dvb_frame->stream_id = 2; // FIXME
                decklink_ctx->non_display_parser.dvb_frame->pts = stream_time;

                add_to_mux_queue( decklink_ctx->h, decklink_ctx->non_display_parser.dvb_frame );
                decklink_ctx->non_display_parser.dvb_frame = NULL;
            }
        }
    }

    /* TODO: probe SMPTE 337M audio */

    if( audioframe && !decklink_opts_->probe )
    {
        raw_frame = new_raw_frame();
        if( !raw_frame )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        raw_frame->num_samples = audioframe->GetSampleFrameCount();
        raw_frame->channel_layout = AV_CH_LAYOUT_STEREO;
        raw_frame->sample_fmt = AV_SAMPLE_FMT_S32;

        bytes = raw_frame->num_samples * decklink_opts_->num_channels * sizeof(int32_t);

        audioframe->GetBytes( &frame_bytes );
        raw_frame->len = raw_frame->bytes_left = bytes;
        raw_frame->data = (uint8_t*)av_malloc( raw_frame->len );
        if( !raw_frame->data )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto fail;
        }

        raw_frame->cur_pos = raw_frame->data;
        memcpy( raw_frame->data, frame_bytes, bytes );

        BMDTimeValue packet_time;
        audioframe->GetPacketTime( &packet_time, 90000 );
        raw_frame->pts = packet_time;
        raw_frame->release_data = obe_release_other_data;
        raw_frame->release_frame = obe_release_frame;
        raw_frame->stream_id = 1; // FIXME

        add_to_encode_queue( decklink_ctx->h, raw_frame );
    }

end:
    av_free_packet( &pkt );

    return S_OK;

fail:

    if( raw_frame )
    {
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    return S_OK;
}

static void close_card( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

    if( decklink_ctx->p_config )
        decklink_ctx->p_config->Release();

    if( decklink_ctx->p_input )
    {
        decklink_ctx->p_input->StopStreams();
        decklink_ctx->p_input->Release();
    }

    if( decklink_ctx->p_card )
        decklink_ctx->p_card->Release();

    if( decklink_ctx->p_delegate )
        decklink_ctx->p_delegate->Release();

    if( decklink_ctx->codec )
    {
        avcodec_close( decklink_ctx->codec );
        av_free( decklink_ctx->codec );
    }

    if( IS_SD( decklink_opts->video_format ) )
        vbi_raw_decoder_destroy( &decklink_ctx->non_display_parser.vbi_decoder );
}

static int open_card( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;
    int         found_mode;
    int         ret = 0;
    int         i;
    const int   sample_rate = 48000;
    const char *model_name;
    BMDDisplayMode wanted_mode_id;

    IDeckLinkDisplayModeIterator *p_display_iterator = NULL;
    IDeckLinkIterator *decklink_iterator = NULL;
    HRESULT result;

    avcodec_init();
    avcodec_register_all();
    decklink_ctx->dec = avcodec_find_decoder( CODEC_ID_V210 );
    if( !decklink_ctx->dec )
    {
        fprintf( stderr, "[decklink] Could not find v210 decoder\n" );
        goto finish;
    }

    decklink_ctx->codec = avcodec_alloc_context();
    if( !decklink_ctx->codec )
    {
        fprintf( stderr, "[decklink] Could not allocate AVCodecContext\n" );
        goto finish;
    }

    decklink_ctx->codec->get_buffer = obe_get_buffer;
    decklink_ctx->codec->release_buffer = obe_release_buffer;
    decklink_ctx->codec->reget_buffer = obe_reget_buffer;
    decklink_ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;

    if( avcodec_open( decklink_ctx->codec, decklink_ctx->dec ) < 0 )
    {
        fprintf( stderr, "[decklink] Could not open libavcodec\n" );
        goto finish;
    }

    decklink_iterator = CreateDeckLinkIteratorInstance();
    if( !decklink_iterator )
    {
        fprintf( stderr, "[decklink] DeckLink drivers not found\n" );
        ret = -1;
        goto finish;
    }

    if( decklink_opts->card_idx < 0 )
    {
        fprintf( stderr, "[decklink] Invalid card index %d \n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

    for( i = 0; i <= decklink_opts->card_idx; ++i )
    {
        if( decklink_ctx->p_card )
            decklink_ctx->p_card->Release();
        result = decklink_iterator->Next( &decklink_ctx->p_card );
        if( result != S_OK )
            break;
    }

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] DeckLink PCI card %d not found\n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_card->GetModelName( &model_name );

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Could not get model name\n" );
        ret = -1;
        goto finish;
    }

    syslog( LOG_INFO, "Opened DeckLink PCI card %d (%s)", decklink_opts->card_idx, model_name );

    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkInput, (void**)&decklink_ctx->p_input) != S_OK )
    {
        fprintf( stderr, "[decklink] Card has no inputs\n" );
        ret = -1;
        goto finish;
    }

    /* Set up the video and audio sources. */
    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkConfiguration, (void**)&decklink_ctx->p_config) != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to get configuration interface\n" );
        ret = -1;
        goto finish;
    }

    /* Setup video connection */
    for( i = 0; video_conn_tab[i].obe_name != -1; i++ )
    {
        if( video_conn_tab[i].obe_name == decklink_opts->video_conn )
            break;
    }

    if( video_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigVideoInputConnection, video_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set video input connection\n" );
        ret = -1;
        goto finish;
    }

    /* Setup audio connection */
    for( i = 0; audio_conn_tab[i].obe_name != -1; i++ )
    {
        if( audio_conn_tab[i].obe_name == decklink_opts->audio_conn )
            break;
    }

    if( audio_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported audio input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigAudioInputConnection, audio_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set audio input connection\n" );
        ret = -1;
        goto finish;
    }

    /* Get the list of display modes. */
    result = decklink_ctx->p_input->GetDisplayModeIterator( &p_display_iterator );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enumerate display modes\n" );
        ret = -1;
        goto finish;
    }

    for( i = 0; video_format_tab[i].obe_name != -1; i++ )
    {
        if( video_format_tab[i].obe_name == decklink_opts->video_format )
            break;
    }

    if( video_format_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video format\n" );
        ret = -1;
        goto finish;
    }

    wanted_mode_id = video_format_tab[i].bmd_name;
    found_mode = false;
    decklink_opts->timebase_num = video_format_tab[i].timebase_num;
    decklink_opts->timebase_den = video_format_tab[i].timebase_den;

    for (;;)
    {
        IDeckLinkDisplayMode *p_display_mode;
        result = p_display_iterator->Next( &p_display_mode );
        if( result != S_OK || !p_display_mode )
            break;

        BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();

        BMDTimeValue frame_duration, time_scale;
        result = p_display_mode->GetFrameRate( &frame_duration, &time_scale );
        if( result != S_OK )
        {
            fprintf( stderr, "[decklink] Failed to get frame rate\n" );
            ret = -1;
            p_display_mode->Release();
            goto finish;
        }

        if( wanted_mode_id == mode_id )
        {
            found_mode = true;
            decklink_opts->width = p_display_mode->GetWidth();
            decklink_opts->height = p_display_mode->GetHeight();

            switch( p_display_mode->GetFieldDominance() )
            {
                case bmdProgressiveFrame:
                    decklink_opts->interlaced = 0;
                    decklink_opts->tff        = 0;
                    break;
                case bmdProgressiveSegmentedFrame:
                    /* Assume tff interlaced - this mode should not be used in broadcast */
                    decklink_opts->interlaced = 1;
                    decklink_opts->tff        = 1;
                    break;
                case bmdUpperFieldFirst:
                    decklink_opts->interlaced = 1;
                    decklink_opts->tff        = 1;
                    break;
                case bmdLowerFieldFirst:
                    decklink_opts->interlaced = 1;
                    decklink_opts->tff        = 0;
                    break;
                case bmdUnknownFieldDominance:
                default:
                    /* Assume progressive */
                    decklink_opts->interlaced = 0;
                    decklink_opts->tff        = 0;
                    break;
            }
        }

        p_display_mode->Release();
    }

    if( !found_mode )
    {
        fprintf( stderr, "[decklink] Unsupported video mode\n" );
        ret = -1;
        goto finish;
    }

    /* Setup VBI and VANC unpack functions */
    if( IS_SD( decklink_opts->video_format ) )
    {
        ret = setup_vbi_parser( &decklink_ctx->non_display_parser, decklink_opts->video_format == INPUT_VIDEO_FORMAT_NTSC );
        decklink_ctx->unpack_line = obe_v210_line_to_uyvy_c;
        decklink_ctx->downscale_line = obe_downscale_line_c;
    }
    else
        decklink_ctx->unpack_line = obe_v210_line_to_nv20_c;

    if( ret < 0 )
    {
        fprintf( stderr, "[decklink] Failed to enable VBI parsing\n" );
        goto finish;
    }

    result = decklink_ctx->p_input->EnableVideoInput( wanted_mode_id, bmdFormat10BitYUV, 0 );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable video input\n" );
        ret = -1;
        goto finish;
    }

    /* Set up audio. */
    result = decklink_ctx->p_input->EnableAudioInput( sample_rate, bmdAudioSampleType32bitInteger, decklink_opts->num_channels );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable audio input\n" );
        ret = -1;
        goto finish;
    }

    decklink_ctx->p_delegate = new DeckLinkCaptureDelegate( decklink_opts );
    decklink_ctx->p_input->SetCallback( decklink_ctx->p_delegate );

    result = decklink_ctx->p_input->StartStreams();
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Could not start streaming from card\n" );
        ret = -1;
        goto finish;
    }

    ret = 0;

finish:
    if( decklink_iterator )
        decklink_iterator->Release();

    if( p_display_iterator )
        p_display_iterator->Release();

    if( ret )
        close_card( decklink_opts );

    return ret;
}

static void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int num_streams = 0, vbi_stream_services = 0;
    obe_sdi_non_display_data_t *non_display_parser;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    non_display_parser = &decklink_opts->decklink_ctx.non_display_parser;

    /* TODO: support multi-channel */
    decklink_opts->num_channels = 2;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;

    decklink_opts->probe = non_display_parser->probe = 1;

    open_card( decklink_opts );

    sleep( 1 );

    close_card( decklink_opts );

    /* TODO: probe for SMPTE 337M */
    /* TODO: add other streams */

    for( int i = 0; i < non_display_parser->num_frame_data; i++ )
    {
        if( non_display_parser->frame_data[i].location == USER_DATA_LOCATION_DVB_STREAM )
            vbi_stream_services++;
    }

    /* TODO: teletext */

    num_streams = 2+!!vbi_stream_services;
    for( int i = 0; i < num_streams; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            goto finish;

        /* TODO: make it take a continuous set of stream-ids */
        pthread_mutex_lock( &h->device_list_mutex );
        streams[i]->stream_id = h->cur_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        if( i == 0 )
        {
            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->width  = decklink_opts->width;
            streams[i]->height = decklink_opts->height;
            streams[i]->timebase_num = decklink_opts->timebase_num;
            streams[i]->timebase_den = decklink_opts->timebase_den;
            streams[i]->csp    = PIX_FMT_YUV422P10;
            streams[i]->interlaced = decklink_opts->interlaced;
            streams[i]->tff = decklink_opts->tff;
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */

            if( add_non_display_services( non_display_parser, streams[i], USER_DATA_LOCATION_FRAME ) < 0 )
                goto finish;
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->channel_layout = AV_CH_LAYOUT_STEREO;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32;
            streams[i]->sample_rate = 48000;
        }
        else /* VBI stream */
        {
            streams[i]->stream_type = STREAM_TYPE_MISC;
            streams[i]->stream_format = VBI_RAW;
            if( add_non_display_services( non_display_parser, streams[i], USER_DATA_LOCATION_DVB_STREAM ) < 0 )
                goto finish;
        }
    }

    if( non_display_parser->num_frame_data )
        free( non_display_parser->frame_data );

    device = new_device();

    if( !device )
        goto finish;

    device->num_input_streams = num_streams;
    memcpy( device->streams, streams, num_streams * sizeof(obe_int_input_stream_t**) );
    device->device_type = INPUT_DEVICE_DECKLINK;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );

    /* add device */
    add_device( h, device );

finish:
    if( decklink_opts )
        free( decklink_opts );

    free( probe_ctx );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    obe_input_t *user_opts = &device->user_opts;
    decklink_ctx_t *decklink_ctx;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    decklink_opts->num_channels = 2;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;

    decklink_ctx = &decklink_opts->decklink_ctx;

    decklink_ctx->h = h;

    /* TODO: wait for encoder */

    open_card( decklink_opts );

    sleep( INT_MAX );

    close_card( decklink_opts );

finish:
    if( decklink_opts )
        free( decklink_opts );
    return NULL;
}

const obe_input_func_t decklink_input = { probe_stream, open_input };
