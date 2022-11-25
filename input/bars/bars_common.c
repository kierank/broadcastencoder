/*****************************************************************************
 * bars_common.c: SMPTE bars common files
 *****************************************************************************
 * Copyright (C) 2014 Open Broadcast Systems Ltd.
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

#include "bars_common.h"
#include "common/common.h"
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>

#define TONE_PEAK 0xCCCD
#define FREQ 1000
#define X_OFFSET 100
#define Y_OFFSET 100
#define X_WIDTH 200
#define Y_WIDTH 200
#define DEFAULT_PERIOD 2

typedef struct
{
    obe_bars_opts_t *bars_opts;
    const obe_video_config_t *format;

    int64_t v_pts;
    AVFilterGraph *v_filter_graph;
    AVFilterContext *v_buffersink_ctx;
    AVFrame *frame;
    int beep;
    int beep_frame_period;

    const obe_audio_sample_pattern_t *sample_pattern;
    int64_t audio_samples;
} bars_ctx_t;

static const char *format_strings[] =
{
    [INPUT_VIDEO_FORMAT_PAL]        = "625i",
    [INPUT_VIDEO_FORMAT_NTSC]       = "525i",
    [INPUT_VIDEO_FORMAT_720P_50]    = "720p50",
    [INPUT_VIDEO_FORMAT_720P_5994]  = "720p59.94",
    [INPUT_VIDEO_FORMAT_720P_60]    = "720p60",
    [INPUT_VIDEO_FORMAT_1080I_50]   = "1080i25",
    [INPUT_VIDEO_FORMAT_1080I_5994] = "1080i29.97",
    [INPUT_VIDEO_FORMAT_1080P_2398] = "1080p23.98",
    [INPUT_VIDEO_FORMAT_1080P_24]   = "1080p24",
    [INPUT_VIDEO_FORMAT_1080P_25]   = "1080p25",
    [INPUT_VIDEO_FORMAT_1080P_2997] = "1080p29.97",
    [INPUT_VIDEO_FORMAT_1080P_30]   = "1080p30",
    [INPUT_VIDEO_FORMAT_1080P_50]   = "1080p50",
    [INPUT_VIDEO_FORMAT_1080P_5994] = "1080p59.94",
    [INPUT_VIDEO_FORMAT_1080P_60]   = "1080p60",
};

static void drawtext_set_font_size(AVFilterContext *drawtext_ctx, int font_size)
{
    const AVOption *o = av_opt_find(drawtext_ctx, "fontsize", NULL, 0, AV_OPT_SEARCH_CHILDREN);
    if (o && o->type == AV_OPT_TYPE_STRING) {
        char font_size_str[32];
        snprintf(font_size_str, sizeof(font_size_str), "%d", font_size);
        av_opt_set( drawtext_ctx, "fontsize", font_size_str, AV_OPT_SEARCH_CHILDREN );
    }
    else
        av_opt_set_int( drawtext_ctx, "fontsize", font_size, AV_OPT_SEARCH_CHILDREN );
}


int open_bars( hnd_t *p_handle, obe_bars_opts_t *bars_opts )
{
    int ret = 0;
    const char *fontfile = "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf";
    char tmp[50];
    int interlaced = 0;

    AVFilter        *buffersink = avfilter_get_by_name( "buffersink" );
    AVFilter        *interlace = avfilter_get_by_name( "interlace" );
    AVFilterContext *interlace_ctx = NULL;
    AVFilter        *drawtext = avfilter_get_by_name( "drawtext" );
    AVFilterContext *drawtext_ctx1;
    AVFilterContext *drawtext_ctx2;
    AVFilterContext *drawtext_ctx3;
    AVFilterContext *drawtext_ctx4;
    AVFilterContext *drawtext_ctx5;
    AVFilter        *format = avfilter_get_by_name( "format" );
    AVFilterContext *format_ctx;
    AVFilter        *smptesrc;
    AVFilterContext *smptesrc_ctx;

    bars_ctx_t *bars_ctx = calloc( 1, sizeof(*bars_ctx) );
    if( !bars_ctx )
    {
        fprintf( stderr, "malloc failed \n" );
        goto end;
    }

    bars_ctx->bars_opts = bars_opts;

    /* Setup bars and text */
    int j = 0;
    for( ; video_format_tab[j].obe_name != -1; j++ )
    {
        if( bars_opts->video_format == video_format_tab[j].obe_name )
            break;
    }

    if( video_format_tab[j].obe_name == -1 )
    {
        fprintf( stderr, "Invalid video format \n" );
        ret = -1;
        goto end;
    }

    bars_ctx->format = &video_format_tab[j];
    int font_size;
    if( IS_SD( video_format_tab[j].obe_name ) )
    {
        font_size = bars_ctx->format->height == 480 ? 50 : 60;
        smptesrc = avfilter_get_by_name( "smptebars" );
        interlaced = 1;
    }
    else
    {
        font_size = bars_ctx->format->height == 720 ? 75 : 120;
        smptesrc = avfilter_get_by_name( "smptehdbars" );
        if( IS_INTERLACED( bars_ctx->format->obe_name ) )
            interlaced = 1;
    }

    /* allocate the filtergraph */
    bars_ctx->v_filter_graph = avfilter_graph_alloc();
    if( !bars_ctx->v_filter_graph )
    {
        fprintf( stderr, "Could not allocate filter graph \n" );
        ret = -1;
        goto end;
    }

    smptesrc_ctx = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, smptesrc, "src" );
    if( !smptesrc_ctx )
    {
        fprintf( stderr, "Could not allocate smpte source \n" );
        ret = -1;
        goto end;
    }

    snprintf( tmp, sizeof(tmp), "%ix%i", bars_ctx->format->width, bars_ctx->format->height );
    av_opt_set( smptesrc_ctx, "s", tmp, AV_OPT_SEARCH_CHILDREN );

    snprintf( tmp, sizeof(tmp), "%i/%i", bars_ctx->format->timebase_den * (1+interlaced), bars_ctx->format->timebase_num );
    av_opt_set( smptesrc_ctx, "rate", tmp, AV_OPT_SEARCH_CHILDREN );

    if( avfilter_init_str( smptesrc_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init smpte source \n" );
        ret = -1;
        goto end;
    }

    format_ctx = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, format, "format" );
    if( !format_ctx )
    {
        fprintf( stderr, "Could not allocate format filter \n" );
        ret = -1;
        goto end;
    }

    av_opt_set( format_ctx, "pix_fmts", "yuv422p", AV_OPT_SEARCH_CHILDREN );

    if( avfilter_init_str( format_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init format filter \n" );
        ret = -1;
        goto end;
    }

    /* First line (animated) */
    drawtext_ctx1 = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, drawtext, "drawtext1" );
    if( !drawtext_ctx1 )
    {
        fprintf( stderr, "Could not allocate drawtext filter \n" );
        ret = -1;
        goto end;
    }

    av_opt_set( drawtext_ctx1, "fontfile", fontfile, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx1, "text", bars_ctx->bars_opts->bars_line1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx1, "x", "(w-text_w)*sin(0.25*t)*sin(0.25*t)", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx1, "y", "(2*lh)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx1, "fontcolor", "white", AV_OPT_SEARCH_CHILDREN );
    av_opt_set_int( drawtext_ctx1, "box", 1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx1, "boxcolor", "black", AV_OPT_SEARCH_CHILDREN );
    drawtext_set_font_size(drawtext_ctx1, font_size);

    if( avfilter_init_str( drawtext_ctx1, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init drawtext filter \n" );
        ret = -1;
        goto end;
    }

    /* Second line */
    drawtext_ctx2 = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, drawtext, "drawtext2" );
    if( !drawtext_ctx2 )
    {
        fprintf( stderr, "Could not allocate drawtext filter \n" );
        ret = -1;
        goto end;
    }

    av_opt_set( drawtext_ctx2, "fontfile", fontfile, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx2, "text", bars_ctx->bars_opts->bars_line2, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx2, "x", "(w-text_w)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx2, "y", "(5*lh)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx2, "fontcolor", "white", AV_OPT_SEARCH_CHILDREN );
    av_opt_set_int( drawtext_ctx2, "box", 1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx2, "boxcolor", "black", AV_OPT_SEARCH_CHILDREN );
    drawtext_set_font_size(drawtext_ctx2, font_size);

    if( avfilter_init_str( drawtext_ctx2, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init drawtext filter \n" );
        ret = -1;
        goto end;
    }

    /* Third line */
    drawtext_ctx3 = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, drawtext, "drawtext3" );
    if( !drawtext_ctx3 )
    {
        fprintf( stderr, "Could not allocate drawtext filter \n" );
        ret = -1;
        goto end;
    }

    av_opt_set( drawtext_ctx3, "fontfile", fontfile, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx3, "text", bars_ctx->bars_opts->bars_line3, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx3, "x", "(w-text_w)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx3, "y", "(8*lh)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx3, "fontcolor", "white", AV_OPT_SEARCH_CHILDREN );
    av_opt_set_int( drawtext_ctx3, "box", 1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx3, "boxcolor", "black", AV_OPT_SEARCH_CHILDREN );
    drawtext_set_font_size(drawtext_ctx3, font_size);

    if( avfilter_init_str( drawtext_ctx3, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init drawtext filter \n" );
        ret = -1;
        goto end;
    }

    /* Fourth line */
    drawtext_ctx4 = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, drawtext, "drawtext4" );
    if( !drawtext_ctx4 )
    {
        fprintf( stderr, "Could not allocate drawtext filter \n" );
        ret = -1;
        goto end;
    }

    av_opt_set( drawtext_ctx4, "fontfile", fontfile, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx4, "text", bars_ctx->bars_opts->bars_line4, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx4, "x", "(w-text_w)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx4, "y", "(11*lh)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx4, "fontcolor", "white", AV_OPT_SEARCH_CHILDREN );
    av_opt_set_int( drawtext_ctx4, "box", 1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx4, "boxcolor", "black", AV_OPT_SEARCH_CHILDREN );
    drawtext_set_font_size(drawtext_ctx4, font_size);

    if( avfilter_init_str( drawtext_ctx4, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init drawtext filter \n" );
        ret = -1;
        goto end;
    }

    /* Fifth line (video format) */
    drawtext_ctx5 = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, drawtext, "drawtext5" );
    if( !drawtext_ctx5 )
    {
        fprintf( stderr, "Could not allocate drawtext filter \n" );
        ret = -1;
        goto end;
    }

    snprintf( tmp, sizeof(tmp), "%s", format_strings[bars_ctx->format->obe_name] );
    if( bars_ctx->bars_opts->no_signal )
        strcat( tmp, " - NO SIGNAL" );
    av_opt_set( drawtext_ctx5, "fontfile", fontfile, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx5, "text", tmp, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx5, "x", "(w-text_w)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx5, "y", "(14*lh)/2", AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx5, "fontcolor", "white", AV_OPT_SEARCH_CHILDREN );
    av_opt_set_int( drawtext_ctx5, "box", 1, AV_OPT_SEARCH_CHILDREN );
    av_opt_set( drawtext_ctx5, "boxcolor", "black", AV_OPT_SEARCH_CHILDREN );
    drawtext_set_font_size(drawtext_ctx5, font_size);

    if( avfilter_init_str( drawtext_ctx5, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init drawtext filter \n" );
        ret = -1;
        goto end;
    }

    if( interlaced )
    {
        /* Interlace filter */
        interlace_ctx = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, interlace, "interlace" );
        if( !interlace_ctx )
        {
            fprintf( stderr, "Could not allocate interlace filter \n" );
            ret = -1;
            goto end;
        }
    }

    bars_ctx->v_buffersink_ctx = avfilter_graph_alloc_filter( bars_ctx->v_filter_graph, buffersink, "sink" );
    if( !bars_ctx->v_buffersink_ctx )
    {
        fprintf( stderr, "Could not allocate buffer sink \n" );
        ret = -1;
        goto end;
    }

    if( avfilter_init_str( bars_ctx->v_buffersink_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init buffer sink \n" );
        ret = -1;
        goto end;
    }

    int err;
    err = avfilter_link( smptesrc_ctx, 0, format_ctx, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    err = avfilter_link( format_ctx, 0, drawtext_ctx1, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    err = avfilter_link( drawtext_ctx1, 0, drawtext_ctx2, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    err = avfilter_link( drawtext_ctx2, 0, drawtext_ctx3, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    err = avfilter_link( drawtext_ctx3, 0, drawtext_ctx4, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    err = avfilter_link( drawtext_ctx4, 0, drawtext_ctx5, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    AVFilterContext *final;
    if( interlaced )
    {
        err = avfilter_link( drawtext_ctx5, 0, interlace_ctx, 0 );
        if( err < 0 )
        {
            fprintf( stderr, "Could not link filter \n" );
            ret = -1;
            goto end;
        }
        final = interlace_ctx;
    }
    else
        final = drawtext_ctx5;

    err = avfilter_link( final, 0, bars_ctx->v_buffersink_ctx, 0 );
    if( err < 0 )
    {
        fprintf( stderr, "Could not link filter \n" );
        ret = -1;
        goto end;
    }

    /* configure the graph*/
    if( avfilter_graph_config( bars_ctx->v_filter_graph, NULL ) < 0 )
    {
        fprintf( stderr, "Could not configure graph \n" );
        ret = -1;
        goto end;
    }

    bars_ctx->frame = av_frame_alloc();
    if( !bars_ctx->frame )
    {
        fprintf( stderr, "Could not allocate frame \n" );
        goto end;
    }

    bars_ctx->sample_pattern = get_sample_pattern( bars_opts->video_format );
    if( !bars_ctx->sample_pattern )
    {
        fprintf( stderr, "Invalid sample format \n" );
        goto end;
    }

    int frame_period = bars_ctx->format->timebase_den;
    if( frame_period > 1000 )
        frame_period /= 1000;
    bars_ctx->beep_frame_period = frame_period * (bars_opts->bars_beep_interval ? bars_opts->bars_beep_interval : DEFAULT_PERIOD);

    *p_handle = bars_ctx;

end:

    return ret;
}

int get_bars( hnd_t ptr, obe_raw_frame_t **raw_frames )
{
    bars_ctx_t *bars_ctx = ptr;
    int ret = 0;
    obe_raw_frame_t *raw_frame;
    const obe_video_config_t *format = bars_ctx->format;
    int beep_now = bars_ctx->bars_opts->bars_beep && !(bars_ctx->v_pts % bars_ctx->beep_frame_period);

    while( 1 )
    {
        ret = av_buffersink_get_frame( bars_ctx->v_buffersink_ctx, bars_ctx->frame );
        if( ret == AVERROR(EAGAIN) || ret == AVERROR_EOF )
            continue;
        if( ret < 0 )
        {
            fprintf( stderr, "Could not get frame from buffersink \n" );
            return -1;
        }
        else
        {
            break;
        }
    }

    raw_frame = new_raw_frame();
    if( !raw_frame )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }

    raw_frame->alloc_img.width = format->width;
    raw_frame->alloc_img.height = format->height;

    memcpy( raw_frame->alloc_img.stride, bars_ctx->frame->linesize, sizeof(raw_frame->alloc_img.stride) );
    memcpy( raw_frame->alloc_img.plane, bars_ctx->frame->data, sizeof(raw_frame->alloc_img.plane) );
    raw_frame->alloc_img.csp = AV_PIX_FMT_YUV422P;
    raw_frame->alloc_img.planes = av_pix_fmt_count_planes(raw_frame->alloc_img.csp);
    raw_frame->alloc_img.format = format->obe_name;

    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );

    memcpy( raw_frame->buf_ref, bars_ctx->frame->buf, sizeof(bars_ctx->frame->buf) );
    memset( &bars_ctx->frame->buf, 0, sizeof(bars_ctx->frame->buf) );

    raw_frame->release_data = obe_release_bufref;
    raw_frame->release_frame = obe_release_frame;
    raw_frame->dup_frame = obe_dup_bufref;

    raw_frame->sar_width = raw_frame->sar_height = 1;

    raw_frame->pts = av_rescale_q( bars_ctx->v_pts,
                                   (AVRational){format->timebase_num, format->timebase_den},
                                   (AVRational){1, OBE_CLOCK} );

    if( beep_now )
    {
        for( int i = 0; i < Y_WIDTH; i++ )
        {
            memset( raw_frame->alloc_img.plane[0] + (Y_OFFSET + i)*raw_frame->alloc_img.stride[0] + X_OFFSET, 0x10, X_WIDTH );
        }
    }

    raw_frames[0] = raw_frame;

    raw_frame = new_raw_frame();
    if( !raw_frame )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }

    /* FIXME: Generate and store sine wave while making NTSC spacing work */
    raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
    raw_frame->audio_frame.num_samples = bars_ctx->sample_pattern->pattern[bars_ctx->v_pts % bars_ctx->sample_pattern->mod];
    raw_frame->audio_frame.num_channels = 16;

    /* Allocate one channel of tones and copy that to other planes */
    if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, 1,
                          raw_frame->audio_frame.num_samples, raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    int32_t *audio = (int32_t*)raw_frame->audio_frame.audio_data[0];
    if( beep_now )
    {
        for( int i = 0; i < raw_frame->audio_frame.num_samples; i++ )
            audio[i] = 0;
    }
    else
    {
        for( int i = 0; i < raw_frame->audio_frame.num_samples; i++ )
            audio[i] = (int32_t)(TONE_PEAK * sin (2 * M_PI * (bars_ctx->audio_samples+i) * FREQ / 48000) + 0.5) << 12;
    }

    for( int i = 1; i < MAX_CHANNELS; i++ )
        raw_frame->audio_frame.audio_data[i] = raw_frame->audio_frame.audio_data[0];

    raw_frame->pts = av_rescale_q( bars_ctx->audio_samples, (AVRational){1, 48000}, (AVRational){1, OBE_CLOCK} );
    raw_frame->video_pts = raw_frames[0]->pts;
    raw_frame->video_duration = av_rescale_q( 1, (AVRational){format->timebase_num, format->timebase_den}, (AVRational){1, OBE_CLOCK} );
    raw_frame->release_data = obe_release_audio_data;
    raw_frame->release_frame = obe_release_frame;
    raw_frames[1] = raw_frame;

    bars_ctx->audio_samples += raw_frame->audio_frame.num_samples;
    bars_ctx->v_pts++;

    return 0;
}

void close_bars( hnd_t ptr )
{
    bars_ctx_t *bars_ctx = ptr;

    avfilter_graph_free( &bars_ctx->v_filter_graph );
    av_frame_free( &bars_ctx->frame );
    free( bars_ctx );
    ptr = NULL;
}
