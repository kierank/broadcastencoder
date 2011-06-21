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
#include <libswscale/swscale.h>
#include "common/common.h"
#include "common/bitstream.h"
#include "video.h"
#include "cc.h"
#include "dither.h"
#include "x86/vfilter.h"
#include "input/sdi/sdi.h"


#if X264_BIT_DEPTH > 8
typedef uint16_t pixel;
#else
typedef uint8_t pixel;
#endif

#define PAL_FIRST_NON_BLANKED  24
#define NTSC_FIRST_NON_BLANKED 22

typedef struct
{
    /* cpu flags */
    uint32_t avutil_cpu;

    /* upscaling */
    void (*scale_plane)( uint16_t *src, int stride, int width, int height, int lshift, int rshift );

    /* downscaling */
    struct SwsContext *sws_ctx;
    int sws_ctx_flags;
    enum PixelFormat dst_pix_fmt;

    /* dither */
    void (*dither_row_10_to_8)( uint16_t *src, uint8_t *dst, const uint16_t *dithers, int width, int stride );
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

static obe_cli_csp_t obe_cli_csps[] =
{
    [PIX_FMT_YUV420P] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 8 },
    [PIX_FMT_NV12] =    { 2, { 1,  1 },     { 1, .5 },     2, 2, 8 },
    [PIX_FMT_YUV420P10] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 10 },
    [PIX_FMT_YUV422P10] = { 3, { 1, .5, .5 }, { 1, 1, 1 }, 2, 2, 10 },
    [PIX_FMT_YUV420P16] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 16 },
};

const static int wss_to_afd[] =
{
    [0x0] = 0x9,
    [0x1] = 0xb,
    [0x2] = 0x3,
    [0x3] = 0xa,
    [0x4] = 0x2,
    [0x5] = 0x4,
    [0x6] = 0xd,
    [0x7] = 0x8,
};

static void scale_plane_c( uint16_t *src, int stride, int width, int height, int lshift, int rshift )
{
    for( int i = 0; i < height; i++ )
    {
        for( int j = 0; j < width; j++ )
            src[j] = (src[j] << lshift) + (src[j] >> rshift);

        src += stride / 2;
    }
}

static void dither_row_10_to_8_c( uint16_t *src, uint8_t *dst, const uint16_t *dither, int width, int stride )
{
    const int scale = 511;
    const uint16_t shift = 11;

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

}

static void init_filter( obe_vid_filter_ctx_t *vfilt )
{
    vfilt->avutil_cpu = av_get_cpu_flags();

#if 0
    vfilt->scale_plane = scale_plane_c;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_MMX2 )
        vfilt->scale_plane = obe_scale_plane_mmxext;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_SSE2 )
        vfilt->scale_plane = obe_scale_plane_sse2;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX )
        vfilt->scale_plane = obe_scale_plane_avx;
#endif

    vfilt->dither_row_10_to_8 = dither_row_10_to_8_c;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_SSE4 )
        vfilt->dither_row_10_to_8 = obe_dither_row_10_to_8_sse4;

    if( vfilt->avutil_cpu & AV_CPU_FLAG_AVX )
        vfilt->dither_row_10_to_8 = obe_dither_row_10_to_8_avx;

}

static void blank_lines( obe_raw_frame_t *raw_frame )
{
    /* All SDI input is 10-bit 4:2:2 */
    /* FIXME: assumes planar, non-interleaved format */
    uint16_t *y, *u, *v;
    int cur_line, first_nonblank_line;

    y = (uint16_t*)raw_frame->img.plane[0];
    u = (uint16_t*)raw_frame->img.plane[1];
    v = (uint16_t*)raw_frame->img.plane[2];

    cur_line = raw_frame->img.first_line;
    first_nonblank_line = raw_frame->img.format == INPUT_VIDEO_FORMAT_PAL ? PAL_FIRST_NON_BLANKED : NTSC_FIRST_NON_BLANKED;

    while( cur_line != first_nonblank_line )
    {
        for( int i = 0; i < raw_frame->img.width; i++ )
            y[i] = 0x40;

        for( int i = 0; i < raw_frame->img.width/2; i++ )
        {
            u[i] = 0x200;
            v[i] = 0x200;
        }

        cur_line = sdi_next_line( raw_frame->img.format, cur_line );
        y += raw_frame->img.stride[0] / 2;
        u += raw_frame->img.stride[1] / 2;
        v += raw_frame->img.stride[2] / 2;
    }
}

static int scale_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;

    /* TODO: when frames are needed for reference we can't scale in-place */

    /* this function mimics how swscale does upconversion. 8-bit is converted
     * to 16-bit through left shifting the orginal value with 8 and then adding
     * the original value to that. This effectively keeps the full color range
     * while also being fast. for n-bit we basically do the same thing, but we
     * discard the lower 16-n bits. */
    const int lshift = 16-obe_cli_csps[img->csp].bit_depth;
    const int rshift = 2*obe_cli_csps[img->csp].bit_depth - 16;
    for( int i = 0; i < img->planes; i++ )
    {
        uint16_t *src = (uint16_t*)img->plane[i];
        int height = obe_cli_csps[img->csp].height[i] * img->height;
        int width = obe_cli_csps[img->csp].width[i] * img->width;

        vfilt->scale_plane( src, img->stride[i], width, height, lshift, rshift );
    }

    img->csp = img->csp == PIX_FMT_YUV420P ? PIX_FMT_YUV420P10 : PIX_FMT_YUV422P10;

    return 0;
}

static int downconvert_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t tmp_image = {0};

    if( !vfilt->sws_ctx )
    {
        vfilt->sws_ctx_flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_LANCZOS;
        vfilt->dst_pix_fmt = raw_frame->img.csp == PIX_FMT_YUV422P10 ? PIX_FMT_YUV420P10 : PIX_FMT_YUV420P;

        vfilt->sws_ctx = sws_getContext( raw_frame->img.width, raw_frame->img.height, raw_frame->img.csp,
                                         raw_frame->img.width, raw_frame->img.height, vfilt->dst_pix_fmt,
                                         vfilt->sws_ctx_flags, NULL, NULL, NULL );
        if( !vfilt->sws_ctx )
        {
            fprintf( stderr, "Video scaling failed\n" );
            return -1;
        }
    }

    tmp_image.csp = vfilt->dst_pix_fmt;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;
    tmp_image.planes = av_pix_fmt_descriptors[vfilt->dst_pix_fmt].nb_components;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height+1,
                        vfilt->dst_pix_fmt, 16 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    sws_scale( vfilt->sws_ctx, (const uint8_t* const*)raw_frame->img.plane, raw_frame->img.stride,
               0, tmp_image.height, tmp_image.plane, tmp_image.stride );

    raw_frame->release_data( raw_frame );
    memcpy( &raw_frame->img, &tmp_image, sizeof(obe_image_t) );

    return 0;
}

static int csp_num_interleaved( int csp, int plane )
{
    return ( csp == PIX_FMT_NV12 && plane == 1 ) ? 2 : 1;
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

    tmp_image.csp = X264_BIT_DEPTH == 10 ? PIX_FMT_YUV420P10 : PIX_FMT_YUV420P;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height,
                        tmp_image.csp, 16 ) < 0 )
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
    memcpy( &raw_frame->img, out, sizeof(obe_image_t) );

    return 0;
}

#endif

static int dither_image( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
    obe_image_t tmp_image = {0};
    obe_image_t *out = &tmp_image;

    tmp_image.csp = PIX_FMT_YUV420P;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height+1,
                        tmp_image.csp, 16 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    for( int i = 0; i < img->planes; i++ )
    {
        //const int src_depth = av_pix_fmt_descriptors[img->csp].comp[i].depth_minus1+1;
        //const int dst_depth = av_pix_fmt_descriptors[out->csp].comp[i].depth_minus1+1;

        //uint16_t scale = obe_dither_scale[dst_depth-1][src_depth-1];
        //int shift = src_depth-dst_depth + obe_dither_scale[src_depth-2][dst_depth-1];

        int num_interleaved = csp_num_interleaved( img->csp, i );
        int height = obe_cli_csps[img->csp].height[i] * img->height;
        int width = obe_cli_csps[img->csp].width[i] * img->width / num_interleaved;
        uint16_t *src = (uint16_t*)img->plane[i];
        uint8_t *dst = out->plane[i];

        for( int j = 0; j < height; j++ )
        {
            const uint16_t *dither = obe_dithers[j&7];

            vfilt->dither_row_10_to_8( src, dst, dither, width, img->stride[i] );

            src += img->stride[i] / 2;
            dst += out->stride[i];
        }
    }

    raw_frame->release_data( raw_frame );
    memcpy( &raw_frame->img, out, sizeof(obe_image_t) );

    return 0;
}

/** User-data encapsulation **/
static int write_afd( obe_user_data_t *user_data )
{
    bs_t r;
    uint8_t temp[100];
    const int country_code      = 0xb5;
    const int provider_code     = 0x31;
    const char *user_identifier = "DTG1";
    int active_format_flag;

    /* TODO: when MPEG-2 is added make this do the right thing */

    bs_init( &r, temp, 100 );

    bs_write( &r,  8, country_code );  // itu_t_t35_country_code
    bs_write( &r, 16, provider_code ); // itu_t_t35_provider_code

    for( int i = 0; i < 4; i++ )
        bs_write( &r, 8, user_identifier[i] ); // user_identifier

    active_format_flag = (user_data->data[0] >> 2) & 1;
    // afd_data()
    bs_write1( &r, 0 );   // '0'
    bs_write1( &r, active_format_flag ); // active_format_flag
    bs_write( &r, 6, 1 ); // reserved

    if( active_format_flag )
    {
        bs_write( &r, 4, 0xf ); // reserved
        bs_write( &r, 4, ( user_data->data[0] >> 3) & 0xf ); // active_format
    }

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

static int convert_wss_to_afd( obe_user_data_t *user_data )
{
    user_data->data[0] = (wss_to_afd[user_data->data[0]] << 3) | (1 << 2);
    if( write_afd( user_data ) < 0 )
        return -1;

    return 0;
}

static int encapsulate_user_data( obe_raw_frame_t *raw_frame, obe_int_input_stream_t *input_stream )
{
    /* Encapsulate user-data */
    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        if( raw_frame->user_data[i].type == USER_DATA_CEA_608 )
            write_608_cc( &raw_frame->user_data[i], input_stream );
        else if( raw_frame->user_data[i].type == USER_DATA_CEA_708_CDP )
            write_cdp( &raw_frame->user_data[i] );
        else if( raw_frame->user_data[i].type == USER_DATA_AFD )
            write_afd( &raw_frame->user_data[i] );
        else if( raw_frame->user_data[i].type == USER_DATA_BAR_DATA )
            write_bar_data( &raw_frame->user_data[i] );
        else if( raw_frame->user_data[i].type == USER_DATA_WSS )
            convert_wss_to_afd( &raw_frame->user_data[i] );
    }

    return 0;
}

void *start_filter( void *ptr )
{
    obe_vid_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_int_input_stream_t *input_stream = filter_params->input_stream;
    obe_raw_frame_t *raw_frame;

    obe_vid_filter_ctx_t *vfilt = calloc( 1, sizeof(*vfilt) );
    if( !vfilt )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    init_filter( vfilt );

    while( 1 )
    {
        /* TODO: support resolution changes */
        /* TODO: support changes in pixel format */

        pthread_mutex_lock( &filter->filter_mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->filter_mutex );
            goto end;
        }

        if( !filter->num_raw_frames )
            pthread_cond_wait( &filter->filter_cv, &filter->filter_mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->filter_mutex );
            goto end;
        }

        raw_frame = filter->frames[0];
        pthread_mutex_unlock( &filter->filter_mutex );

        /* TODO: scale 8-bit to 10-bit */

        if( raw_frame->img.first_line )
            blank_lines( raw_frame );

        if( raw_frame->img.csp == PIX_FMT_YUV422P || raw_frame->img.csp == PIX_FMT_YUV422P10 )
        {
            if( downconvert_frame( vfilt, raw_frame ) < 0 )
                goto end;
        }

        if( raw_frame->img.csp == PIX_FMT_YUV420P10 && X264_BIT_DEPTH == 8 )
        {
            if( dither_image( vfilt, raw_frame ) < 0 )
                goto end;
        }

        if( encapsulate_user_data( raw_frame, input_stream ) < 0 )
            goto end;

        remove_frame_from_filter_queue( filter );
        add_to_encode_queue( h, raw_frame );
    }

end:
    if( vfilt )
    {
        if( vfilt->sws_ctx )
            sws_freeContext( vfilt->sws_ctx );

        free( vfilt );
    }

    free( filter_params );

    return NULL;
}

const obe_vid_filter_func_t video_filter = { start_filter };
