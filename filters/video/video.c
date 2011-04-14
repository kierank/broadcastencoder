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
#include "filters/video/video.h"

#if X264_BIT_DEPTH > 8
typedef uint16_t pixel;
#else
typedef uint8_t pixel;
#endif

typedef struct
{
    /* scaling */
    struct SwsContext *sws_ctx;
    int sws_ctx_flags;
    enum PixelFormat dst_pix_fmt;

    /* dither */
    int16_t *error_buf;
} obe_vid_filter_ctx_t;

typedef struct
{
    int planes;
    float width[4];
    float height[4];
    int mod_width;
    int mod_height;
} obe_cli_csp_t;

const obe_cli_csp_t obe_cli_csps[] =
{
    [PIX_FMT_YUV420P] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2 },
    [PIX_FMT_NV12] =    { 2, { 1,  1 },     { 1, .5 },     2, 2 },
    [PIX_FMT_YUV420P16] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2 },
};

static uint32_t convert_cpu_to_flag( void )
{
    uint32_t avutil_cpu = av_get_cpu_flags();
    uint32_t swscale_cpu = 0;
    if( avutil_cpu & AV_CPU_FLAG_ALTIVEC )
        swscale_cpu |= SWS_CPU_CAPS_ALTIVEC;
    if( avutil_cpu & AV_CPU_FLAG_MMX )
        swscale_cpu |= SWS_CPU_CAPS_MMX;
    if( avutil_cpu & AV_CPU_FLAG_MMX2 )
        swscale_cpu |= SWS_CPU_CAPS_MMX2;
    return swscale_cpu;
}

static int scale_frame( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    obe_image_t tmp_image = {0};
    tmp_image.planes = 3; // FIXME

    if( !vfilt->sws_ctx )
    {
        vfilt->sws_ctx_flags |= SWS_SPLINE;
        vfilt->sws_ctx_flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND;
        vfilt->sws_ctx_flags |= convert_cpu_to_flag();
        vfilt->dst_pix_fmt = raw_frame->img.csp == PIX_FMT_YUV422P ? PIX_FMT_YUV420P : PIX_FMT_YUV420P16;

        vfilt->sws_ctx = sws_getContext( raw_frame->img.width, raw_frame->img.height, raw_frame->img.csp,
                                         raw_frame->img.width, raw_frame->img.height, vfilt->dst_pix_fmt,
                                         vfilt->sws_ctx_flags, NULL, NULL, NULL );
        if( !vfilt->sws_ctx )
        {
            fprintf( stderr, "swsfail \n" );
            // TODO fail
        }

    }

    tmp_image.csp = vfilt->dst_pix_fmt;
    tmp_image.width = raw_frame->img.width;
    tmp_image.height = raw_frame->img.height;

    if( av_image_alloc( tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height,
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

    tmp_image.csp = PIX_FMT_YUV420P;
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
    memcpy( &raw_frame->img, out, sizeof(*out) );

    return 0;
}

int parse_user_data( obe_raw_frame_t *raw_frame )
{
    /* Parse user-data */
    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
    }

    return 0;
}

void *start_filter( void *ptr )
{
    obe_vid_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_raw_frame_t *raw_frame;

    obe_vid_filter_ctx_t *vfilt = calloc( 1, sizeof(*vfilt) );
    if( !vfilt )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    while( 1 )
    {
        /* TODO: support resolution changes */
        /* TODO: handle actual 10-bit images */
        /* TODO: support changes in pixel format */

        pthread_mutex_lock( &filter->filter_mutex );

        if( !filter->num_raw_frames )
            pthread_cond_wait( &filter->filter_cv, &filter->filter_mutex );

        raw_frame = filter->frames[0];
        pthread_mutex_unlock( &filter->filter_mutex );

        if( raw_frame->img.csp == PIX_FMT_YUV422P || raw_frame->img.csp == PIX_FMT_YUV422P16 )
        {
            if( scale_frame( vfilt, raw_frame ) < 0 )
                break;
        }

        if( raw_frame->img.csp == PIX_FMT_YUV420P16 )
        {
            if( !vfilt->error_buf )
            {
                vfilt->error_buf = malloc( (raw_frame->img.width+1) * sizeof(*vfilt->error_buf) );
                if( !vfilt->error_buf )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    break;
                }
            }

            if( dither_image( raw_frame, vfilt->error_buf ) < 0 )
                break;
        }

        if( parse_user_data( raw_frame ) < 0 )
            break;

        remove_frame_from_filter_queue( filter );
        add_to_encode_queue( h, raw_frame );
    }

    if( !vfilt->sws_ctx )
    {
        sws_freeContext( vfilt->sws_ctx );
    }

    return NULL;
}

const obe_vid_filter_func_t video_filter = { start_filter };
