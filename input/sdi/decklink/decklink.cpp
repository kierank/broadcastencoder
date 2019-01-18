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

#define __STDC_FORMAT_MACROS   1
#define __STDC_CONSTANT_MACROS 1

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include "input/bars/bars_common.h"
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
}

#include "include/DeckLinkAPI.h"
#include "include/DeckLinkAPIDispatch.cpp"

#define DECKLINK_VANC_LINES 100
#define DECKLINK_SAMPLE_RATE 48000

#define DROP_MIN 50

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
    int width;
    int height;
    int interlaced;
};

const static struct obe_to_decklink video_conn_tab[] =
{
    { INPUT_VIDEO_CONNECTION_SDI,         bmdVideoConnectionSDI },
    { INPUT_VIDEO_CONNECTION_HDMI,        bmdVideoConnectionHDMI },
    { INPUT_VIDEO_CONNECTION_OPTICAL_SDI, bmdVideoConnectionOpticalSDI },
    { INPUT_VIDEO_CONNECTION_COMPONENT,   bmdVideoConnectionComponent },
    { INPUT_VIDEO_CONNECTION_COMPOSITE,   bmdVideoConnectionComposite },
    { INPUT_VIDEO_CONNECTION_S_VIDEO,     bmdVideoConnectionSVideo },
    { -1, 0 },
};

const static struct obe_to_decklink audio_conn_tab[] =
{
    { INPUT_AUDIO_EMBEDDED,               bmdAudioConnectionEmbedded },
    { INPUT_AUDIO_AES_EBU,                bmdAudioConnectionAESEBU },
    { INPUT_AUDIO_ANALOGUE,               bmdAudioConnectionAnalog },
    { -1, 0 },
};

const static struct obe_to_decklink_video decklink_video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_AUTODETECT,      bmdModePAL,           1,    25,    720, 576,   1 }, /* Set it up as PAL arbitrarily */
    { INPUT_VIDEO_FORMAT_PAL,             bmdModePAL,           1,    25,    720, 576,   1 },
    { INPUT_VIDEO_FORMAT_NTSC,            bmdModeNTSC,          1001, 30000, 720, 480,   1 },
    { INPUT_VIDEO_FORMAT_720P_50,         bmdModeHD720p50,      1,    50,    1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_720P_5994,       bmdModeHD720p5994,    1001, 60000, 1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_720P_60,         bmdModeHD720p60,      1,    60,    1280, 720,  0 },
    { INPUT_VIDEO_FORMAT_1080I_50,        bmdModeHD1080i50,     1,    25,    1920, 1080, 1 },
    { INPUT_VIDEO_FORMAT_1080I_5994,      bmdModeHD1080i5994,   1001, 30000, 1920, 1080, 1 },
    { INPUT_VIDEO_FORMAT_1080P_2398,      bmdModeHD1080p2398,   1001, 24000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_24,        bmdModeHD1080p24,     1,    24,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_25,        bmdModeHD1080p25,     1,    25,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_2997,      bmdModeHD1080p2997,   1001, 30000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_30,        bmdModeHD1080p30,     1,    30,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_50,        bmdModeHD1080p50,     1,    50,    1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_5994,      bmdModeHD1080p5994,   1001, 60000, 1920, 1080, 0 },
    { INPUT_VIDEO_FORMAT_1080P_60,        bmdModeHD1080p6000,   1,    60,    1920, 1080, 0 },
    { -1, 0, -1, -1, -1, -1, -1 },
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

#if 0
    int      probe_buf_len;
    int32_t  *audio_probe_buf;
#endif

    /* Video */
    AVFrame         *frame;
    AVCodec         *dec;
    AVCodecContext  *codec;
    int64_t         v_counter;
    AVRational      v_timebase;
    hnd_t           bars_hnd;
    int64_t         drop_count;

    /* frame data for black or last-frame */
    obe_raw_frame_t stored_video_frame;
    obe_raw_frame_t stored_audio_frame;

    /* output frame pointers for bars and tone */
    obe_raw_frame_t **raw_frames;

    /* Audio */
    int64_t         a_counter;
    AVRational      a_timebase;
    AVAudioResampleContext *avr;
    const obe_audio_sample_pattern_t *sample_pattern;
    int64_t         a_errors;


    int64_t last_frame_time;

    /* VBI */
    int has_setup_vbi;

    /* Ancillary */
    void (*unpack_line) ( uint16_t *dsty, intptr_t i_dsty, uint16_t *dstc, intptr_t i_dstc, uint32_t *src, intptr_t i_src, intptr_t w, intptr_t h );
    void (*downscale_line) ( uint16_t *src, uint8_t *dst, int lines );
    void (*blank_line) ( uint16_t *dst, int width );
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
    int picture_on_loss;
    int downscale;
    obe_bars_opts_t obe_bars_opts;

    /* Output */
    int probe_success;

    int width;
    int coded_height;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} decklink_opts_t;

struct decklink_status
{
    obe_input_params_t *input;
    decklink_opts_t *decklink_opts;
};

static void setup_pixel_funcs( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

    int cpu_flags = av_get_cpu_flags();

    /* Setup VBI and VANC unpack functions */
    if( IS_SD( decklink_opts->video_format ) )
    {
        decklink_ctx->unpack_line = obe_v210_line_to_uyvy_c;
        decklink_ctx->downscale_line = obe_downscale_line_c;
        decklink_ctx->blank_line = obe_blank_line_uyvy_c;

        if( cpu_flags & AV_CPU_FLAG_MMX )
            decklink_ctx->downscale_line = obe_downscale_line_mmx;

        if( cpu_flags & AV_CPU_FLAG_SSE2 )
            decklink_ctx->downscale_line = obe_downscale_line_sse2;
    }
    else
    {
        decklink_ctx->unpack_line = obe_v210_line_to_nv20_c;
        decklink_ctx->blank_line = obe_blank_line_nv20_c;

        if( cpu_flags & AV_CPU_FLAG_SSSE3 )
            decklink_ctx->unpack_line = obe_v210_line_to_nv20_ssse3;

        if( cpu_flags & AV_CPU_FLAG_AVX )
            decklink_ctx->unpack_line = obe_v210_line_to_nv20_avx;
    }
}

static void get_format_opts( decklink_opts_t *decklink_opts, IDeckLinkDisplayMode *p_display_mode )
{
    decklink_opts->width = p_display_mode->GetWidth();
    decklink_opts->coded_height = p_display_mode->GetHeight();

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

    decklink_opts->height = decklink_opts->coded_height;
    if( decklink_opts->coded_height == 486 )
        decklink_opts->height = 480;
}

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

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *p_display_mode, BMDDetectedVideoInputFormatFlags)
    {
        decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
        obe_t *h = decklink_ctx->h;
        int i = 0;
        BMDPixelFormat pix_fmt;
        if( events & bmdVideoInputDisplayModeChanged )
        {
            BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();
            syslog( LOG_WARNING, "Video input format changed" );

            for( i = 0; decklink_video_format_tab[i].obe_name != -1; i++ )
            {
                if( decklink_video_format_tab[i].obe_name != INPUT_VIDEO_FORMAT_AUTODETECT &&
                    decklink_video_format_tab[i].bmd_name == mode_id )
                    break;
            }

            if( decklink_video_format_tab[i].obe_name == -1 )
            {
                syslog( LOG_WARNING, "Unsupported video format" );
                return S_OK;
            }

            int pal = IS_PAL( decklink_video_format_tab[i].obe_name );
            if( decklink_ctx->last_frame_time == -1 || (decklink_opts_->downscale && pal) )
            {
                decklink_opts_->video_format = decklink_video_format_tab[i].obe_name;
                decklink_opts_->timebase_num = decklink_video_format_tab[i].timebase_num;
                decklink_opts_->timebase_den = decklink_video_format_tab[i].timebase_den;

                get_format_opts( decklink_opts_, p_display_mode );
                setup_pixel_funcs( decklink_opts_ );

                /* Need to change resolution if it's not the one that was setup originally */
                if( decklink_opts_->picture_on_loss )
                {
                    /* Clear old setup video and audio frames */
                    if( decklink_ctx->stored_video_frame.release_data )
                        decklink_ctx->stored_video_frame.release_data( &decklink_ctx->stored_video_frame );
                    if( decklink_ctx->stored_audio_frame.release_data )
                        decklink_ctx->stored_audio_frame.release_data( &decklink_ctx->stored_audio_frame );

                    decklink_ctx->sample_pattern = get_sample_pattern( decklink_opts_->video_format );
                    if( !decklink_ctx->sample_pattern )
                    {
                        syslog( LOG_WARNING, "Invalid sample pattern" );
                        return S_OK;
                    }

                    if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_BARS )
                    {
                        decklink_opts_->obe_bars_opts.video_format = decklink_opts_->video_format;

                        if( decklink_ctx->bars_hnd )
                            close_bars( decklink_ctx->bars_hnd );

                        if( open_bars( &decklink_ctx->bars_hnd, &decklink_opts_->obe_bars_opts ) < 0 )
                        {
                            fprintf( stderr, "[decklink] Could not open bars\n" );
                            return S_OK;
                        }
                    }
                    else
                    {
                        /* Setup new video and audio frames */
                        if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
                            decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
                        {
                            setup_stored_video_frame( &decklink_ctx->stored_video_frame, decklink_video_format_tab[i].width,
                                                      decklink_video_format_tab[i].height );
                            blank_yuv422p_frame( &decklink_ctx->stored_video_frame );
                        }

                        setup_stored_audio_frame( &decklink_ctx->stored_audio_frame, decklink_ctx->sample_pattern->max );
                    }
                }

                decklink_ctx->p_input->PauseStreams();
                pix_fmt = h->filter_bit_depth == OBE_BIT_DEPTH_10 ? bmdFormat10BitYUV : bmdFormat8BitYUV;
                decklink_ctx->p_input->EnableVideoInput( p_display_mode->GetDisplayMode(), pix_fmt, bmdVideoInputEnableFormatDetection );
                decklink_ctx->p_input->FlushStreams();
                decklink_ctx->p_input->StartStreams();
            }
        }
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
    void *frame_bytes, *anc_line;
    obe_t *h = decklink_ctx->h;
    int finished = 0, ret, num_anc_lines = 0, anc_line_stride,
    lines_read = 0, first_line = 0, last_line = 0, line, num_vbi_lines, vii_line;
    uint32_t *frame_ptr;
    uint16_t *anc_buf, *anc_buf_pos;
    uint8_t *vbi_buf;
    int anc_lines[DECKLINK_VANC_LINES];
    IDeckLinkVideoFrameAncillary *ancillary;
    int64_t pts = -1;

    if( decklink_opts_->probe_success )
        return S_OK;

    av_init_packet( &pkt );

    if( videoframe )
    {
        pts = av_rescale_q( decklink_ctx->v_counter, decklink_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
        if( videoframe->GetFlags() & bmdFrameHasNoInputSource )
        {
            syslog( LOG_ERR, "inputDropped: Decklink card index %i: No input signal detected", decklink_opts_->card_idx );
            if( !decklink_opts_->probe )
            {
                pthread_mutex_lock( &h->device_mutex );
                h->device.input_status.active = 0;
                pthread_mutex_unlock( &h->device_mutex );
            }
            decklink_ctx->drop_count++;

            if( !decklink_opts_->probe && decklink_opts_->picture_on_loss && decklink_ctx->drop_count > DROP_MIN )
            {
                obe_raw_frame_t *video_frame = NULL, *audio_frame = NULL;
                obe_clock_tick( h, pts );

                /* Reset Speedcontrol */
                if( decklink_ctx->drop_count == DROP_MIN+1 )
                {
                    pthread_mutex_lock( &h->drop_mutex );
                    h->encoder_drop = h->mux_drop = 1;
                    pthread_mutex_unlock( &h->drop_mutex );
                }

                if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
                    decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
                {
                    video_frame = new_raw_frame();
                    if( !video_frame )
                    {
                        syslog( LOG_ERR, "Malloc failed\n" );
                        goto end;
                    }
                    memcpy( video_frame, &decklink_ctx->stored_video_frame, sizeof(*video_frame) );
                    int i = 0;
                    while( video_frame->buf_ref[i] != NULL )
                    {
                        video_frame->buf_ref[i] = av_buffer_ref( decklink_ctx->stored_video_frame.buf_ref[i] );
                        i++;
                    }
                    video_frame->buf_ref[i] = NULL;
                }
                else if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_BARS )
                {
                    get_bars( decklink_ctx->bars_hnd, decklink_ctx->raw_frames );

                    video_frame = decklink_ctx->raw_frames[0];
                    audio_frame = decklink_ctx->raw_frames[1];
                }

                video_frame->pts = pts;

                if( add_to_filter_queue( h, video_frame ) < 0 )
                    goto end;

                if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
                    decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
                {
                    audio_frame = new_raw_frame();
                    if( !audio_frame )
                    {
                        syslog( LOG_ERR, "Malloc failed\n" );
                        goto end;
                    }

                    memcpy( audio_frame, &decklink_ctx->stored_audio_frame, sizeof(*audio_frame) );
                    /* Assumes only one buffer reference */
                    audio_frame->buf_ref[0] = av_buffer_ref( decklink_ctx->stored_audio_frame.buf_ref[0] );
                    audio_frame->buf_ref[1] = NULL;

                    audio_frame->audio_frame.num_samples = decklink_ctx->sample_pattern->pattern[decklink_ctx->v_counter % decklink_ctx->sample_pattern->mod];
                }

                /* Write audio frame */
                for( int i = 0; i < h->device.num_input_streams; i++ )
                {
                    if( h->device.streams[i]->stream_format == AUDIO_PCM )
                        audio_frame->input_stream_id = h->device.streams[i]->input_stream_id;
                }

                audio_frame->pts = av_rescale_q( decklink_ctx->a_counter, decklink_ctx->a_timebase,
                                                 (AVRational){1, OBE_CLOCK} );
                decklink_ctx->a_counter += audio_frame->audio_frame.num_samples;

                if( add_to_filter_queue( h, audio_frame ) < 0 )
                    goto end;
                /* Increase video PTS at the end so it can be used in NTSC sample size generation */
                decklink_ctx->v_counter++;
            }
            return S_OK;
        }
        else if( decklink_opts_->probe )
            decklink_opts_->probe_success = 1;

        if( !decklink_opts_->probe )
        {
            pthread_mutex_lock( &h->device_mutex );
            h->device.input_status.active = 1;
            pthread_mutex_unlock( &h->device_mutex );
        }

        /* use SDI ticks as clock source */
        obe_clock_tick( h, pts );

        if( decklink_ctx->last_frame_time == -1 )
        {
            decklink_ctx->last_frame_time = obe_mdate();
            syslog( LOG_INFO, "inputActivate: Decklink input active" );
        }

        if( !decklink_opts_->probe && decklink_ctx->drop_count )
        {
            pthread_mutex_lock( &h->drop_mutex );
            h->encoder_drop = h->mux_drop = 1;
            pthread_mutex_unlock( &h->drop_mutex );
        }

        decklink_ctx->drop_count = 0;

        const int width = videoframe->GetWidth();
        const int height = videoframe->GetHeight();
        const int stride = videoframe->GetRowBytes();

        videoframe->GetBytes( &frame_bytes );

        /* TODO: support format switching (rare in SDI) */
        int j;
        for( j = 0; first_active_line[j].format != -1; j++ )
        {
            if( decklink_opts_->video_format == first_active_line[j].format )
                break;
        }

        if( !decklink_opts_->probe )
        {
            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
        }

        /* TODO: ancillary data in 8-bit mode */
        if( h->filter_bit_depth == OBE_BIT_DEPTH_10 )
        {
            videoframe->GetAncillaryData( &ancillary );

            /* NTSC starts on line 4 */
            line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? 4 : 1;
            anc_line_stride = FFALIGN( (width * 2 * sizeof(uint16_t)), 32 );

            /* Overallocate slightly for VANC buffer
             * Some VBI services stray into the active picture so allocate some extra space */
            anc_buf = anc_buf_pos = (uint16_t*)av_malloc( DECKLINK_VANC_LINES * anc_line_stride );
            if( !anc_buf )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            while( 1 )
            {
                /* Some cards have restrictions on what lines can be accessed so try them all
                 * Some buggy decklink cards will randomly refuse access to a particular line so
                 * work around this issue by blanking the line */
                if( ancillary->GetBufferForVerticalBlankingLine( line, &anc_line ) == S_OK )
                {
                    uint16_t *uv = anc_buf_pos + width;
                    decklink_ctx->unpack_line( anc_buf_pos, anc_line_stride, uv, anc_line_stride, (uint32_t*)anc_line, stride, width, 1 );
                }
                else
                    decklink_ctx->blank_line( anc_buf_pos, width );

                anc_buf_pos += anc_line_stride / 2;
                anc_lines[num_anc_lines++] = line;

                if( !first_line )
                    first_line = line;
                last_line = line;

                lines_read++;
                line = sdi_next_line( decklink_opts_->video_format, line );

                if( line == first_active_line[j].line )
                    break;
            }

            ancillary->Release();

            anc_buf_pos = anc_buf;
            for( int i = 0; i < num_anc_lines; i++ )
            {
                parse_vanc_line( h, &decklink_ctx->non_display_parser, raw_frame, anc_buf_pos, width, anc_lines[i] );
                anc_buf_pos += anc_line_stride / 2;
            }

            if( IS_SD( decklink_opts_->video_format ) && first_line != last_line )
            {
                /* Add a some VBI lines to the ancillary buffer */
                frame_ptr = (uint32_t*)frame_bytes;

                /* NTSC starts from line 283 so add an extra line */
                num_vbi_lines = NUM_ACTIVE_VBI_LINES + ( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC );
                for( int i = 0; i < num_vbi_lines; i++ )
                {
                    /* Second plane is irrelevant for UYVY */
                    decklink_ctx->unpack_line( anc_buf_pos, anc_line_stride, NULL, 0, (uint32_t*)frame_ptr, stride, width, 1 );
                    anc_buf_pos += anc_line_stride / 2;
                    frame_ptr += stride / 4;
                    last_line = sdi_next_line( decklink_opts_->video_format, last_line );
                }
                num_anc_lines += num_vbi_lines;

                vbi_buf = (uint8_t*)av_malloc( width * 2 * num_anc_lines );
                if( !vbi_buf )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    goto end;
                }

                /* Scale the lines from 10-bit to 8-bit */
                decklink_ctx->downscale_line( anc_buf, vbi_buf, num_anc_lines );
                anc_buf_pos = anc_buf;

                /* Setup VBI parser. Also sets up CRCs for Video Index */
                if( !decklink_ctx->has_setup_vbi )
                {
                    vbi_raw_decoder_init( &decklink_ctx->non_display_parser.vbi_decoder );

                    decklink_ctx->non_display_parser.ntsc = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC;
                    decklink_ctx->non_display_parser.vbi_decoder.start[0] = first_line;
                    decklink_ctx->non_display_parser.vbi_decoder.start[1] = sdi_next_line( decklink_opts_->video_format, first_line );
                    decklink_ctx->non_display_parser.vbi_decoder.count[0] = last_line - decklink_ctx->non_display_parser.vbi_decoder.start[1] + 1;
                    decklink_ctx->non_display_parser.vbi_decoder.count[1] = decklink_ctx->non_display_parser.vbi_decoder.count[0];

                    if( setup_vbi_parser( &decklink_ctx->non_display_parser ) < 0 )
                        goto fail;

                    decklink_ctx->has_setup_vbi = 1;
                }

                /* Handle Video Index information */
                int tmp_line = first_line;
                vii_line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? NTSC_VIDEO_INDEX_LINE : PAL_VIDEO_INDEX_LINE;
                while( tmp_line != vii_line )
                {
                    anc_buf_pos += anc_line_stride / 2;
                    tmp_line = sdi_next_line( decklink_opts_->video_format, tmp_line );
                }

                if( decode_video_index_information( h, &decklink_ctx->non_display_parser, anc_buf_pos, raw_frame, vii_line ) < 0 )
                    goto fail;

                if( decode_vbi( h, &decklink_ctx->non_display_parser, vbi_buf, raw_frame ) < 0 )
                    goto fail;

                av_free( vbi_buf );
            }

            av_free( anc_buf );
        }

        if( !decklink_opts_->probe )
        {
            raw_frame->alloc_img.width = width;
            raw_frame->alloc_img.height = height;
            if( h->filter_bit_depth == OBE_BIT_DEPTH_8 )
            {
                raw_frame->alloc_img.csp = AV_PIX_FMT_UYVY422;
                int size = av_image_alloc( raw_frame->alloc_img.plane, raw_frame->alloc_img.stride,
                                           raw_frame->alloc_img.width, raw_frame->alloc_img.height,
                                           (AVPixelFormat)raw_frame->alloc_img.csp, 32 );
                if( size < 0 )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return -1;
                }

                uint8_t *dst = raw_frame->alloc_img.plane[0];
                uint8_t *uyvy_ptr = (uint8_t*)frame_bytes;
                for( int i = 0; i < raw_frame->alloc_img.height; i++ )
                {
                    memcpy( dst, uyvy_ptr, raw_frame->alloc_img.width *2 );
                    uyvy_ptr += stride;
                    dst += raw_frame->alloc_img.stride[0];
                }

                raw_frame->buf_ref[0] = av_buffer_create( raw_frame->alloc_img.plane[0],
                                                          size, av_buffer_default_free,
                                                          NULL, 0 );
                if( !raw_frame->buf_ref[0] )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return -1;
                }
                raw_frame->buf_ref[1] = NULL;
            }
            else
            {
                decklink_ctx->codec->width = width;
                decklink_ctx->codec->height = height;

                pkt.data = (uint8_t*)frame_bytes;
                pkt.size = stride * height;

                ret = avcodec_decode_video2( decklink_ctx->codec, decklink_ctx->frame, &finished, &pkt );
                if( ret < 0 || !finished )
                {
                    syslog( LOG_ERR, "[decklink]: Could not decode video frame\n" );
                    goto end;
                }

                memcpy( raw_frame->buf_ref, decklink_ctx->frame->buf, sizeof(decklink_ctx->frame->buf) );

                memcpy( raw_frame->alloc_img.stride, decklink_ctx->frame->linesize, sizeof(raw_frame->alloc_img.stride) );
                memcpy( raw_frame->alloc_img.plane, decklink_ctx->frame->data, sizeof(raw_frame->alloc_img.plane) );
                raw_frame->alloc_img.csp = decklink_ctx->codec->pix_fmt;
            }

            raw_frame->release_data = obe_release_bufref;
            raw_frame->release_frame = obe_release_frame;
            
            raw_frame->alloc_img.planes = av_pix_fmt_count_planes( (AVPixelFormat)raw_frame->alloc_img.csp );
            raw_frame->alloc_img.format = decklink_opts_->video_format;

            memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );
            if( IS_SD( decklink_opts_->video_format ) )
            {
                raw_frame->img.first_line = first_active_line[j].line;
                if( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC )
                {
                    raw_frame->img.height = 480;
                    while( raw_frame->img.first_line != NTSC_FIRST_CODED_LINE )
                    {
                        for( int i = 0; i < raw_frame->img.planes; i++ )
                            raw_frame->img.plane[i] += raw_frame->img.stride[i];

                        raw_frame->img.first_line = sdi_next_line( INPUT_VIDEO_FORMAT_NTSC, raw_frame->img.first_line );
                    }
                }
            }

            /* If AFD is present and the stream is SD this will be changed in the video filter */
            raw_frame->sar_width = raw_frame->sar_height = 1;
            pts = raw_frame->pts = av_rescale_q( decklink_ctx->v_counter++, decklink_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );

            for( int i = 0; i < h->device.num_input_streams; i++ )
            {
                if( h->device.streams[i]->stream_format == VIDEO_UNCOMPRESSED )
                    raw_frame->input_stream_id = h->device.streams[i]->input_stream_id;
            }

            /* Make a copy of the frame for showing the last frame */
            if( decklink_opts_->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
            {
                int i = 0;
                if( decklink_ctx->stored_video_frame.release_data )
                    decklink_ctx->stored_video_frame.release_data( &decklink_ctx->stored_video_frame );

                memcpy( &decklink_ctx->stored_video_frame, raw_frame, sizeof(decklink_ctx->stored_video_frame) );
                while( raw_frame->buf_ref[i] != NULL )
                {
                    decklink_ctx->stored_video_frame.buf_ref[i] = av_buffer_ref( raw_frame->buf_ref[i] );
                    i++;
                }
                decklink_ctx->stored_video_frame.buf_ref[i] = NULL;
                decklink_ctx->stored_video_frame.release_data = obe_release_bufref;
                decklink_ctx->stored_video_frame.num_user_data = 0;
                decklink_ctx->stored_video_frame.user_data = NULL;
            }

            if( add_to_filter_queue( h, raw_frame ) < 0 )
                goto fail;

            if( send_vbi_and_ttx( h, &decklink_ctx->non_display_parser, pts ) < 0 )
                goto fail;

            decklink_ctx->non_display_parser.num_vbi = 0;
            decklink_ctx->non_display_parser.num_anc_vbi = 0;
        }
    }

    /* TODO: probe SMPTE 337M audio */

    if( videoframe && !decklink_opts_->probe )
    {
        int restart_input = 0;
        int src_frames, dst_frames;

        if( audioframe )
        {
            audioframe->GetBytes( &frame_bytes );
            src_frames = audioframe->GetSampleFrameCount();
            if( IS_PAL( decklink_opts_->video_format ) && src_frames != 1920 )
            {
                syslog( LOG_ERR, "Invalid audio packet length, attempting to correct, distortion may occur. Received: %i Expected: 1920 \n", src_frames );
                dst_frames = 1920;
                decklink_ctx->a_errors++;
            }
            else
            {
                dst_frames = audioframe->GetSampleFrameCount();
                decklink_ctx->a_errors = 0;
            }
        }
        else if( !audioframe && IS_PAL( decklink_opts_->video_format ) )
        {
            /* generate silence */
            dst_frames = 1920;
            restart_input = 1;
            syslog( LOG_ERR, "No audio packet, generating silence and restarting input \n" );
        }
        else
        {
            syslog( LOG_ERR, "No audio packet, lipsync issues will occur \n" );
            /* XXX: possibly guessing the NTSC pattern will be ok? */
            goto end;
        }

        if( decklink_ctx->a_errors > 20 )
        {
            decklink_ctx->a_errors = 0;
            restart_input = 1;
        }

        raw_frame = new_raw_frame();
        if( !raw_frame )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        raw_frame->audio_frame.num_samples = dst_frames;
        raw_frame->audio_frame.num_channels = decklink_opts_->num_channels;
        raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;

        if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, decklink_opts_->num_channels,
                              raw_frame->audio_frame.num_samples, (AVSampleFormat)raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            return -1;
        }

        if( audioframe && src_frames > 0 )
        {
            if( avresample_convert( decklink_ctx->avr, raw_frame->audio_frame.audio_data, raw_frame->audio_frame.linesize,
                                    dst_frames, (uint8_t**)&frame_bytes, 0, src_frames) < 0 )
            {
                syslog( LOG_ERR, "[decklink] Sample format conversion failed\n" );
                return -1;
            }
        }
        else if( IS_PAL( decklink_opts_->video_format ) )
        {
            /* Encode 1920 samples of silence */
            av_samples_set_silence( raw_frame->audio_frame.audio_data, 0, raw_frame->audio_frame.num_samples,
                                    raw_frame->audio_frame.num_channels, (AVSampleFormat)raw_frame->audio_frame.sample_fmt );
        }

        raw_frame->pts = av_rescale_q( decklink_ctx->a_counter, decklink_ctx->a_timebase, (AVRational){1, OBE_CLOCK} );
        if( pts != -1 )
        {
            raw_frame->video_pts = pts;
            raw_frame->video_duration = av_rescale_q( 1, decklink_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
        }
        decklink_ctx->a_counter += raw_frame->audio_frame.num_samples;
        raw_frame->release_data = obe_release_audio_data;
        raw_frame->release_frame = obe_release_frame;
        for( int i = 0; i < h->device.num_input_streams; i++ )
        {
            if( h->device.streams[i]->stream_format == AUDIO_PCM )
                raw_frame->input_stream_id = h->device.streams[i]->input_stream_id;
        }

        if( add_to_filter_queue( decklink_ctx->h, raw_frame ) < 0 )
            goto fail;

        if( restart_input )
        {
            int i = 0;
            for( i = 0; decklink_video_format_tab[i].obe_name != -1; i++ )
            {
                if( decklink_video_format_tab[i].obe_name != INPUT_VIDEO_FORMAT_AUTODETECT &&
                    decklink_video_format_tab[i].obe_name == decklink_opts_->video_format )
                    break;
            }

            if( decklink_video_format_tab[i].obe_name == -1 )
            {
                syslog( LOG_WARNING, "Unsupported video format" );
                return S_OK;
            }

            BMDPixelFormat pix_fmt;
            decklink_ctx->p_input->PauseStreams();
            pix_fmt = h->filter_bit_depth == OBE_BIT_DEPTH_10 ? bmdFormat10BitYUV : bmdFormat8BitYUV;
            decklink_ctx->p_input->EnableVideoInput( decklink_video_format_tab[i].bmd_name, pix_fmt, bmdVideoInputEnableFormatDetection );
            decklink_ctx->p_input->FlushStreams();
            decklink_ctx->p_input->StartStreams();
        }
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

    if( decklink_ctx->frame )
        av_frame_free( &decklink_ctx->frame );

    if( decklink_ctx->codec )
    {
        avcodec_close( decklink_ctx->codec );
        avcodec_free_context( &decklink_ctx->codec );
    }

    if( IS_SD( decklink_opts->video_format ) )
        vbi_raw_decoder_destroy( &decklink_ctx->non_display_parser.vbi_decoder );

    if( decklink_ctx->avr )
        avresample_free( &decklink_ctx->avr );

    if( decklink_ctx->raw_frames )
        free( decklink_ctx->raw_frames );

    if( decklink_ctx->bars_hnd )
        close_bars( decklink_ctx->bars_hnd );

    /* Stored frames are not malloced */
    if( decklink_ctx->stored_video_frame.release_data )
        decklink_ctx->stored_video_frame.release_data( &decklink_ctx->stored_video_frame );

    if( decklink_ctx->stored_audio_frame.release_data )
        decklink_ctx->stored_audio_frame.release_data( &decklink_ctx->stored_audio_frame );
}

static int open_card( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;
    obe_t       *h = decklink_ctx->h;
    int         found_mode;
    int         ret = 0;
    int         i;
    const int   sample_rate = DECKLINK_SAMPLE_RATE;
    const char *model_name;
    BMDDisplayMode wanted_mode_id;
    BMDPixelFormat pix_fmt;
    IDeckLinkAttributes *decklink_attributes = NULL;
    uint32_t    flags = 0;
    bool        supported;
    const struct obe_to_decklink_video *decklink_format;

    IDeckLinkDisplayModeIterator *p_display_iterator = NULL;
    IDeckLinkIterator *decklink_iterator = NULL;
    HRESULT result;

    if( h->filter_bit_depth == OBE_BIT_DEPTH_10 && !decklink_opts->probe )
    {
        decklink_ctx->frame = av_frame_alloc();
        if( !decklink_ctx->frame )
        {
            fprintf( stderr, "[decklink] Could not allocate frame\n" );
            goto finish;
        }

        decklink_ctx->dec = avcodec_find_decoder( AV_CODEC_ID_V210 );
        if( !decklink_ctx->dec )
        {
            fprintf( stderr, "[decklink] Could not find v210 decoder\n" );
            goto finish;
        }

        decklink_ctx->codec = avcodec_alloc_context3( decklink_ctx->dec );
        if( !decklink_ctx->codec )
        {
            fprintf( stderr, "[decklink] Could not allocate AVCodecContext\n" );
            goto finish;
        }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 48, 102)
        decklink_ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;
#endif

        /* TODO: setup custom strides */
        if( avcodec_open2( decklink_ctx->codec, decklink_ctx->dec, NULL ) < 0 )
        {
            fprintf( stderr, "[decklink] Could not open libavcodec\n" );
            goto finish;
        }
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
    free( (char *)model_name );

    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkInput, (void**)&decklink_ctx->p_input ) != S_OK )
    {
        fprintf( stderr, "[decklink] Card has no inputs\n" );
        ret = -1;
        goto finish;
    }

    /* Set up the video and audio sources. */
    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkConfiguration, (void**)&decklink_ctx->p_config ) != S_OK )
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

    result = decklink_ctx->p_card->QueryInterface(IID_IDeckLinkAttributes, (void**)&decklink_attributes );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not obtain the IDeckLinkAttributes interface\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_attributes->GetFlag( BMDDeckLinkSupportsInputFormatDetection, &supported );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not query card for format detection\n" );
        ret = -1;
        goto finish;
    }

    if( decklink_opts->video_format == INPUT_VIDEO_FORMAT_AUTODETECT )
    {
        if( supported )
            flags = bmdVideoInputEnableFormatDetection;
        decklink_opts->video_format = INPUT_VIDEO_FORMAT_PAL;
    }

    if( decklink_opts->downscale && supported )
        flags = bmdVideoInputEnableFormatDetection;

    /* Get the list of display modes. */
    result = decklink_ctx->p_input->GetDisplayModeIterator( &p_display_iterator );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enumerate display modes\n" );
        ret = -1;
        goto finish;
    }

    for( i = 0; decklink_video_format_tab[i].obe_name != -1; i++ )
    {
        if( decklink_video_format_tab[i].obe_name == decklink_opts->video_format )
            break;
    }

    if( decklink_video_format_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video format\n" );
        ret = -1;
        goto finish;
    }
    decklink_format = &decklink_video_format_tab[i];

    wanted_mode_id = decklink_video_format_tab[i].bmd_name;
    found_mode = false;
    decklink_ctx->v_timebase.num = decklink_opts->timebase_num = decklink_video_format_tab[i].timebase_num;
    decklink_ctx->v_timebase.den = decklink_opts->timebase_den = decklink_video_format_tab[i].timebase_den;

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
            get_format_opts( decklink_opts, p_display_mode );
            setup_pixel_funcs( decklink_opts );
        }

        p_display_mode->Release();
    }

    if( !found_mode )
    {
        fprintf( stderr, "[decklink] Unsupported video mode\n" );
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

    pix_fmt = h->filter_bit_depth == OBE_BIT_DEPTH_10 ? bmdFormat10BitYUV : bmdFormat8BitYUV;
    result = decklink_ctx->p_input->EnableVideoInput( wanted_mode_id, pix_fmt, flags );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable video input\n" );
        ret = -1;
        goto finish;
    }

    /* Set up audio. */
    decklink_ctx->a_timebase.num = 1;
    decklink_ctx->a_timebase.den = DECKLINK_SAMPLE_RATE;
    result = decklink_ctx->p_input->EnableAudioInput( sample_rate, bmdAudioSampleType32bitInteger, decklink_opts->num_channels );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable audio input\n" );
        ret = -1;
        goto finish;
    }

    if( !decklink_opts->probe )
    {
        if( decklink_opts->picture_on_loss )
        {
            decklink_ctx->sample_pattern = get_sample_pattern( decklink_opts->video_format );
            if( !decklink_ctx->sample_pattern )
            {
                fprintf( stderr, "[decklink] Invalid sample pattern" );
                ret = -1;
                goto finish;
            }

            /* Setup Picture on Loss */
            if( decklink_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
                decklink_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
            {
                setup_stored_video_frame( &decklink_ctx->stored_video_frame, decklink_format->width, decklink_format->height );
                blank_yuv422p_frame( &decklink_ctx->stored_video_frame );
            }
            else if( decklink_opts->picture_on_loss == PICTURE_ON_LOSS_BARS )
            {
                decklink_ctx->raw_frames = (obe_raw_frame_t**)calloc( 2, sizeof(*decklink_ctx->raw_frames) );
                if( !decklink_ctx->raw_frames )
                {
                    fprintf( stderr, "[decklink] Malloc failed\n" );
                    ret = -1;
                    goto finish;
                }

                decklink_opts->obe_bars_opts.video_format = decklink_opts->video_format;
                /* Setup bars later if we don't know the video format */
                if( open_bars( &decklink_ctx->bars_hnd, &decklink_opts->obe_bars_opts ) < 0 )
                {
                    fprintf( stderr, "[decklink] Could not open bars\n" );
                    ret = -1;
                    goto finish;
                }
            }

            /* Setup stored audio frame */
            if( decklink_opts->picture_on_loss == PICTURE_ON_LOSS_BLACK ||
                decklink_opts->picture_on_loss == PICTURE_ON_LOSS_LASTFRAME )
            {
                setup_stored_audio_frame( &decklink_ctx->stored_audio_frame, decklink_ctx->sample_pattern->max );
            }
        }

        /* Setup Audio filtering */
        decklink_ctx->avr = avresample_alloc_context();
        if( !decklink_ctx->avr )
        {
            fprintf( stderr, "[decklink-sdiaudio] couldn't setup sample rate conversion \n" );
            ret = -1;
            goto finish;
        }

        /* Give libavresample a made up channel map */
        av_opt_set_int( decklink_ctx->avr, "in_channel_layout",   (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_rate",      DECKLINK_SAMPLE_RATE, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_channel_layout",  (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );

        if( avresample_open( decklink_ctx->avr ) < 0 )
        {
            fprintf( stderr, "Could not open AVResample\n" );
            goto finish;
        }
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

    if( decklink_attributes )
        decklink_attributes->Release();

    return ret;
}

static void close_thread( void *handle )
{
    struct decklink_status *status = (decklink_status *)handle;

    if( status->decklink_opts )
    {
        close_card( status->decklink_opts );
        free( status->decklink_opts );
    }

    free( status->input );
}

static void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int cur_stream = 2, cur_input_stream_id = 0;
    obe_sdi_non_display_data_t *non_display_parser;
    decklink_ctx_t *decklink_ctx;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    non_display_parser = &decklink_opts->decklink_ctx.non_display_parser;

    /* TODO: support multi-channel */
    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    decklink_opts->downscale = user_opts->downscale;

    decklink_opts->probe = non_display_parser->probe = 1;

    decklink_ctx = &decklink_opts->decklink_ctx;
    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    if( open_card( decklink_opts ) < 0 )
        goto finish;

    sleep( 1 );

    close_card( decklink_opts );

    if( !decklink_opts->probe_success )
    {
        fprintf( stderr, "[decklink] No valid frames received - check connection and input format\n" );
        goto finish;
    }

    /* TODO: probe for SMPTE 337M */
    /* TODO: factor some of the code below out */

    for( int i = 0; i < 2; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            goto finish;

        streams[i]->input_stream_id = cur_input_stream_id++;

        if( i == 0 )
        {
            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->video_format = decklink_opts->video_format;
            streams[i]->width  = decklink_opts->width;
            streams[i]->height = decklink_opts->height;
            streams[i]->timebase_num = decklink_opts->timebase_num;
            streams[i]->timebase_den = decklink_opts->timebase_den;
            streams[i]->csp    = AV_PIX_FMT_YUV422P10;
            streams[i]->interlaced = decklink_opts->interlaced;
            streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */

            if( add_non_display_services( non_display_parser, streams[i], USER_DATA_LOCATION_FRAME ) < 0 )
                goto finish;
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels  = 16;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[i]->sample_rate = DECKLINK_SAMPLE_RATE;
        }
    }

    if( non_display_parser->has_vbi_frame )
    {
        streams[cur_stream] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[cur_stream]) );
        if( !streams[cur_stream] )
            goto finish;

        streams[cur_stream]->input_stream_id = cur_input_stream_id++;

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = VBI_RAW;
        streams[cur_stream]->vbi_ntsc = decklink_opts->video_format == INPUT_VIDEO_FORMAT_NTSC;
        if( add_non_display_services( non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->has_ttx_frame )
    {
        streams[cur_stream] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[cur_stream]) );
        if( !streams[cur_stream] )
            goto finish;

        streams[cur_stream]->input_stream_id = cur_input_stream_id++;

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = MISC_TELETEXT;
        if( add_teletext_service( non_display_parser, streams[cur_stream] ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->num_frame_data )
        free( non_display_parser->frame_data );

    init_device(&h->device);
    h->device.num_input_streams = cur_stream;
    memcpy( h->device.streams, streams, h->device.num_input_streams * sizeof(obe_int_input_stream_t**) );
    h->device.device_type = INPUT_DEVICE_DECKLINK;
    memcpy( &h->device.user_opts, user_opts, sizeof(*user_opts) );
    // FIXME destroy mutex

finish:
    free( decklink_opts );

    return NULL;
}

static void *autoconf_input( void *ptr )
{
    obe_int_input_stream_t *streams[MAX_STREAMS];
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    int cur_input_stream_id = 0;

    for( int i = 0; i < 3; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            return NULL;

        streams[i]->input_stream_id = cur_input_stream_id++;

        if( i == 0 )
        {
            int j;
            for( j = 0; decklink_video_format_tab[j].obe_name != -1; j++ )
            {
                if( decklink_video_format_tab[j].obe_name == user_opts->video_format )
                    break;
            }

            if( decklink_video_format_tab[j].obe_name == -1 )
            {
                fprintf( stderr, "[decklink] Unsupported video format\n" );
                return NULL;
            }

            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->video_format = user_opts->video_format;
            streams[i]->width  = decklink_video_format_tab[j].width;
            streams[i]->height = decklink_video_format_tab[j].height;
            streams[i]->timebase_num = decklink_video_format_tab[j].timebase_num;
            streams[i]->timebase_den = decklink_video_format_tab[j].timebase_den;
            streams[i]->csp    = AV_PIX_FMT_YUV422P10;
            streams[i]->interlaced = decklink_video_format_tab[j].interlaced;
            streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels  = 16;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[i]->sample_rate = DECKLINK_SAMPLE_RATE;
        }
        else if( i == 2 )
        {
            streams[i]->stream_type = STREAM_TYPE_MISC;
            streams[i]->stream_format = MISC_TELETEXT;
        }
    }

    init_device(&h->device);
    h->device.num_input_streams = 3;
    memcpy( h->device.streams, streams, h->device.num_input_streams * sizeof(obe_int_input_stream_t**) );
    h->device.device_type = INPUT_DEVICE_DECKLINK;
    memcpy( &h->device.user_opts, user_opts, sizeof(*user_opts) );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_input_t *user_opts = &h->device.user_opts;
    decklink_ctx_t *decklink_ctx;
    obe_sdi_non_display_data_t *non_display_parser;
    struct decklink_status status;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    status.input = input;
    status.decklink_opts = decklink_opts;

    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    decklink_opts->picture_on_loss = user_opts->picture_on_loss;
    decklink_opts->downscale = user_opts->downscale;

    decklink_opts->obe_bars_opts.video_format = user_opts->video_format;
    decklink_opts->obe_bars_opts.bars_line1 = user_opts->bars_line1;
    decklink_opts->obe_bars_opts.bars_line2 = user_opts->bars_line2;
    decklink_opts->obe_bars_opts.bars_line3 = user_opts->bars_line3;
    decklink_opts->obe_bars_opts.bars_line4 = user_opts->bars_line4;
    decklink_opts->obe_bars_opts.no_signal = 1;

    decklink_ctx = &decklink_opts->decklink_ctx;

    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    non_display_parser = &decklink_ctx->non_display_parser;
    non_display_parser->device = &h->device;

    /* TODO: wait for encoder */

    if( open_card( decklink_opts ) < 0 )
        return NULL;

    pthread_mutex_lock( &h->device_mutex );
    while (!h->device.stop)
        pthread_cond_wait(&h->device_cond, &h->device_mutex);
    pthread_mutex_unlock( &h->device_mutex);

    close_thread(&status);

    return NULL;
}

const obe_input_func_t decklink_input = { probe_stream, autoconf_input, open_input };
