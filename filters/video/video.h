/*****************************************************************************
 * video.h : OBE video filter headers
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

#ifndef OBE_FILTERS_VIDEO_H
#define OBE_FILTERS_VIDEO_H

#include <libavfilter/avfiltergraph.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define MAX_BACKUP_FRAMES 10 // arbitrary

typedef struct
{
    void* (*start_filter)( void *ptr );
} obe_vid_filter_func_t;

typedef struct
{
    obe_t *h;
    obe_filter_t *filter;
    obe_int_input_stream_t *input_stream;
    int target_csp;
} obe_vid_filter_params_t;

extern const obe_vid_filter_func_t video_filter;

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

    /* libavfilter */
    obe_filter_opts_t filter_opts;

    AVFilterGraph *filter_graph;
    AVFilterContext *video_src;
    AVFilterContext *yadif;
    AVFilterContext *hqdn3d;
    AVFilterContext *resize;
    AVFilterContext *logo_src;
    AVFilterContext *overlay;
    AVFilterContext *video_sink;

    int64_t last_pts;
    int timebase_num;
    int timebase_den;
    int sar_width;
    int sar_height;
    int64_t output_frames;
    int bak_frames;
    int last_output;
    obe_raw_frame_t raw_frame_bak[MAX_BACKUP_FRAMES];

    /* logo */
    struct SwsContext *logo_sws_ctx;
    AVFormatContext *logo_format;
    AVCodec         *dec;
    AVCodecContext  *codec;
    obe_image_t logo;

} obe_vid_filter_ctx_t;

#endif
