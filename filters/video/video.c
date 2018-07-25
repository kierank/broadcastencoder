/*****************************************************************************
 * video.c: basic video filter system
 *****************************************************************************
 * Copyright (C) 2010-2011 Open Broadcast Systems Ltd
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Authors: Oskar Arvidsson <oskar@irock.se>
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
 */

#include <libavutil/cpu.h>
#include <libavutil/opt.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "common/common.h"
#include "common/bitstream.h"
#include "video.h"
#include "cc.h"
#include "dither.h"
#include "x86/vfilter.h"
#include "input/sdi/sdi.h"

typedef struct
{
    int filter_bit_depth;

    /* cpu flags */
    uint32_t avutil_cpu;

    /* upscaling */
    void (*scale_plane)( uint16_t *src, int stride, int width, int height, int lshift, int rshift );

    /* resize */
    int src_width;
    int src_height;
    int src_csp;
    int dst_width;
    int dst_height;
    int dst_csp;
    AVFilterGraph   *resize_filter_graph;
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *resize_ctx;
    AVFilterContext *format_ctx;
    AVFilterContext *buffersink_ctx;
    AVFrame *frame;

    /* downsample */
    void (*downsample_chroma_fields_8)( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height );
    void (*downsample_chroma_fields_10)( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height );

    /* dither */
    void (*dither_plane_10_to_8)( uint16_t *src, int src_stride, uint8_t *dst, int dst_stride, int width, int height );
    int16_t *error_buf;
} obe_vid_filter_ctx_t;

typedef struct
{
    int planes;
    float width[4];
    float height[4];
    int mod_width;
    int mod_height;
    int bit_depth;
} obe_cli_csp_t;

typedef struct
{
    int width;
    int height;
    int sar_width;
    int sar_height;
} obe_sar_t;

typedef struct
{
    int afd_code;
    int is_wide;
} obe_wss_to_afd_t;

const static obe_cli_csp_t obe_cli_csps[] =
{
    [AV_PIX_FMT_YUV420P] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 8 },
    [AV_PIX_FMT_YUV422P] = { 3, { 1, .5, .5 }, { 1, 1, 1 }, 2, 2, 8 },
    [AV_PIX_FMT_YUV420P10] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 10 },
    [AV_PIX_FMT_YUV422P10] = { 3, { 1, .5, .5 }, { 1, 1, 1 }, 2, 2, 10 },
    [AV_PIX_FMT_NV12] =    { 2, { 1,  1 },     { 1, .5 },     2, 2, 8 },
};

/* These SARs are often based on historical convention so often cannot be calculated */
const static obe_sar_t obe_sars[][17] =
{
    {
        /* NTSC */
        { 720, 480, 10, 11 },
        { 640, 480,  1,  1 },
        { 528, 480, 40, 33 },
        { 544, 480, 40, 33 },
        { 480, 480, 15, 11 },
        { 352, 480, 20, 11 },
        /* PAL */
        { 720, 576, 12, 11 },
        { 544, 576, 16, 11 },
        { 480, 576, 18, 11 },
        { 352, 576, 24, 11 },
        /* HD */
        { 1920, 1080, 1, 1 },
        { 1280,  720, 1, 1 },
        { 0 },
    },
    {
        /* NTSC */
        { 720, 480, 40, 33 },
        { 640, 480,  4,  3 },
        { 544, 480, 16, 99 },
        { 480, 480, 20, 11 },
        { 352, 480, 80, 33 },
        /* PAL */
        { 720, 576, 16, 11 },
        { 544, 576, 64, 33 },
        { 480, 576, 24, 11 },
        { 352, 576, 32, 11 },
        /* HD */
        { 1920, 1080, 1, 1 },
        { 1440, 1080, 4, 3 },
        { 1280, 1080, 3, 2 },
        {  960, 1080, 2, 1 },
        { 1280,  720, 1, 1 },
        {  960,  720, 4, 3 },
        {  640,  720, 2, 1 },
        { 0 },
    },
};

const static obe_wss_to_afd_t wss_to_afd[] =
{
    [0x0] = { 0x9, 0 }, /* 4:3 (centre) */
    [0x1] = { 0xb, 0 }, /* 14:9 (centre) */
    [0x2] = { 0x3, 0 }, /* box 14:9 (top) */
    [0x3] = { 0xa, 1 }, /* 16:9 (centre) */
    [0x4] = { 0x2, 1 }, /* box 16:9 (top) */
    [0x5] = { 0x4, 1 }, /* box > 16:9 (centre) */
    [0x6] = { 0xd, 0 }, /* 4:3 (shoot and protect 14:9 centre) */
    [0x7] = { 0xa, 1 }, /* 16:9 (shoot and protect 4:3 centre) */
};

static int set_sar( obe_raw_frame_t *raw_frame, int is_wide )
{
    for( int i = 0; obe_sars[is_wide][i].width != 0; i++ )
    {
        if( raw_frame->img.width == obe_sars[is_wide][i].width && raw_frame->img.height == obe_sars[is_wide][i].height )
        {
            raw_frame->sar_width  = obe_sars[is_wide][i].sar_width;
            raw_frame->sar_height = obe_sars[is_wide][i].sar_height;
            return 0;
        }
    }

    return -1;
}

static void dither_plane_10_to_8_c( uint16_t *src, int src_stride, uint8_t *dst, int dst_stride, int width, int height )
{
    const int scale = 511;
    const uint16_t shift = 11;

    for( int j = 0; j < height; j++ )
    {
        const uint16_t *dither = obe_dithers[j&7];
        int k;
        for (k = 0; k < width-7; k+=8)
        {
            dst[k+0] = (src[k+0] + dither[0])*scale>>shift;
            dst[k+1] = (src[k+1] + dither[1])*scale>>shift;
            dst[k+2] = (src[k+2] + dither[2])*scale>>shift;
            dst[k+3] = (src[k+3] + dither[3])*scale>>shift;
            dst[k+4] = (src[k+4] + dither[4])*scale>>shift;
            dst[k+5] = (src[k+5] + dither[5])*scale>>shift;
            dst[k+6] = (src[k+6] + dither[6])*scale>>shift;
            dst[k+7] = (src[k+7] + dither[7])*scale>>shift;
        }
        for (; k < width; k++)
            dst[k] = (src[k] + dither[k&7])*scale>>shift;

        src += src_stride / 2;
        dst += dst_stride;
    }
}

/* Note: srcf is the next field (two pixels down) */
static void downsample_chroma_fields_8_c( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height )
{
    uint8_t *src = src_ptr;
    uint8_t *dst = dst_ptr;
    for( int i = 0; i < height; i += 2 )
    {
        uint8_t *srcf = src + src_stride*2;

        /* Top field */
        for( int j = 0; j < width; j++ )
            dst[j] = (3*src[j] + srcf[j] + 2) >> 2;

        dst  += dst_stride;
        src  += src_stride;
        srcf += src_stride;

        /* Bottom field */
        for( int j = 0; j < width; j++ )
            dst[j] = (src[j] + 3*srcf[j] + 2) >> 2;

        dst += dst_stride;
        src = srcf + src_stride;
    }
}

static void downsample_chroma_fields_10_c( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height )
{
    uint16_t *src = src_ptr;
    uint16_t *dst = dst_ptr;
    for( int i = 0; i < height; i += 2 )
    {
        uint16_t *srcf = src + src_stride;

        /* Top field */
        for( int j = 0; j < width; j++ )
            dst[j] = (3*src[j] + srcf[j] + 2) >> 2;

        dst  += dst_stride/2;
        src  += src_stride/2;
        srcf += src_stride/2;

        /* Bottom field */
        for( int j = 0; j < width; j++ )
            dst[j] = (src[j] + 3*srcf[j] + 2) >> 2;

        dst += dst_stride/2;
        src = srcf + src_stride/2;
    }
}

static void init_filter( obe_t *h, obe_vid_filter_ctx_t *vfilt )
{
    vfilt->filter_bit_depth = h->filter_bit_depth;
    vfilt->avutil_cpu = av_get_cpu_flags();

    /* dither */
    vfilt->dither_plane_10_to_8 = dither_plane_10_to_8_c;

    /* downsampling */
    vfilt->downsample_chroma_fields_8 = downsample_chroma_fields_8_c;
    vfilt->downsample_chroma_fields_10 = downsample_chroma_fields_10_c;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_SSE2 )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_sse2;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_sse2;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_SSE4 )
        vfilt->dither_plane_10_to_8 = obe_dither_plane_10_to_8_sse4;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_avx;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_avx;

        vfilt->dither_plane_10_to_8 = obe_dither_plane_10_to_8_avx;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX2 )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_avx2;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_avx2;
    }
}

static int init_libavfilter( obe_t *h, obe_vid_filter_ctx_t *vfilt,
                             obe_output_stream_t *output_stream, obe_raw_frame_t *raw_frame )
{
    char tmp[1024];
    int ret = 0;
    int interlaced = 0;

    if( vfilt->resize_filter_graph )
    {
        avfilter_graph_free( &vfilt->resize_filter_graph );
        vfilt->resize_filter_graph = NULL;
    }

    if( !vfilt->frame )
    {
        vfilt->frame = av_frame_alloc();
        if( !vfilt->frame )
        {
            fprintf( stderr, "Could not allocate input frame \n" );
            ret = -1;
            goto end;
        }
    }

    /* Setup destination parameters */
    vfilt->src_width = raw_frame->img.width;
    vfilt->src_height = raw_frame->img.height;
    vfilt->src_csp = raw_frame->img.csp;

    /* Resize filter graph */
    vfilt->resize_filter_graph = avfilter_graph_alloc();
    if( !vfilt->resize_filter_graph )
    {
        fprintf( stderr, "Could not allocate filter graph \n" );
        ret = -1;
        goto end;
    }

    vfilt->buffersrc_ctx = avfilter_graph_alloc_filter( vfilt->resize_filter_graph, avfilter_get_by_name( "buffer" ), "src" );
    if( !vfilt->buffersrc_ctx )
    {
        syslog( LOG_ERR, "Failed to create buffersrc\n" );
        ret = -1;
        goto end;
    }

    /* buffersrc flags */
    snprintf( tmp, sizeof(tmp), "%i", raw_frame->img.width );
    av_opt_set( vfilt->buffersrc_ctx, "width", tmp, AV_OPT_SEARCH_CHILDREN );

    snprintf( tmp, sizeof(tmp), "%i", raw_frame->img.height );
    av_opt_set( vfilt->buffersrc_ctx, "height", tmp, AV_OPT_SEARCH_CHILDREN );

    snprintf( tmp, sizeof(tmp), "%s", av_get_pix_fmt_name( raw_frame->img.csp ) );
    av_opt_set( vfilt->buffersrc_ctx, "pix_fmt", tmp, AV_OPT_SEARCH_CHILDREN );

    /* We don't care too much about this */
    snprintf( tmp, sizeof(tmp), "%i/%i", 1, 27000000 );
    av_opt_set( vfilt->buffersrc_ctx, "time_base", tmp, AV_OPT_SEARCH_CHILDREN );

    if( avfilter_init_str( vfilt->buffersrc_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init source \n" );
        ret = -1;
        goto end;
    }

    vfilt->resize_ctx = avfilter_graph_alloc_filter( vfilt->resize_filter_graph, avfilter_get_by_name( "scale" ), "scale" );
    if( !vfilt->resize_ctx )
    {
        syslog( LOG_ERR, "Failed to create scaler\n" );
        ret = -1;
        goto end;
    }

    /* swscale flags */
    interlaced = IS_INTERLACED( raw_frame->img.format );

    /* Pretend the video is progressive if it's just a horizontal resize */
    if( raw_frame->img.height == output_stream->avc_param.i_height )
        interlaced = 0;

    /* Use decent settings if not default */
    if( !output_stream->downscale )
    {
        const char *sws_flags = "sws_flags=lanczos,accurate_rnd,full_chroma_int,bitexact";
        av_opt_set( vfilt->resize_ctx, "sws_flags", sws_flags, AV_OPT_SEARCH_CHILDREN );
    }

    snprintf( tmp, sizeof(tmp), "%i", interlaced );
    av_opt_set( vfilt->resize_ctx, "interl", tmp, AV_OPT_SEARCH_CHILDREN );

    snprintf( tmp, sizeof(tmp), "%i", output_stream->avc_param.i_width );
    av_opt_set( vfilt->resize_ctx, "width", tmp, AV_OPT_SEARCH_CHILDREN );

    snprintf( tmp, sizeof(tmp), "%i", output_stream->avc_param.i_height );
    av_opt_set( vfilt->resize_ctx, "height", tmp, AV_OPT_SEARCH_CHILDREN );

    vfilt->format_ctx = avfilter_graph_alloc_filter( vfilt->resize_filter_graph, avfilter_get_by_name( "format" ), "format" );
    if( !vfilt->format_ctx )
    {
        syslog( LOG_ERR, "Failed to create format\n" );
        ret = -1;
        goto end;
    }

    if( IS_PROGRESSIVE( raw_frame->img.format ) )
        vfilt->dst_csp = h->filter_bit_depth == OBE_BIT_DEPTH_8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
    else
        vfilt->dst_csp = h->filter_bit_depth == OBE_BIT_DEPTH_8 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV422P10;

    av_opt_set( vfilt->format_ctx, "pix_fmts", av_get_pix_fmt_name( vfilt->dst_csp ), AV_OPT_SEARCH_CHILDREN );

    if( avfilter_init_str( vfilt->format_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init format \n" );
        ret = -1;
        goto end;
    }

    vfilt->buffersink_ctx = avfilter_graph_alloc_filter( vfilt->resize_filter_graph, avfilter_get_by_name( "buffersink" ), "sink" );
    if( !vfilt->buffersink_ctx )
    {
        syslog( LOG_ERR, "Failed to create buffersink\n" );
        ret = -1;
        goto end;
    }

    if( avfilter_init_str( vfilt->buffersink_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init buffersink \n" );
        ret = -1;
        goto end;
    }

    ret = avfilter_link( vfilt->buffersrc_ctx, 0, vfilt->resize_ctx, 0 );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to link filter chain\n" );
        goto end;
    }

    ret = avfilter_link( vfilt->resize_ctx, 0, vfilt->format_ctx, 0 );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to link filter chain\n" );
        goto end;
    }

    ret = avfilter_link( vfilt->format_ctx, 0, vfilt->buffersink_ctx, 0 );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to link filter chain\n" );
        goto end;
    }

    /* Configure the graph. */
    ret = avfilter_graph_config( vfilt->resize_filter_graph, NULL );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to configure filter chain\n" );
        goto end;
    }

end:

    return ret;
}

static int csp_num_interleaved( int csp, int plane )
{
    return ( csp == AV_PIX_FMT_NV12 && plane == 1 ) ? 2 : 1;
}

static void blank_line( uint16_t *y, uint16_t *u, uint16_t *v, int width )
{
    for( int i = 0; i < width; i++ )
        y[i] = 0x40;

    for( int i = 0; i < width/2; i++ )
        u[i] = 0x200;

    for( int i = 0; i < width/2; i++ )
        v[i] = 0x200;
}

static void blank_lines( obe_raw_frame_t *raw_frame )
{
    /* All SDI input is 10-bit 4:2:2 */
    /* FIXME: assumes planar, non-interleaved format */
    uint16_t *y, *u, *v;

    y = (uint16_t*)raw_frame->img.plane[0];
    u = (uint16_t*)raw_frame->img.plane[1];
    v = (uint16_t*)raw_frame->img.plane[2];

    blank_line( y, u, v, raw_frame->img.width / 2 );
}

static int scale_frame( obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;
    uint8_t *src;
    uint16_t *dst;

    tmp_image.csp    = AV_PIX_FMT_YUV422P10;
    tmp_image.width  = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( raw_frame->img.csp );
    tmp_image.format = raw_frame->img.format;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height+1,
                        tmp_image.csp, 32 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for( int i = 0; i < tmp_image.planes; i++ )
    {
        int num_interleaved = csp_num_interleaved( img->csp, i );
        int height = obe_cli_csps[out->csp].height[i] * img->height;
        int width = obe_cli_csps[out->csp].width[i] * img->width / num_interleaved;

        src = img->plane[i];
        dst = (uint16_t*)tmp_image.plane[i];

        for( int j = 0; j < height; j++ )
        {
            for( int k = 0; k < width; k++ )
                dst[k] = src[k] << 2;

            src += img->stride[i];
            dst += out->stride[i] / 2;
        }

    }

    raw_frame->release_data( raw_frame );
    raw_frame->release_data = obe_release_video_data;
    memcpy( &raw_frame->alloc_img, out, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );
}

static int resize_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    AVFrame *frame = vfilt->frame;
    /* Setup AVFrame */
    memcpy( frame->buf, raw_frame->buf_ref, sizeof(frame->buf) );
    memcpy( frame->linesize, raw_frame->img.stride, sizeof(raw_frame->img.stride) );
    memcpy( frame->data, raw_frame->img.plane, sizeof(raw_frame->img.plane) );
    frame->format = raw_frame->img.csp;
    frame->width = raw_frame->img.width;
    frame->height = raw_frame->img.height;
    
    if( av_buffersrc_add_frame( vfilt->buffersrc_ctx, vfilt->frame ) < 0 )
    {
        fprintf( stderr, "Could not write frame to buffer source \n" );
        return -1;
    }

    int ret;

    while( 1 )
    {
        ret = av_buffersink_get_frame( vfilt->buffersink_ctx, vfilt->frame );

        if( ret == AVERROR(EAGAIN) || ret == AVERROR_EOF )
            continue; // shouldn't happen
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

    raw_frame->alloc_img.width = frame->width;
    raw_frame->alloc_img.height = frame->height;

    raw_frame->release_data = obe_release_bufref;

    memcpy( raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride) );
    memcpy( raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane) );
    raw_frame->alloc_img.csp = frame->format;
    raw_frame->alloc_img.planes = av_pix_fmt_count_planes( raw_frame->alloc_img.csp );

    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );

    memcpy( raw_frame->buf_ref, frame->buf, sizeof(frame->buf) );

    raw_frame->sar_width = raw_frame->sar_height = 1;

    return 0;
}



#if 0

/* The dithering algorithm is based on Sierra-2-4A error diffusion. It has been
 * written in such a way so that if the source has been upconverted using the
 * same algorithm as used in scale_image, dithering down to the source bit
 * depth again is lossless. */
#define DITHER_PLANE( pitch ) \
static void dither_plane_##pitch( pixel *dst, int dst_stride, uint16_t *src, int src_stride, \
                                        int width, int height, int16_t *errors ) \
{ \
    const int lshift = 16-X264_BIT_DEPTH; \
    const int rshift = 2*X264_BIT_DEPTH-16; \
    const int pixel_max = (1 << X264_BIT_DEPTH)-1; \
    const int half = 1 << (16-X264_BIT_DEPTH); \
    memset( errors, 0, (width+1) * sizeof(int16_t) ); \
    for( int y = 0; y < height; y++, src += src_stride, dst += dst_stride ) \
    { \
        int err = 0; \
        for( int x = 0; x < width; x++ ) \
        { \
            err = err*2 + errors[x] + errors[x+1]; \
            dst[x*pitch] = obe_clip3( (((src[x*pitch]+half)<<2)+err)*pixel_max >> 18, 0, pixel_max ); \
            errors[x] = err = src[x*pitch] - (dst[x*pitch] << lshift) - (dst[x*pitch] >> rshift); \
        } \
    } \
}

DITHER_PLANE( 1 )
DITHER_PLANE( 2 )

static int dither_image( obe_raw_frame_t *raw_frame, int16_t *error_buf )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;

    tmp_image.csp = X264_BIT_DEPTH == 10 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height,
                        tmp_image.csp, 32 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for( int i = 0; i < img->planes; i++ )
    {
        int num_interleaved = csp_num_interleaved( img->csp, i );
        int height = obe_cli_csps[img->csp].height[i] * img->height;
        int width = obe_cli_csps[img->csp].width[i] * img->width / num_interleaved;

#define CALL_DITHER_PLANE( pitch, off ) \
        dither_plane_##pitch( ((pixel*)out->plane[i])+off, out->stride[i]/sizeof(pixel), \
                ((uint16_t*)img->plane[i])+off, img->stride[i]/2, width, height, error_buf )

        if( num_interleaved == 1 )
        {
            CALL_DITHER_PLANE( 1, 0 );
        }
        else
        {
            CALL_DITHER_PLANE( 2, 0 );
            CALL_DITHER_PLANE( 2, 1 );
        }
    }

    raw_frame->release_data( raw_frame );
    raw_frame->release_data = obe_release_video_data;
    memcpy( &raw_frame->alloc_img, out, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );

    return 0;
}

#endif

static int downconvert_image_interlaced( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;
    const AVPixFmtDescriptor *pfd = av_pix_fmt_desc_get( raw_frame->img.csp );
    const AVComponentDescriptor *c = &pfd->comp[0];
    int bpp = c->depth > 8 ? 2 : 1;

    tmp_image.csp    = bpp == 2 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
    tmp_image.width  = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( raw_frame->img.csp );
    tmp_image.format = raw_frame->img.format;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height+1,
                        tmp_image.csp, 32 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    av_image_copy_plane( (uint8_t*)tmp_image.plane[0], tmp_image.stride[0],
                         (const uint8_t *)raw_frame->img.plane[0], raw_frame->img.stride[0],
                          raw_frame->img.width * bpp, raw_frame->img.height );

    for( int i = 1; i < tmp_image.planes; i++ )
    {
        int num_interleaved = csp_num_interleaved( img->csp, i );
        int height = obe_cli_csps[out->csp].height[i] * img->height;
        int width = obe_cli_csps[out->csp].width[i] * img->width / num_interleaved;

        if( bpp == 1 )
            vfilt->downsample_chroma_fields_8( img->plane[i], img->stride[i], out->plane[i], out->stride[i], width, height );
        else
            vfilt->downsample_chroma_fields_10( img->plane[i], img->stride[i], out->plane[i], out->stride[i], width, height );
    }

    raw_frame->release_data( raw_frame );
    raw_frame->release_data = obe_release_video_data;
    memcpy( &raw_frame->alloc_img, out, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );

    return 0;
}

static int dither_image( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;

    tmp_image.csp = img->csp == AV_PIX_FMT_YUV422P10 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( tmp_image.csp );
    tmp_image.format = raw_frame->img.format;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height,
                        tmp_image.csp, 32 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for( int i = 0; i < img->planes; i++ )
    {
        int num_interleaved = csp_num_interleaved( img->csp, i );
        int height = obe_cli_csps[img->csp].height[i] * img->height;
        int width = obe_cli_csps[img->csp].width[i] * img->width / num_interleaved;
        uint16_t *src = (uint16_t*)img->plane[i];
        uint8_t *dst = out->plane[i];

        vfilt->dither_plane_10_to_8( src, img->stride[i], dst, out->stride[i], width, height );
    }

    raw_frame->release_data( raw_frame );
    raw_frame->release_data = obe_release_video_data;
    memcpy( &raw_frame->alloc_img, &tmp_image, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );

    return 0;
}

/** User-data encapsulation **/
static int write_afd( obe_user_data_t *user_data, obe_raw_frame_t *raw_frame )
{
    bs_t r;
    uint8_t temp[100];
    const int country_code      = 0xb5;
    const int provider_code     = 0x31;
    const char *user_identifier = "DTG1";
    const int active_format_flag = 1;
    const int is_wide = (user_data->data[0] >> 2) & 1;

    /* TODO: when MPEG-2 is added make this do the right thing */

    bs_init( &r, temp, 100 );

    bs_write( &r,  8, country_code );  // itu_t_t35_country_code
    bs_write( &r, 16, provider_code ); // itu_t_t35_provider_code

    for( int i = 0; i < 4; i++ )
        bs_write( &r, 8, user_identifier[i] ); // user_identifier

    // afd_data()
    bs_write1( &r, 0 );   // '0'
    bs_write1( &r, active_format_flag ); // active_format_flag
    bs_write( &r, 6, 1 ); // reserved

    /* FIXME: is there any reason active_format_flag would be zero? */
    if( active_format_flag )
    {
        bs_write( &r, 4, 0xf ); // reserved
        bs_write( &r, 4, (user_data->data[0] >> 3) & 0xf ); // active_format
    }

    bs_flush( &r );

    /* Set the SAR from the AFD value */
    if( active_format_flag && IS_SD( raw_frame->img.format ) )
        set_sar( raw_frame, is_wide ); // TODO check return

    user_data->type = USER_DATA_AVC_REGISTERED_ITU_T35;
    user_data->len = bs_pos( &r ) >> 3;

    free( user_data->data );

    user_data->data = malloc( user_data->len );
    if( !user_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    memcpy( user_data->data, temp, user_data->len );

    return 0;
}

static int write_bar_data( obe_user_data_t *user_data )
{
    bs_t r;
    uint8_t temp[100];
    const int country_code      = 0xb5;
    const int provider_code     = 0x31;
    const char *user_identifier = "GA94";
    const int user_data_type_code = 0x06;
    int top, bottom, left, right;
    uint8_t *pos;

    /* TODO: when MPEG-2 is added make this do the right thing */

    bs_init( &r, temp, 100 );

    bs_write( &r,  8, country_code );  // itu_t_t35_country_code
    bs_write( &r, 16, provider_code ); // itu_t_t35_provider_code

    for( int i = 0; i < 4; i++ )
        bs_write( &r, 8, user_identifier[i] ); // user_identifier

    bs_write( &r, 8, user_data_type_code ); // user_data_type_code

    top    =  user_data->data[0] >> 7;
    bottom = (user_data->data[0] >> 6) & 1;
    left   = (user_data->data[0] >> 5) & 1;
    right  = (user_data->data[0] >> 4) & 1;

    bs_write1( &r, top );    // top_bar_flag
    bs_write1( &r, bottom ); // bottom_bar_flag
    bs_write1( &r, left );   // left_bar_flag
    bs_write1( &r, right );  // right_bar_flag
    bs_write( &r, 4, 0xf );  // reserved

    pos = &user_data->data[1];

#define WRITE_ELEMENT(x) \
    if( (x) )\
    {\
        bs_write( &r, 8, pos[0] );\
        bs_write( &r, 8, pos[1] );\
        pos += 2;\
    }\

    WRITE_ELEMENT( top )
    WRITE_ELEMENT( bottom )
    WRITE_ELEMENT( left )
    WRITE_ELEMENT( right )

    bs_flush( &r );

    user_data->type = USER_DATA_AVC_REGISTERED_ITU_T35;
    user_data->len = bs_pos( &r ) >> 3;

    free( user_data->data );

    user_data->data = malloc( user_data->len );
    if( !user_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    memcpy( user_data->data, temp, user_data->len );

    return 0;
}

static int convert_wss_to_afd( obe_user_data_t *user_data, obe_raw_frame_t *raw_frame )
{
    user_data->data[0] = (wss_to_afd[user_data->data[0]].afd_code << 3) | (wss_to_afd[user_data->data[0]].is_wide << 2);

    return write_afd( user_data, raw_frame );
}

static int encapsulate_user_data( obe_raw_frame_t *raw_frame, obe_int_input_stream_t *input_stream )
{
    int ret = 0;

    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        if( raw_frame->user_data[i].type == USER_DATA_CEA_608 )
            ret = write_608_cc( &raw_frame->user_data[i], input_stream );
        else if( raw_frame->user_data[i].type == USER_DATA_CEA_708_CDP )
            ret = read_cdp( &raw_frame->user_data[i] );
        else if( raw_frame->user_data[i].type == USER_DATA_AFD )
            ret = write_afd( &raw_frame->user_data[i], raw_frame );
        else if( raw_frame->user_data[i].type == USER_DATA_BAR_DATA )
            ret = write_bar_data( &raw_frame->user_data[i] );
        else if( raw_frame->user_data[i].type == USER_DATA_WSS )
            ret = convert_wss_to_afd( &raw_frame->user_data[i], raw_frame );

        /* FIXME: use standard return codes */
        if( ret < 0 )
            break;

        if( ret == 1 )
        {
            free( raw_frame->user_data[i].data );
            memmove( &raw_frame->user_data[i], &raw_frame->user_data[i+1],
                     sizeof(raw_frame->user_data) * (raw_frame->num_user_data-i-1) );
            raw_frame->num_user_data--;
            i--;
        }
    }

    if( !raw_frame->num_user_data )
    {
        free( raw_frame->user_data );
        raw_frame->user_data = NULL;
    }

    return ret;
}

static void *start_filter( void *ptr )
{
    obe_vid_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_int_input_stream_t *input_stream = filter_params->input_stream;
    obe_raw_frame_t *raw_frame;
    obe_output_stream_t *output_stream = get_output_stream( h, 0 ); /* FIXME when output_stream_id for video is not zero */
    int h_shift, v_shift;

    obe_vid_filter_ctx_t *vfilt = calloc( 1, sizeof(*vfilt) );
    if( !vfilt )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    init_filter( h, vfilt );

    vfilt->dst_width = output_stream->avc_param.i_width;
    vfilt->dst_height = output_stream->avc_param.i_height;

    while( 1 )
    {
        /* TODO: support resolution changes */
        /* TODO: support changes in pixel format */

        pthread_mutex_lock( &filter->queue.mutex );

        while( ulist_empty( &filter->queue.ulist ) && !filter->cancel_thread )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            goto end;
        }

        raw_frame = obe_raw_frame_t_from_uchain( ulist_pop( &filter->queue.ulist ) );
        pthread_mutex_unlock( &filter->queue.mutex );

        if( 1 )
        {
            const AVPixFmtDescriptor *pfd = av_pix_fmt_desc_get( raw_frame->img.csp );
            const AVComponentDescriptor *c = &pfd->comp[0];

            if( raw_frame->img.csp == AV_PIX_FMT_YUV422P && X264_BIT_DEPTH == 10 )
                scale_frame( raw_frame );

            if( raw_frame->img.format == INPUT_VIDEO_FORMAT_PAL && c->depth == 10 )
                blank_lines( raw_frame );

            /* Resize if wrong pixel format or wrong resolution */
            if( !( raw_frame->img.csp == AV_PIX_FMT_YUV422P   || raw_frame->img.csp == AV_PIX_FMT_YUV422P10 )
                || vfilt->dst_width   != raw_frame->img.width || vfilt->dst_height != raw_frame->img.height )
            {
                /* Reset the filter if it has been setup incorrectly or not setup at all */
                if( vfilt->src_csp    != raw_frame->img.csp || vfilt->src_width != raw_frame->img.width ||
                    vfilt->src_height != raw_frame->img.height )
                {
                    init_libavfilter( h, vfilt, output_stream, raw_frame );
                }

                resize_frame( vfilt, raw_frame );
            }

            if( av_pix_fmt_get_chroma_sub_sample( raw_frame->img.csp, &h_shift, &v_shift ) < 0 )
                goto end;

            /* Downconvert using interlaced scaling if input is 4:2:2 and target is 4:2:0 */
            if( h_shift == 1 && v_shift == 0 && filter_params->target_csp == X264_CSP_I420 )
            {
                if( downconvert_image_interlaced( vfilt, raw_frame ) < 0 )
                    goto end;
            }

            pfd = av_pix_fmt_desc_get( raw_frame->img.csp );
            c = &pfd->comp[0];
            if( c->depth == 10 && X264_BIT_DEPTH == 8 )
            {
                if( dither_image( vfilt, raw_frame ) < 0 )
                    goto end;
            }
        }

        if( encapsulate_user_data( raw_frame, input_stream ) < 0 )
            goto end;

        /* If SAR, on an SD stream, has not been updated by AFD or WSS, set to default 4:3
         * TODO: make this user-choosable. OBE will prioritise any SAR information from AFD or WSS over any user settings */
        if( raw_frame->sar_width == 1 && raw_frame->sar_height == 1 )
        {
            set_sar( raw_frame, IS_SD( raw_frame->img.format ) ? output_stream->is_wide : 1 );
            raw_frame->sar_guess = 1;
        }

        add_to_encode_queue( h, raw_frame, 0 );
    }

end:
    if( vfilt )
    {
        if( vfilt->resize_filter_graph )
            avfilter_graph_free( &vfilt->resize_filter_graph );

        free( vfilt );
    }

    free( filter_params );

    return NULL;
}

const obe_vid_filter_func_t video_filter = { start_filter };
