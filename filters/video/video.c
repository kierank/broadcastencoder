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
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libavcodec/avcodec.h>

#include "common/common.h"
#include "common/bitstream.h"
#include "video.h"
#include "cc.h"
#include "dither.h"
#include "x86/vfilter.h"
#include "input/sdi/sdi.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <bitstream/scte/104.h>
#include <input/sdi/ancillary.h>

#include <jpeglib.h>
#include <setjmp.h>

#include <libswscale/swscale.h>

#include <upipe/ubuf.h>
#include <upipe/ubuf_pic.h>
#include <upipe/ubuf_pic_mem.h>
#include <upipe/uref.h>
#include <upipe/uref_std.h>
#include <upipe/uref_pic.h>
#include <upipe/umem.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>

#define PREVIEW_SECONDS 5

typedef struct
{
    int filter_bit_depth;

    /* cpu flags */
    uint32_t avutil_cpu;

    /* upipe */
    struct uref_mgr *uref_mgr;

#define UBUF_MGR_YUV420P 0
#define UBUF_MGR_YUV422P 1
#define UBUF_MGR_YUV420P10 2
#define UBUF_MGR_YUV422P10 3

    struct ubuf_mgr *ubuf_mgr[4];

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
    AVFilterContext *flip_ctx;
    AVFilterContext *buffersink_ctx;
    AVFrame *frame;

    /* downsample */
    void (*downsample_chroma_fields_8)( void *src_ptr, ptrdiff_t src_stride, void *dst_ptr, ptrdiff_t dst_stride, uintptr_t width, uintptr_t height );
    void (*downsample_chroma_fields_10)( void *src_ptr, ptrdiff_t src_stride, void *dst_ptr, ptrdiff_t dst_stride, uintptr_t width, uintptr_t height );

    /* dither */
    void (*dither_plane_10_to_8)( uint16_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, uintptr_t width, uintptr_t height );
    int16_t *error_buf;

    int flip_ready;

    /* SCTE TCP */
    int64_t duration;
    int sockfd;
    int connfd;
    uint8_t msg_number;

    /* JPEG encoding */
    int encode_period;
    uint64_t frame_counter;
    int divisor;
    char jpeg_dst[30];
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPIMAGE row_pointers;
    jmp_buf setjmp_buffer;

    struct SwsContext *sws_context;
    int interlaced;

    uint8_t *jpeg_src[3];
    int jpeg_src_stride[3];
    long unsigned int jpeg_output_buf_size;
    uint8_t *jpeg_output_buf;
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

static void dither_plane_10_to_8_c( uint16_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, uintptr_t width, uintptr_t height )
{
    const int scale = 511;
    const uint16_t shift = 11;

    for( uintptr_t j = 0; j < height; j++ )
    {
        const uint16_t *dither = obe_dithers[j&7];
        uintptr_t k;
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
static void downsample_chroma_fields_8_c( void *src_ptr, ptrdiff_t src_stride, void *dst_ptr, ptrdiff_t dst_stride, uintptr_t width, uintptr_t height )
{
    uint8_t *src = src_ptr;
    uint8_t *dst = dst_ptr;
    for( uintptr_t i = 0; i < height; i += 2 )
    {
        uint8_t *srcf = src + src_stride*2;

        /* Top field */
        for( uintptr_t j = 0; j < width; j++ )
            dst[j] = (3*src[j] + srcf[j] + 2) >> 2;

        dst  += dst_stride;
        src  += src_stride;
        srcf += src_stride;

        /* Bottom field */
        for( uintptr_t j = 0; j < width; j++ )
            dst[j] = (src[j] + 3*srcf[j] + 2) >> 2;

        dst += dst_stride;
        src = srcf + src_stride;
    }
}

static void downsample_chroma_fields_10_c( void *src_ptr, ptrdiff_t src_stride, void *dst_ptr, ptrdiff_t dst_stride, uintptr_t width, uintptr_t height )
{
    uint16_t *src = src_ptr;
    uint16_t *dst = dst_ptr;
    for( uintptr_t i = 0; i < height; i += 2 )
    {
        uint16_t *srcf = src + src_stride;

        /* Top field */
        for( uintptr_t j = 0; j < width; j++ )
            dst[j] = (3*src[j] + srcf[j] + 2) >> 2;

        dst  += dst_stride/2;
        src  += src_stride/2;
        srcf += src_stride/2;

        /* Bottom field */
        for( uintptr_t j = 0; j < width; j++ )
            dst[j] = (src[j] + 3*srcf[j] + 2) >> 2;

        dst += dst_stride/2;
        src = srcf + src_stride/2;
    }
}

static void init_filter( obe_t *h, obe_vid_filter_ctx_t *vfilt )
{
    struct ubuf_mgr *mgr;

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
        vfilt->dither_plane_10_to_8 = obe_dither_plane_10_to_8_sse2;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_SSSE3 )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_ssse3;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_avx;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_avx;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX2 )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_avx2;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_avx2;
        vfilt->dither_plane_10_to_8 = obe_dither_plane_10_to_8_avx2;
    }

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX512ICL )
    {
        vfilt->downsample_chroma_fields_8 = obe_downsample_chroma_fields_8_avx512icl;
        vfilt->downsample_chroma_fields_10 = obe_downsample_chroma_fields_10_avx512icl;
        vfilt->dither_plane_10_to_8 = obe_dither_plane_10_to_8_avx512icl;
    }

#define UDICT_POOL_DEPTH 300
#define UREF_POOL_DEPTH 300
#define UBUF_POOL_DEPTH 300

    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH, h->umem_mgr, -1, -1);
    vfilt->uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    udict_mgr_release(udict_mgr);

    /* yuv420p */
    mgr = ubuf_pic_mem_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH, h->umem_mgr, 1, 0, 0, 0, 0, 64, 0);
    assert(mgr != NULL);
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "y8", 1, 1, 1));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "u8", 2, 2, 1));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "v8", 2, 2, 1));

    vfilt->ubuf_mgr[UBUF_MGR_YUV420P] = mgr;

    /* yuv422p */
    mgr = ubuf_pic_mem_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH, h->umem_mgr, 1, 0, 0, 0, 0, 64, 0);
    assert(mgr != NULL);
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "y8", 1, 1, 1));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "u8", 2, 1, 1));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "v8", 2, 1, 1));

    vfilt->ubuf_mgr[UBUF_MGR_YUV422P] = mgr;

    /* yuv420p10 */
    mgr = ubuf_pic_mem_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH, h->umem_mgr, 1, 0, 0, 0, 0, 64, 0);
    assert(mgr != NULL);
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "y10l", 1, 1, 2));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "u10l", 2, 2, 2));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "v10l", 2, 2, 2));

    vfilt->ubuf_mgr[UBUF_MGR_YUV420P10] = mgr;

    /* yuv422p10 */
    mgr = ubuf_pic_mem_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH, h->umem_mgr, 1, 0, 0, 0, 0, 64, 0);
    assert(mgr != NULL);
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "y10l", 1, 1, 2));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "u10l", 2, 1, 2));
    ubase_assert(ubuf_pic_mem_mgr_add_plane(mgr, "v10l", 2, 1, 2));

    vfilt->ubuf_mgr[UBUF_MGR_YUV422P10] = mgr;
}

static int init_libavfilter( obe_t *h, obe_vid_filter_ctx_t *vfilt, obe_vid_filter_params_t *filter_params,
                             obe_output_stream_t *output_stream, obe_raw_frame_t *raw_frame )
{
    char tmp[1024];
    int ret = 0;
    int interlaced = 0;
    AVFilterContext *penultimate = NULL;

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
    {
        if( filter_params->target_csp == X264_CSP_I422 )
            vfilt->dst_csp = X264_BIT_DEPTH == 8 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV422P10;
        else
            vfilt->dst_csp = X264_BIT_DEPTH == 8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
    }
    else
        vfilt->dst_csp = X264_BIT_DEPTH == 8 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV422P10;

    av_opt_set( vfilt->format_ctx, "pix_fmts", av_get_pix_fmt_name( vfilt->dst_csp ), AV_OPT_SEARCH_CHILDREN );

    if( avfilter_init_str( vfilt->format_ctx, NULL ) < 0 )
    {
        fprintf( stderr, "Could not init format \n" );
        ret = -1;
        goto end;
    }

    if( output_stream->flip == VIDEO_FLIP_HORIZONTAL )
    {
        vfilt->flip_ctx = avfilter_graph_alloc_filter( vfilt->resize_filter_graph, avfilter_get_by_name( "hflip" ), "hflip" );
        if( !vfilt->flip_ctx )
        {
            syslog( LOG_ERR, "Failed to create flip\n" );
            ret = -1;
            goto end;
        }
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

    penultimate = vfilt->format_ctx;
    if( output_stream->flip == VIDEO_FLIP_HORIZONTAL )
    {
        ret = avfilter_link( penultimate, 0, vfilt->flip_ctx, 0 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }
        penultimate = vfilt->flip_ctx;
        vfilt->flip_ready = 1;
    }

    ret = avfilter_link( penultimate, 0, vfilt->buffersink_ctx, 0 );
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

/*
 * Exit error handler for libjpeg
 */
static void user_error_exit( j_common_ptr p_jpeg )
{
    obe_vid_filter_ctx_t *vfilt = (obe_vid_filter_ctx_t *)p_jpeg->err;

    fprintf( stderr, "libjpeg exit \n");
    longjmp( vfilt->setjmp_buffer, 1 );
}

/*
 * Emit message error handler for libjpeg
 */
static void user_error_message( j_common_ptr p_jpeg )
{
    fprintf( stderr, "Unknown libjpeg error \n" );
}

static int init_jpegenc( obe_t *h, obe_vid_filter_ctx_t *vfilt, obe_vid_filter_params_t *filter_params,
                         obe_int_input_stream_t *input_stream )
{
    int buf_size;

    snprintf( vfilt->jpeg_dst, sizeof(vfilt->jpeg_dst), "/tmp/obepreview%u.jpg", encoder_id );

    vfilt->encode_period  = input_stream->timebase_den > 60 ? input_stream->timebase_den / 1000 : input_stream->timebase_den;
    vfilt->encode_period *= PREVIEW_SECONDS;

    vfilt->divisor = vfilt->dst_width <= 720 ? 3 : vfilt->dst_width <= 1280 ? 5 : 8;

    vfilt->cinfo.err = jpeg_std_error( &vfilt->jerr );
    vfilt->jerr.error_exit = user_error_exit;
    vfilt->jerr.output_message = user_error_message;
    jpeg_create_compress( &vfilt->cinfo );

    vfilt->cinfo.image_width = ((vfilt->dst_width / vfilt->divisor) / 16) * 16;
    vfilt->cinfo.image_height = ((vfilt->dst_height / vfilt->divisor) / 16) * 16;

    buf_size = 0;
    vfilt->jpeg_src_stride[0] = ((vfilt->cinfo.image_width + 15) / 16) * 16;
    vfilt->jpeg_src_stride[1] = vfilt->jpeg_src_stride[2] = (((vfilt->cinfo.image_width / 2) + 15) / 16) * 16;

    buf_size += vfilt->jpeg_src_stride[0] * vfilt->cinfo.image_height;
    for( int i = 1; i < 3; i++ )
        buf_size += vfilt->jpeg_src_stride[i] * vfilt->cinfo.image_height / 2;

    vfilt->jpeg_src[0] = av_malloc( buf_size );
    if( !vfilt->jpeg_src )
    {
        fprintf( stderr, "Could not allocate jpeg src buffer \n" );
        return -1;
    }

    vfilt->jpeg_src[1] = vfilt->jpeg_src[0] + (vfilt->jpeg_src_stride[0] * vfilt->cinfo.image_height);
    vfilt->jpeg_src[2] = vfilt->jpeg_src[1] + ((vfilt->jpeg_src_stride[1] * vfilt->cinfo.image_height / 2));

    vfilt->cinfo.input_components = 3;
    vfilt->cinfo.in_color_space = JCS_YCbCr;
    vfilt->cinfo.jpeg_color_space = JCS_YCbCr;
    vfilt->cinfo.data_precision   = 8;

    jpeg_set_defaults( &vfilt->cinfo );

    vfilt->cinfo.raw_data_in = true;

    /* 4:2:0 */
    vfilt->cinfo.comp_info[0].h_samp_factor = 2;
    vfilt->cinfo.comp_info[0].v_samp_factor = 2;
    vfilt->cinfo.comp_info[1].h_samp_factor = 1;
    vfilt->cinfo.comp_info[1].v_samp_factor = 1;
    vfilt->cinfo.comp_info[2].h_samp_factor = 1;
    vfilt->cinfo.comp_info[2].v_samp_factor = 1;

    vfilt->row_pointers = malloc( 3 * sizeof(JSAMPARRAY) );
    if( !vfilt->row_pointers )
    {
        fprintf( stderr, "Could not allocate libjpeg pointer buffers \n" );
        return -1;
    }

    for( int i = 0; i < 3; i++ )
    {
        vfilt->row_pointers[i] = malloc(vfilt->cinfo.comp_info[i].v_samp_factor * sizeof(JSAMPROW) * DCTSIZE);
        if( !vfilt->row_pointers[i] )
        {
            fprintf( stderr, "Could not allocate libjpeg pointer buffers \n" );
            return -1;
        }
    }

    int dst_pix_fmt = filter_params->target_csp == X264_CSP_I422 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
    int dst_height = input_stream->interlaced ? vfilt->dst_height / 2 : vfilt->dst_height;
    vfilt->sws_context = sws_getContext( vfilt->dst_width, dst_height, dst_pix_fmt,
                                         vfilt->cinfo.image_width, vfilt->cinfo.image_height, AV_PIX_FMT_YUV420P,
                                         SWS_BICUBIC, NULL, NULL, NULL );


    jpeg_destroy_compress( &vfilt->cinfo );

    return 0;
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

static int scale_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;
    uint8_t *src;
    uint16_t *dst;
    struct uref *uref;

    tmp_image.csp    = AV_PIX_FMT_YUV422P10;
    tmp_image.width  = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( raw_frame->img.csp );
    tmp_image.format = raw_frame->img.format;

    const char *output_chroma_map[3+1];
    output_chroma_map[0] = "y10l";
    output_chroma_map[1] = "u10l";
    output_chroma_map[2] = "v10l";
    output_chroma_map[3] = NULL;

    uref = uref_pic_alloc( vfilt->uref_mgr, vfilt->ubuf_mgr[UBUF_MGR_YUV422P10], tmp_image.width, tmp_image.height );
    if( !uref )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for (int i = 0; i < 3 && output_chroma_map[i] != NULL; i++)
    {
        uint8_t *data;
        size_t stride;
        if (unlikely(!ubase_check(uref_pic_plane_write(uref, output_chroma_map[i], 0, 0, -1, -1, (uint8_t **)&data)) ||
                    !ubase_check(uref_pic_plane_size(uref, output_chroma_map[i], &stride, NULL, NULL, NULL)))) {
            syslog(LOG_ERR, "invalid buffer received");
            uref_free(uref);
            return -1;
        }

        tmp_image.plane[i] = (uint8_t *)data;
        tmp_image.stride[i] = stride;
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
    raw_frame->uref = uref;

    raw_frame->release_data = obe_release_video_uref;
    raw_frame->dup_frame = obe_dup_video_uref;
    memcpy( &raw_frame->alloc_img, out, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );

    return 0;
}

static int resize_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    AVFrame *frame = vfilt->frame;
    /* Setup AVFrame */
    memcpy( frame->buf, raw_frame->buf_ref, sizeof(frame->buf) );
    memset(raw_frame->buf_ref, 0, sizeof(raw_frame->buf_ref));
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

    raw_frame->release_data(raw_frame);

    raw_frame->alloc_img.width = frame->width;
    raw_frame->alloc_img.height = frame->height;

    raw_frame->release_data = obe_release_bufref;

    memcpy( raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride) );
    memcpy( raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane) );
    raw_frame->alloc_img.csp = frame->format;
    raw_frame->alloc_img.planes = av_pix_fmt_count_planes( raw_frame->alloc_img.csp );

    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );

    memcpy( raw_frame->buf_ref, frame->buf, sizeof(frame->buf) );
    memset( frame->buf, 0, sizeof(frame->buf) );

    raw_frame->sar_width = raw_frame->sar_height = 1;

    return 0;
}

static int encode_jpeg( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    int src_stride[4] = {0}, height;
    int interlaced = IS_INTERLACED( raw_frame->img.format );

    /* Duplicate init_jpegenc to avoid per frame malloc */
    vfilt->cinfo.err = jpeg_std_error( &vfilt->jerr );
    vfilt->jerr.error_exit = user_error_exit;
    vfilt->jerr.output_message = user_error_message;
    jpeg_create_compress( &vfilt->cinfo );

    vfilt->cinfo.image_width = ((vfilt->dst_width / vfilt->divisor) / 16) * 16;
    vfilt->cinfo.image_height = ((vfilt->dst_height / vfilt->divisor) / 16) * 16;

    vfilt->cinfo.input_components = 3;
    vfilt->cinfo.in_color_space = JCS_YCbCr;
    vfilt->cinfo.jpeg_color_space = JCS_YCbCr;
    vfilt->cinfo.data_precision   = 8;

    jpeg_set_defaults( &vfilt->cinfo );

    vfilt->cinfo.raw_data_in = true;

    /* 4:2:0 */
    vfilt->cinfo.comp_info[0].h_samp_factor = 2;
    vfilt->cinfo.comp_info[0].v_samp_factor = 2;
    vfilt->cinfo.comp_info[1].h_samp_factor = 1;
    vfilt->cinfo.comp_info[1].v_samp_factor = 1;
    vfilt->cinfo.comp_info[2].h_samp_factor = 1;
    vfilt->cinfo.comp_info[2].v_samp_factor = 1;

    jpeg_set_quality( &vfilt->cinfo, 80, true );

    if( setjmp( vfilt->setjmp_buffer ) )
    {
        goto error;
    }

    for( int i = 0; i < 3; i++ )
        src_stride[i] = interlaced ? raw_frame->img.stride[i] * 2 : raw_frame->img.stride[i];
    height = interlaced ? raw_frame->img.height / 2 : raw_frame->img.height;

    sws_scale( vfilt->sws_context, (const uint8_t * const*)raw_frame->img.plane, src_stride, 0, height, vfilt->jpeg_src, vfilt->jpeg_src_stride );

    jpeg_mem_dest( &vfilt->cinfo, &vfilt->jpeg_output_buf, &vfilt->jpeg_output_buf_size );
    jpeg_start_compress( &vfilt->cinfo, true );

    while( vfilt->cinfo.next_scanline < vfilt->cinfo.image_height )
    {
        for( int i = 0; i < 3; i++ )
        {
            int offset = vfilt->cinfo.next_scanline * vfilt->cinfo.comp_info[i].v_samp_factor / vfilt->cinfo.max_v_samp_factor;

            for( int j = 0; j < vfilt->cinfo.comp_info[i].v_samp_factor * DCTSIZE; j++ )
            {
                vfilt->row_pointers[i][j] = vfilt->jpeg_src[i] + vfilt->jpeg_src_stride[i] * (offset + j);
            }
        }

        jpeg_write_raw_data( &vfilt->cinfo, vfilt->row_pointers, vfilt->cinfo.max_v_samp_factor * DCTSIZE );
    }

    jpeg_finish_compress( &vfilt->cinfo );

    FILE *fp = fopen( vfilt->jpeg_dst, "wb" );
    fwrite( vfilt->jpeg_output_buf, 1, vfilt->jpeg_output_buf_size, fp );
    fclose( fp );

    jpeg_destroy_compress( &vfilt->cinfo );
    free( vfilt->jpeg_output_buf );
    vfilt->jpeg_output_buf = NULL;
    vfilt->jpeg_output_buf_size = 0;

error:

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
                        tmp_image.csp, 64 ) < 0 )
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
    struct uref *uref;

    tmp_image.csp    = bpp == 2 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
    tmp_image.width  = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( raw_frame->img.csp );
    tmp_image.format = raw_frame->img.format;

    int ubuf_mgr = tmp_image.csp == AV_PIX_FMT_YUV420P10 ? UBUF_MGR_YUV420P10 : UBUF_MGR_YUV420P;

    const char *output_chroma_map[3+1];

    if( tmp_image.csp == AV_PIX_FMT_YUV420P10 )
    {
        output_chroma_map[0] = "y10l";
        output_chroma_map[1] = "u10l";
        output_chroma_map[2] = "v10l";
        output_chroma_map[3] = NULL;
    }
    else
    {
        output_chroma_map[0] = "y8";
        output_chroma_map[1] = "u8";
        output_chroma_map[2] = "v8";
        output_chroma_map[3] = NULL;
    }

    uref = uref_pic_alloc( vfilt->uref_mgr, vfilt->ubuf_mgr[ubuf_mgr], tmp_image.width, tmp_image.height );
    if( !uref )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for (int i = 0; i < 3 && output_chroma_map[i] != NULL; i++)
    {
        uint8_t *data;
        size_t stride;
        if (unlikely(!ubase_check(uref_pic_plane_write(uref, output_chroma_map[i], 0, 0, -1, -1, (uint8_t **)&data)) ||
                    !ubase_check(uref_pic_plane_size(uref, output_chroma_map[i], &stride, NULL, NULL, NULL)))) {
            syslog(LOG_ERR, "invalid buffer received");
            uref_free(uref);
            return -1;
        }

        tmp_image.plane[i] = (uint8_t *)data;
        tmp_image.stride[i] = stride;
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
    raw_frame->uref = uref;

    raw_frame->release_data = obe_release_video_uref;
    raw_frame->dup_frame = obe_dup_video_uref;
    memcpy( &raw_frame->alloc_img, out, sizeof(obe_image_t) );
    memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t) );

    return 0;
}

static int dither_image( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;
    struct uref *uref;

    tmp_image.csp = img->csp == AV_PIX_FMT_YUV422P10 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_count_planes( tmp_image.csp );
    tmp_image.format = raw_frame->img.format;
    int ubuf_mgr = tmp_image.csp == AV_PIX_FMT_YUV422P ? UBUF_MGR_YUV422P : UBUF_MGR_YUV420P;

    const char *output_chroma_map[3+1];
    output_chroma_map[0] = "y8";
    output_chroma_map[1] = "u8";
    output_chroma_map[2] = "v8";
    output_chroma_map[3] = NULL;

    uref = uref_pic_alloc( vfilt->uref_mgr, vfilt->ubuf_mgr[ubuf_mgr], tmp_image.width, tmp_image.height );
    if( !uref )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for (int i = 0; i < 3 && output_chroma_map[i] != NULL; i++)
    {
        uint8_t *data;
        size_t stride;
        if (unlikely(!ubase_check(uref_pic_plane_write(uref, output_chroma_map[i], 0, 0, -1, -1, &data)) ||
                    !ubase_check(uref_pic_plane_size(uref, output_chroma_map[i], &stride, NULL, NULL, NULL)))) {
            syslog(LOG_ERR, "invalid buffer received");
            uref_free(uref);
            return -1;
        }

        tmp_image.plane[i] = (uint8_t *)data;
        tmp_image.stride[i] = stride;
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
    raw_frame->uref = uref;

    raw_frame->release_data = obe_release_video_uref;
    raw_frame->dup_frame = obe_dup_video_uref;
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

static int setup_scte104_socket( obe_vid_filter_ctx_t *vfilt, obe_output_stream_t *scte35_stream )
{
    struct sockaddr_in listen_addr;
    int flags;
    char *ip, *port;

    vfilt->sockfd = socket( AF_INET, SOCK_STREAM, 0 );
    if( vfilt->sockfd < 0 )
    {
        fprintf( stderr, "socket creation failed \n" );
        return -1;
    }

    flags = fcntl(vfilt->sockfd, F_GETFL, 0);
    fcntl(vfilt->sockfd, F_SETFL, flags | O_NONBLOCK);

    int enable = 1;
    if( setsockopt( vfilt->sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int) ) < 0 )
        fprintf( stderr, "setsockopt(SO_REUSEADDR) failed \n" );

    ip = strtok( scte35_stream->scte_tcp_address, ":" );
    port = strtok( NULL, ":" );

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = inet_addr(ip);
    listen_addr.sin_port = htons(atoi(port));

    if( bind( vfilt->sockfd, (struct sockaddr*)&listen_addr, sizeof(listen_addr) ) < 0 )
    {
        fprintf( stderr, "bind() failed \n" );
        return -1;
    }

    if( listen( vfilt->sockfd, 128 ) < 0 )
    {
        fprintf( stderr, "listen() failed \n" );
        return -1;
    }

    return 0;
}

static void send_scte104_reply(obe_vid_filter_ctx_t *vfilt, uint16_t type)
{
    uint8_t msg[500];

    int len = SCTE104M_HEADER_SIZE + SCTE104T_HEADER_SIZE + 1 + SCTE104O_HEADER_SIZE;

    scte104_set_opid(msg, SCTE104_OPID_MULTIPLE);
    scte104o_set_data_length(msg, len);
    scte104m_set_protocol(msg, 0);
    scte104m_set_as_index(msg, 0);
    scte104m_set_message_number(msg, vfilt->msg_number++);
    scte104m_set_dpi_pid_index(msg, 0);
    scte104m_set_scte35_protocol(msg, 0);

    uint8_t *ts = scte104m_get_timestamp(msg);
    scte104t_set_type(ts, SCTE104T_TYPE_NONE);

    scte104m_set_num_ops(msg, 1);

    uint8_t *op = ts + 2;
    scte104o_set_opid(op, type);
    scte104o_set_data_length(op, 0);

    if( send(vfilt->connfd, msg, len, 0) < 0)
        fprintf( stderr, "Failed to send scte 104 reply \n" );
}

static int handle_scte104_message( obe_t *h, obe_vid_filter_ctx_t *vfilt, int64_t pts )
{
    uint8_t scte104[500];
    obe_sdi_non_display_data_t non_display_data;

    int len = recv( vfilt->connfd, (void *)scte104, sizeof(scte104), 0 );

    if( len < 0 )
    {
        fprintf( stderr, "error receiving scte tcp message \n");
        return -1;
    }

    if( len == 0 )
    {
        close( vfilt->connfd );
        vfilt->connfd = 0;
    }

    if( len >= 14 )
    {
        /* Check it's a message we should decode */
        if( scte104m_validate( scte104, len ) && scte104_get_opid( scte104 ) == SCTE104_OPID_MULTIPLE )
        {
            int num_ops = scte104m_get_num_ops( scte104 );
            for( int i = 0; i < num_ops; i++ )
            {
                uint8_t *op = scte104m_get_op( scte104, i );
                if( scte104o_get_opid( op ) == SCTE104_OPID_INIT_REQUEST_DATA ||
                    scte104o_get_opid( op ) == SCTE104_OPID_ALIVE_REQUEST_DATA )
                {
                    send_scte104_reply( vfilt, scte104o_get_opid( op ) + 1 );
                }
                else if( scte104o_get_opid( op ) == SCTE104_OPID_SPLICE_NULL ||
                         scte104o_get_opid( op ) == SCTE104_OPID_SPLICE ||
                         scte104o_get_opid( op ) == SCTE104_OPID_TIME_SIGNAL ||
                         scte104o_get_opid( op ) == SCTE104_OPID_INSERT_SEGMENTATION_DESCRIPTOR )
                {
                    decode_scte104( &non_display_data, scte104, len );
                    if( non_display_data.scte35_frame )
                    {
                        if( send_scte35( h, &non_display_data, pts, vfilt->duration) < 0 )
                            fprintf( stderr, "couldn't send scte35 data \n");
                    }
                }
            }
        }
    }


    return 0;
}

static void *start_filter( void *ptr )
{
    obe_vid_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_int_input_stream_t *input_stream = filter_params->input_stream;
    obe_raw_frame_t *raw_frame, *raw_frame_dup = NULL;
    obe_output_stream_t *output_stream = get_output_stream( h, 0 ); /* FIXME when output_stream_id for video is not zero */
    obe_output_stream_t *scte35_stream = get_output_stream_by_format( h, MISC_SCTE35 );
    int h_shift, v_shift, scte_tcp = 0;
    int64_t pts;
    struct pollfd fds[2];

    obe_vid_filter_ctx_t *vfilt = calloc( 1, sizeof(*vfilt) );
    if( !vfilt )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    init_filter( h, vfilt );

    vfilt->dst_width = output_stream->avc_param.i_width;
    vfilt->dst_height = output_stream->avc_param.i_height;

    if( scte35_stream && strlen( scte35_stream->scte_tcp_address ) )
    {
        if( setup_scte104_socket( vfilt, scte35_stream ) < 0 )
        {
            fprintf( stderr, "Could not open socket\n" );
            goto end;
        }

        scte_tcp = 1;
    }

    vfilt->duration = av_rescale_q( 1, (AVRational){input_stream->timebase_num, input_stream->timebase_den}, (AVRational){1, OBE_CLOCK} );

    init_jpegenc( h, vfilt, filter_params, input_stream );

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
            break;
        }

        raw_frame = obe_raw_frame_t_from_uchain( ulist_pop( &filter->queue.ulist ) );
        pthread_mutex_unlock( &filter->queue.mutex );

        if( 1 )
        {
            const AVPixFmtDescriptor *pfd = av_pix_fmt_desc_get( raw_frame->img.csp );
            const AVComponentDescriptor *c = &pfd->comp[0];

            if( raw_frame->img.csp == AV_PIX_FMT_YUV422P && X264_BIT_DEPTH == 10 )
                scale_frame( vfilt, raw_frame );

            if( raw_frame->img.format == INPUT_VIDEO_FORMAT_PAL && c->depth == 10 )
                blank_lines( raw_frame );

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

            /* Resize if wrong resolution */
            if( vfilt->dst_width   != raw_frame->img.width || vfilt->dst_height != raw_frame->img.height || output_stream->flip )
            {
                /* Reset the filter if it has been setup incorrectly or not setup at all */
                if( vfilt->src_csp    != raw_frame->img.csp || vfilt->src_width != raw_frame->img.width ||
                    vfilt->src_height != raw_frame->img.height || ( output_stream->flip && !vfilt->flip_ready) )
                {
                    init_libavfilter( h, vfilt, filter_params, output_stream, raw_frame );
                }

                resize_frame( vfilt, raw_frame );
            }

            if( X264_BIT_DEPTH == 8 && !( vfilt->frame_counter % vfilt->encode_period ) )
            {
                raw_frame_dup = raw_frame->dup_frame( raw_frame );
                if( !raw_frame_dup )
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

        pts = raw_frame->pts;
        add_to_encode_queue( h, raw_frame, 0 );
        /* TODO: 10-bit jpeg */
        if( raw_frame_dup )
        {
            encode_jpeg( vfilt, raw_frame_dup );
            raw_frame_dup->release_data( raw_frame_dup );
            raw_frame_dup->release_frame( raw_frame_dup );
            raw_frame_dup = NULL;
        }

        vfilt->frame_counter++;

        if( scte_tcp )
        {
            int num_fds = 1;
            fds[0].fd = vfilt->sockfd;
            fds[0].events = POLLIN;

            if( vfilt->connfd > 0 )
            {
                fds[1].fd = vfilt->connfd;
                fds[1].events = POLLIN;
                num_fds = 2;
            }

            if( poll( fds, num_fds, 2 ) )
            {
                if( fds[0].revents & POLLIN )
                {
                    vfilt->connfd = accept( vfilt->sockfd, NULL, 0 );
                    if( vfilt->connfd < 0 && (vfilt->connfd != EAGAIN || vfilt->connfd != EWOULDBLOCK) )
                    {
                        fprintf( stderr, "accept() failed \n" );
                    }
                }

                if( fds[1].revents & POLLIN )
                {
                    handle_scte104_message( h, vfilt, pts );
                }
            }
        }
    }

end:
    if( vfilt )
    {
        for( int i = 0; i < 4; i++ )
            ubuf_mgr_release( vfilt->ubuf_mgr[i] );

        uref_mgr_release( vfilt->uref_mgr );

        if( vfilt->resize_filter_graph )
            avfilter_graph_free( &vfilt->resize_filter_graph );

        if( vfilt->frame )
            av_frame_free( &vfilt->frame );

        if ( vfilt->sws_context )
            sws_freeContext( vfilt->sws_context );

        jpeg_destroy_compress( &vfilt->cinfo );

        for( int i = 0; i < 3; i++ )
            free( vfilt->row_pointers[i] );

        free( vfilt->row_pointers );

        if( vfilt->jpeg_src[0] )
            av_free( vfilt->jpeg_src[0] );

        if( vfilt->jpeg_output_buf )
            free( vfilt->jpeg_output_buf );

        if( vfilt->connfd )
            close( vfilt->connfd );

        if( vfilt->sockfd )
            close( vfilt->sockfd );

        free( vfilt );
    }

    free( filter_params );

    return NULL;
}

const obe_vid_filter_func_t video_filter = { start_filter };
