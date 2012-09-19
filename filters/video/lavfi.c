/*****************************************************************************
 * lavfi.c: libavfilter handling code
 *****************************************************************************
 * Copyright (C) 2010-2012 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@ob-encoder.com>
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
#include "common/lavc.h"
#include "video.h"
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>

static void lavfi_free( struct AVFilterBuffer *buf )
{
    obe_raw_frame_t *raw_frame = buf->priv;

    if( buf->extended_data != buf->data )
        av_freep( &buf->extended_data );

    raw_frame->release_data( raw_frame );
    raw_frame->release_frame( raw_frame );

    av_free( buf );
}

static int read_logo( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    AVPacket pkt;
    AVCodecContext *c = NULL;
    AVFrame frame;
    int finished = 0, ret = 0, sws_flags;
    av_init_packet( &pkt );

    av_register_all();

    ret = avformat_open_input( &vfilt->logo_format, vfilt->filter_opts.logo_filename, NULL, NULL );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Could not open logo input\n" );
        return -1;
    }

    c = vfilt->logo_format->streams[0]->codec;
    c->get_buffer = obe_get_buffer;
    c->release_buffer = obe_release_buffer;
    c->reget_buffer = obe_reget_buffer;
    c->flags |= CODEC_FLAG_EMU_EDGE;

    if( avcodec_open2( c, avcodec_find_decoder( c->codec_id ), NULL ) < 0 )
    {
        syslog( LOG_ERR, "Could not open logo decoder\n" );
        ret = -1;
        goto end;
    }

    if( av_read_frame( vfilt->logo_format, &pkt ) < 0 )
    {
        syslog( LOG_ERR, "Could not read logo packet\n" );
        ret = -1;
        goto end;
    }

    if( pkt.stream_index == 0 )
    {
        while( !finished && ret >= 0 )
            ret = avcodec_decode_video2( c, &frame, &finished, &pkt );

        if( ret < 0 )
            goto end;

        sws_flags = SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_LANCZOS;
        vfilt->logo_sws_ctx = sws_getContext( frame.width, frame.height, c->pix_fmt,
                                              frame.width, frame.height, PIX_FMT_YUVA420P,
                                              sws_flags, NULL, NULL, NULL );
        if( !vfilt->logo_sws_ctx )
        {
            syslog( LOG_ERR, "Colourspace conversion initialisation failed\n" );
            goto end;
        }

        if( av_image_alloc( vfilt->logo.plane, vfilt->logo.stride, frame.width, frame.height,
                            PIX_FMT_YUVA420P, 16 ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            ret = -1;
            goto end;
        }

        sws_scale( vfilt->logo_sws_ctx, (const uint8_t * const*)frame.data, frame.linesize,
                   0, frame.height, vfilt->logo.plane, vfilt->logo.stride );

        av_freep( &frame.data[0] );

        vfilt->logo.csp = PIX_FMT_YUVA420P;
        vfilt->logo.width = frame.width;
        vfilt->logo.height = frame.height;
        vfilt->logo.planes = av_pix_fmt_descriptors[PIX_FMT_YUVA420P].nb_components;

        uint8_t *a = vfilt->logo.plane[3];
        for( int i = 0; i < vfilt->logo.height; i++ )
        {
            for( int j = 0; j < vfilt->logo.width; j++ )
                a[j] = av_clip( a[j] - vfilt->filter_opts.logo_transparency, 0, 255 );

            a += vfilt->logo.stride[3];
        }

    }
end:
    if( c )
        avcodec_close( c );

    avformat_close_input( &vfilt->logo_format );

    av_free_packet( &pkt );

    if( vfilt->logo_sws_ctx )
        sws_freeContext( vfilt->logo_sws_ctx );

    return ret;
}

int init_lavfi( obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    char args[1024];
    int ret;
    AVFilterContext *last_filter;
    AVFilterBufferRef *logo_buf;

    avfilter_register_all();

    vfilt->filter_graph = avfilter_graph_alloc();
    if( !vfilt->filter_graph )
    {
        syslog( LOG_ERR, "unable to create filter graph \n" );
        return -1;
    }

    /* Buffer source options */
    snprintf( args, sizeof(args), "%d:%d:%d:%d:%"PRIi64":%d:%d", raw_frame->img.width, raw_frame->img.height, raw_frame->img.csp,
              1, (int64_t)OBE_CLOCK, raw_frame->sar_width, raw_frame->sar_height );
    ret = avfilter_graph_create_filter( &vfilt->video_src,  avfilter_get_by_name( "buffer" ), "src",  args, NULL, vfilt->filter_graph );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to create buffersrc\n" );
        goto end;
    }

    ret = avfilter_graph_create_filter( &vfilt->video_sink, avfilter_get_by_name( "buffersink" ), "sink", NULL, NULL, vfilt->filter_graph );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to create buffersink\n" );
        goto end;
    }

    if( vfilt->filter_opts.deinterlace )
    {
        /* TODO: deal with tff one day */
        snprintf( args, sizeof(args), "%i:0:0", vfilt->filter_opts.yadif_mode );
        ret = avfilter_graph_create_filter( &vfilt->yadif, avfilter_get_by_name( "yadif" ), "yadif", args, NULL, vfilt->filter_graph );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to create yadif\n" );
            goto end;
        }
    }

    if( vfilt->filter_opts.denoise )
    {
        ret = avfilter_graph_create_filter( &vfilt->hqdn3d, avfilter_get_by_name( "hqdn3d" ), "hqdn3d", vfilt->filter_opts.denoise_opts, NULL, vfilt->filter_graph );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to create hqdn3d\n" );
            goto end;
        }
    }

    if( vfilt->filter_opts.resize_width && vfilt->filter_opts.resize_height )
    {
        snprintf( args, sizeof(args), "%d:%d", vfilt->filter_opts.resize_width, vfilt->filter_opts.resize_height );
        ret = avfilter_graph_create_filter( &vfilt->resize, avfilter_get_by_name( "scale" ), "scale", args, NULL, vfilt->filter_graph );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to create buffersrc\n" );
            goto end;
        }
    }

    if( vfilt->filter_opts.logo_filename )
    {
        if( read_logo( vfilt, raw_frame ) < 0 )
        {
            fprintf( stderr, "unable to create filter graph \n" );
            return -1;
        }

        snprintf( args, sizeof(args), "%d:%d:%d:%d:%"PRIi64":%d:%d", vfilt->logo.width, vfilt->logo.height, vfilt->logo.csp,
                  1, (int64_t)OBE_CLOCK, 1, 1 );
        ret = avfilter_graph_create_filter( &vfilt->logo_src, avfilter_get_by_name( "buffer" ), "logosrc", args, NULL, vfilt->filter_graph );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to create buffersrc\n" );
            goto end;
        }

        ret = avfilter_graph_create_filter( &vfilt->overlay, avfilter_get_by_name( "overlay" ), "overlay", vfilt->filter_opts.logo_opts, NULL, vfilt->filter_graph );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to create overlay\n" );
            goto end;
        }

    }

    /* Filter linking */
    last_filter = vfilt->video_src;

    if( vfilt->filter_opts.deinterlace )
    {
        ret = avfilter_link( last_filter, 0, vfilt->yadif, 0 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }
        last_filter = vfilt->yadif;
    }

    if( vfilt->filter_opts.denoise )
    {
        ret = avfilter_link( last_filter, 0, vfilt->hqdn3d, 0 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }
        last_filter = vfilt->hqdn3d;
    }

    if( vfilt->filter_opts.resize_width && vfilt->filter_opts.resize_height )
    {
        ret = avfilter_link( last_filter, 0, vfilt->resize, 0 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }
        last_filter = vfilt->resize;
    }

    if( vfilt->filter_opts.logo_filename )
    {
        ret = avfilter_link( last_filter, 0, vfilt->overlay, 0 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }
        last_filter = vfilt->overlay;

        ret = avfilter_link( vfilt->logo_src, 0, vfilt->overlay, 1 );
        if( ret < 0 )
        {
            syslog( LOG_ERR, "Failed to link filter chain\n" );
            goto end;
        }

        logo_buf = avfilter_get_video_buffer_ref_from_arrays( vfilt->logo.plane, vfilt->logo.stride,
                                                              AV_PERM_READ | AV_PERM_WRITE, vfilt->logo.width,
                                                              vfilt->logo.height, vfilt->logo.csp );

        av_buffersrc_buffer( vfilt->logo_src, logo_buf );
        av_buffersrc_buffer( vfilt->logo_src, NULL );
    }

    ret = avfilter_link( last_filter, 0, vfilt->video_sink, 0 );
    if( ret < 0 )
    {
        syslog( LOG_ERR, "Failed to link filter chain\n" );
        goto end;
    }

    avfilter_graph_config( vfilt->filter_graph, NULL );

end:

    return ret;
}

int lavfi_filter_frame( obe_t *h, obe_vid_filter_ctx_t *vfilt, obe_raw_frame_t *raw_frame )
{
    AVFilterBufferRef *buf_ref;
    obe_raw_frame_t *fil_raw_frame;

    buf_ref = avfilter_get_video_buffer_ref_from_arrays( raw_frame->img.plane, raw_frame->img.stride,
                                                         AV_PERM_READ | AV_PERM_WRITE, raw_frame->img.width,
                                                         raw_frame->img.height, raw_frame->img.csp );

    buf_ref->pts = raw_frame->pts;
    buf_ref->buf->priv = raw_frame;
    buf_ref->buf->free = lavfi_free;

    /* We cannot guarantee raw_frame has not been freed after filtering */

    memcpy( &vfilt->raw_frame_bak[vfilt->bak_frames], raw_frame, sizeof(vfilt->raw_frame_bak[vfilt->bak_frames]) );
    vfilt->bak_frames = (vfilt->bak_frames + 1) % MAX_BACKUP_FRAMES;
    /* avoid freeing user data */
    raw_frame->user_data = NULL;
    raw_frame->num_user_data = 0;

    av_buffersrc_buffer( vfilt->video_src, buf_ref );
    buf_ref = NULL;

    int ret;

    while( 1 )
    {
        ret = av_buffersink_read( vfilt->video_sink, &buf_ref );

        if( ret == AVERROR(EAGAIN) || ret == AVERROR_EOF )
            break;
        if( ret < 0 )
            return -1;

        if( buf_ref )
        {
            fil_raw_frame = new_raw_frame();
            if( !fil_raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            if( vfilt->filter_opts.yadif_mode == 1 && vfilt->output_frames & 1 )
            {
                fil_raw_frame->pts = vfilt->last_pts + av_rescale_q( 1, (AVRational){ vfilt->timebase_num, vfilt->timebase_den * 2 }, (AVRational){1, OBE_CLOCK} );
                fil_raw_frame->img.csp = PIX_FMT_YUV420P;
                fil_raw_frame->sar_width = vfilt->sar_width;
                fil_raw_frame->sar_height = vfilt->sar_height;
            }
            else
            {
                memcpy( fil_raw_frame, &vfilt->raw_frame_bak[vfilt->last_output], sizeof(*fil_raw_frame) );
                vfilt->last_output = (vfilt->last_output + 1) % MAX_BACKUP_FRAMES;
                memset( fil_raw_frame->img.plane, 0, sizeof(fil_raw_frame->img.plane) );
                memset( fil_raw_frame->img.stride, 0, sizeof(fil_raw_frame->img.stride) );
                vfilt->last_pts = fil_raw_frame->pts;

                vfilt->timebase_num = fil_raw_frame->timebase_num;
                vfilt->timebase_den = fil_raw_frame->timebase_den;
                vfilt->sar_width = fil_raw_frame->sar_width;
                vfilt->sar_height = fil_raw_frame->sar_height;
            }

            memcpy( &fil_raw_frame->alloc_img, &fil_raw_frame->img, sizeof(fil_raw_frame->img) );
            fil_raw_frame->alloc_img.width = buf_ref->video->w;
            fil_raw_frame->alloc_img.height = buf_ref->video->h;

            /* Assume pixel format hasn't changed */
            if( av_image_alloc( fil_raw_frame->alloc_img.plane, fil_raw_frame->alloc_img.stride,
                                buf_ref->video->w, buf_ref->video->h, fil_raw_frame->alloc_img.csp, 16 ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }

            /* Regrettable memcpy */
            av_image_copy( fil_raw_frame->alloc_img.plane, fil_raw_frame->alloc_img.stride,
                           (const uint8_t **)buf_ref->data, buf_ref->linesize,
                           fil_raw_frame->alloc_img.csp, buf_ref->video->w, buf_ref->video->h );

            memcpy( &fil_raw_frame->img, &fil_raw_frame->alloc_img, sizeof(fil_raw_frame->img) );
            fil_raw_frame->release_data = obe_release_video_data;
            fil_raw_frame->release_frame = obe_release_frame;

            vfilt->output_frames++;

            avfilter_unref_bufferp( &buf_ref );
            add_to_encode_queue( h, fil_raw_frame, 0 );
        }
    }

    return 0;

}
