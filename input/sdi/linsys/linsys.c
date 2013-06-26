/*****************************************************************************
 * linsys.c: linsys sdi card input
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Some code originates from the VideoLAN project
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

/* NOTE: Using valgrind to debug this card does not work because of the ioctls used! */

#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "common/common.h"
#include "common/lavc.h"
#include "common/linsys/util.h"
#include "include/sdi.h"
#include "include/sdivideo.h"
#include "include/sdiaudio.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"

#include <libavutil/mathematics.h>
#include <libavutil/bswap.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>

#define SDIVIDEO_DEVICE         "/dev/sdivideorx%u"
#define SDIVIDEO_BUFFERS_FILE   "/sys/class/sdivideo/sdivideorx%u/buffers"
#define SDIVIDEO_BUFSIZE_FILE   "/sys/class/sdivideo/sdivideorx%u/bufsize"
#define SDIVIDEO_MODE_FILE      "/sys/class/sdivideo/sdivideorx%u/mode"
#define SDIVIDEO_VANC_FILE      "/sys/class/sdivideo/sdivideorx%u/vanc"
#define SDIAUDIO_DEVICE         "/dev/sdiaudiorx%u"
#define SDIAUDIO_BUFFERS_FILE   "/sys/class/sdiaudio/sdiaudiorx%u/buffers"
#define SDIAUDIO_BUFSIZE_FILE   "/sys/class/sdiaudio/sdiaudiorx%u/bufsize"
#define SDIAUDIO_SAMPLESIZE_FILE "/sys/class/sdiaudio/sdiaudiorx%u/sample_size"
#define SDIAUDIO_CHANNELS_FILE  "/sys/class/sdiaudio/sdiaudiorx%u/channels"
#define READ_TIMEOUT            10000
#define NB_VBUFFERS             2
#define NB_ABUFFERS             2
#define LINSYS_VANC_LINES       100
#define LINSYS_NTSC_TOP_LINES   6

struct obe_to_linsys_video
{
    int obe_name;
    int linsys_name;
    int timebase_num;
    int timebase_den;
    int width;
    int height;
    int total_height;
    int tff;
};

const static struct obe_to_linsys_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_PAL,        SDIVIDEO_CTL_BT_601_576I_50HZ,         1,    25,    720,  576,  625,  1 },
    { INPUT_VIDEO_FORMAT_NTSC,       SDIVIDEO_CTL_SMPTE_125M_486I_59_94HZ,  1001, 30000, 720,  486,  525,  0 },
    { INPUT_VIDEO_FORMAT_720P_50,    SDIVIDEO_CTL_SMPTE_296M_720P_50HZ,     1,    50,    1280, 720,  750,  0 },
    { INPUT_VIDEO_FORMAT_720P_5994,  SDIVIDEO_CTL_SMPTE_296M_720P_59_94HZ,  1001, 60000, 1280, 720,  750,  0 },
    { INPUT_VIDEO_FORMAT_720P_60,    SDIVIDEO_CTL_SMPTE_296M_720P_60HZ,     1,    60,    1280, 720,  750,  0 },
    { INPUT_VIDEO_FORMAT_1080I_50,   SDIVIDEO_CTL_SMPTE_274M_1080I_50HZ,    1,    25,    1920, 1080, 1125, 1 },
    { INPUT_VIDEO_FORMAT_1080I_5994, SDIVIDEO_CTL_SMPTE_274M_1080I_59_94HZ, 1001, 30000, 1920, 1080, 1125, 1 },
    { INPUT_VIDEO_FORMAT_1080I_60,   SDIVIDEO_CTL_SMPTE_274M_1080I_60HZ,    1,    60,    1920, 1080, 1125, 1 },
    { INPUT_VIDEO_FORMAT_1080P_2398, SDIVIDEO_CTL_SMPTE_274M_1080P_23_98HZ, 1001, 24000, 1920, 1080, 1125, 0 },
    { INPUT_VIDEO_FORMAT_1080P_24,   SDIVIDEO_CTL_SMPTE_274M_1080P_24HZ,    1,    24,    1920, 1080, 1125, 0 },
    { INPUT_VIDEO_FORMAT_1080P_25,   SDIVIDEO_CTL_SMPTE_274M_1080P_25HZ,    1,    25,    1920, 1080, 1125, 0 },
    { INPUT_VIDEO_FORMAT_1080P_2997, SDIVIDEO_CTL_SMPTE_274M_1080P_29_97HZ, 1001, 30000, 1920, 1080, 1125, 0 },
    { INPUT_VIDEO_FORMAT_1080P_30,   SDIVIDEO_CTL_SMPTE_274M_1080P_30HZ,    1,    30,    1920, 1080, 1125, 0 },
    { -1, -1, -1, -1, -1, -1 },
};

struct linsys_audio_channels
{
    int linsys_name;
    int num_channels;
};

const static struct linsys_audio_channels active_audio_tab[] =
{
    { SDIAUDIO_CTL_ACT_CHAN_0, 0 },
    { SDIAUDIO_CTL_ACT_CHAN_2, 2 },
    { SDIAUDIO_CTL_ACT_CHAN_4, 4 },
    { SDIAUDIO_CTL_ACT_CHAN_6, 6 },
    { SDIAUDIO_CTL_ACT_CHAN_8, 8 },
    { -1, -1 },
};

const static struct linsys_audio_channels audio_channels_tab[] =
{
    { SDIAUDIO_CTL_AUDCH_EN_0, 0 },
    { SDIAUDIO_CTL_AUDCH_EN_2, 2 },
    { SDIAUDIO_CTL_AUDCH_EN_4, 4 },
    { SDIAUDIO_CTL_AUDCH_EN_6, 6 },
    { SDIAUDIO_CTL_AUDCH_EN_8, 8 },
    { -1, -1 },
};

typedef struct
{
    /* video device reader */
    int          vfd;
    unsigned int link;
    unsigned int standard;
    uint8_t      **vbuffers;
    unsigned int num_vbuffers, current_vbuffer;
    unsigned int vbuffer_size;
    int          has_vanc;
    int          stride;
    int          width;
    int          coded_height;
    int64_t      v_counter;
    AVRational   v_timebase;

    obe_raw_frame_t *raw_frame;
    void (*unpack_line) ( const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width );

    /* audio device reader */
    int          afd;
    int          max_channel;
    unsigned int sample_rate;
    uint8_t      **abuffers;
    unsigned int num_abuffers, current_abuffer;
    unsigned int abuffer_size;
    int64_t      a_counter;
    AVRational   a_timebase;
    AVAudioResampleContext *avr;

    int64_t      last_frame_time;

#if 0
    int          probe_buf_len;
    int32_t      *audio_probe_buf;
#endif

    /* VBI */
    int has_setup_vbi;

    /* Ancillary */
    void (*pack_line) ( uint16_t *y, uint16_t *u, uint16_t *v, uint16_t *dst, int width );
    void (*downscale_line) ( uint16_t *src, uint8_t *dst, int lines );
    obe_sdi_non_display_data_t non_display_parser;

    obe_device_t *device;
    obe_t *h;
} linsys_ctx_t;

typedef struct
{
    linsys_ctx_t linsys_ctx;

    /* Input */
    int card_idx;
    int num_channels;
    int probe;
    int audio_samples;

    /* Output */
    int video_format;
    int width;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;

    int sample_rate;
} linsys_opts_t;

struct linsys_status
{
    obe_input_params_t *input;
    linsys_opts_t *linsys_opts;
};

#define MAXLEN 256

#define READ_PIXELS(a, b, c)         \
    do {                             \
        val  = av_le2ne32(*src++);   \
        *a++ =  val & 0x3FF;         \
        *b++ = (val >> 10) & 0x3FF;  \
        *c++ = (val >> 20) & 0x3FF;  \
    } while (0)

static inline void obe_decode_line( linsys_ctx_t *linsys_ctx, const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v )
{
    uint32_t val;
    int w;

    w = (linsys_ctx->width / 6) * 6;
    linsys_ctx->unpack_line( src, y, u, v, w );

    y += w;
    u += w >> 1;
    v += w >> 1;
    src += (w << 1) / 3;

   if( w < linsys_ctx->width - 1 )
   {
       READ_PIXELS(u, y, v);

       val  = av_le2ne32(*src++);
       *y++ =  val & 0x3FF;
   }

   if( w < linsys_ctx->width - 3 )
   {
       *u++ = (val >> 10) & 0x3FF;
       *y++ = (val >> 20) & 0x3FF;

       val  = av_le2ne32(*src++);
       *v++ =  val & 0x3FF;
       *y++ = (val >> 10) & 0x3FF;
   }
}

static ssize_t write_ul_sysfs( const char *fmt, unsigned int card_idx, unsigned int buf )
{
    char filename[MAXLEN], data[MAXLEN];
    int fd;
    ssize_t ret;

    snprintf( filename, sizeof(filename) -1, fmt, card_idx );

    snprintf( data, sizeof(data) -1, "%u\n", buf );

    if ( (fd = open( filename, O_WRONLY )) < 0 )
        return fd;

    ret = write( fd, data, strlen( data ) + 1 );

    close( fd );
    return ret;
}

static void close_card( linsys_opts_t *linsys_opts )
{
    linsys_ctx_t *linsys_ctx = &linsys_opts->linsys_ctx;

    if( linsys_ctx->vbuffers )
    {
        for( int i = 0; i < linsys_ctx->num_vbuffers; i++ )
            munmap( linsys_ctx->vbuffers[i], linsys_ctx->vbuffer_size );

        free( linsys_ctx->vbuffers );
    }
    close( linsys_ctx->vfd );

    if( linsys_ctx->abuffers )
    {
        for( int i = 0; i < linsys_ctx->num_abuffers; i++ )
            munmap( linsys_ctx->abuffers[i], linsys_ctx->abuffer_size );

        free( linsys_ctx->abuffers );
    }
    close( linsys_ctx->afd );

    if( linsys_ctx->avr )
        avresample_free( &linsys_ctx->avr );
}

static int handle_video_frame( linsys_opts_t *linsys_opts, uint8_t *data )
{
    linsys_ctx_t *linsys_ctx = &linsys_opts->linsys_ctx;
    obe_t *h = linsys_ctx->h;
    obe_raw_frame_t *raw_frame = NULL;
    int num_anc_lines = 0, anc_line_stride, first_line = 0, last_line = 0, cur_line, num_vbi_lines, vii_line, tmp_line;
    uint16_t *anc_buf = NULL, *anc_buf_pos = NULL;
    uint16_t *y_src, *u_src, *v_src;
    uint8_t *vbi_buf;
    int64_t pts, sdi_clock;

    obe_image_t *output;

    if( linsys_ctx->non_display_parser.has_probed )
        return 0;

    /* use SDI ticks as clock source */
    sdi_clock = av_rescale_q( linsys_ctx->v_counter, linsys_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
    obe_clock_tick( h, sdi_clock );

    if( linsys_ctx->last_frame_time == -1 )
        linsys_ctx->last_frame_time = obe_mdate();
    else
    {
        int64_t cur_frame_time = obe_mdate();
        if( cur_frame_time - linsys_ctx->last_frame_time >= SDI_MAX_DELAY )
        {
            syslog( LOG_WARNING, "Linsys card index %i: No frame received for %"PRIi64" ms", linsys_opts->card_idx,
                   (cur_frame_time - linsys_ctx->last_frame_time) / 1000 );
            pthread_mutex_lock( &h->drop_mutex );
            h->encoder_drop = h->mux_drop = 1;
            pthread_mutex_unlock( &h->drop_mutex );
        }

        linsys_ctx->last_frame_time = cur_frame_time;
    }

    int j;
    for( j = 0; first_active_line[j].format != -1; j++ )
    {
        if( linsys_opts->video_format == first_active_line[j].format )
            break;
    }

    /* Create raw frame */
    raw_frame = new_raw_frame();
    if( !raw_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    output = &raw_frame->alloc_img;

    raw_frame->release_data = obe_release_video_data;
    raw_frame->release_frame = obe_release_frame;
    raw_frame->arrival_time = linsys_ctx->last_frame_time;

    output->csp = PIX_FMT_YUV422P10;
    output->planes = av_pix_fmt_descriptors[output->csp].nb_components;
    output->width = linsys_ctx->width;
    output->height = linsys_opts->height;

    if( av_image_fill_linesizes( output->stride, output->csp, output->width ) < 0 )
        goto fail;

    if( av_image_alloc( output->plane, output->stride, linsys_ctx->width, linsys_ctx->coded_height + 1, PIX_FMT_YUV422P10, 16 ) < 0 )
        goto fail;

    uint16_t *y_dst = (uint16_t*)output->plane[0];
    uint16_t *u_dst = (uint16_t*)output->plane[1];
    uint16_t *v_dst = (uint16_t*)output->plane[2];

    /* Interleave fields */
    if( linsys_opts->interlaced )
    {
        uint8_t *v210_src_f1, *v210_src_f2;

        int k;
        for( k = 0; field_start_lines[k].format != -1; k++ )
        {
            if( linsys_opts->video_format == field_start_lines[k].format )
                break;
        }

        v210_src_f1 = v210_src_f2 = data;

        /* If we can only access the active frame in NTSC mode then swap the field order */
        if( !linsys_ctx->has_vanc && linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
            v210_src_f1 += (linsys_ctx->coded_height / 2) * linsys_ctx->stride;
        else if( linsys_ctx->has_vanc )
            v210_src_f2 += (field_start_lines[k].field_two - field_start_lines[k].line) * linsys_ctx->stride;
        else
            /* All non-VANC resolutions have an even height */
            v210_src_f2 += (linsys_ctx->coded_height / 2) * linsys_ctx->stride;

        for( int i = 0; i < linsys_ctx->coded_height; i++ )
        {
            if( !(i & 1) )
            {
                obe_decode_line( linsys_ctx, (const uint32_t*)v210_src_f1, y_dst, u_dst, v_dst );
                v210_src_f1 += linsys_ctx->stride;
            }
            else
            {
                obe_decode_line( linsys_ctx, (const uint32_t*)v210_src_f2, y_dst, u_dst, v_dst );
                v210_src_f2 += linsys_ctx->stride;
            }

            y_dst += output->stride[0] / 2;
            u_dst += output->stride[1] / 2;
            v_dst += output->stride[2] / 2;
        }
    }
    else
    {
        for( int i = 0; i < linsys_ctx->coded_height; i++ )
        {
            obe_decode_line( linsys_ctx, (const uint32_t*)data, y_dst, u_dst, v_dst );

            data += linsys_ctx->stride;
            y_dst += output->stride[0] / 2;
            u_dst += output->stride[1] / 2;
            v_dst += output->stride[2] / 2;
        }
    }

    anc_line_stride = FFALIGN( (linsys_ctx->width * 2 * sizeof(uint16_t)), 16 );

    y_src = (uint16_t*)output->plane[0];
    u_src = (uint16_t*)output->plane[1];
    v_src = (uint16_t*)output->plane[2];

    /* Handle VANC if the card allows it */
    if( linsys_ctx->has_vanc )
    {
        /* NTSC starts on line 4 so skip the top lines for NTSC */
        if( linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
        {
            y_src += output->stride[0] * LINSYS_NTSC_TOP_LINES / 2;
            u_src += output->stride[1] * LINSYS_NTSC_TOP_LINES / 2;
            v_src += output->stride[2] * LINSYS_NTSC_TOP_LINES / 2;
        }

        first_line = cur_line = linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC ? 4 : 1;

        /* Overallocate slightly for VANC buffer
         * Some VBI services stray into the active picture so allocate some extra space */
        anc_buf = anc_buf_pos = av_malloc( LINSYS_VANC_LINES * anc_line_stride );
        if( !anc_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto fail;
        }

        while( cur_line != first_active_line[j].line )
        {
            linsys_ctx->pack_line( y_src, u_src, v_src, anc_buf_pos, linsys_ctx->width );
            parse_vanc_line( h, &linsys_ctx->non_display_parser, raw_frame, anc_buf_pos, linsys_ctx->width, cur_line );
            anc_buf_pos += anc_line_stride / 2;

            y_src += output->stride[0] / 2;
            u_src += output->stride[1] / 2;
            v_src += output->stride[2] / 2;

            cur_line = sdi_next_line( linsys_opts->video_format, cur_line );
            num_anc_lines++;
        }
    }
    else
        first_line = cur_line = first_active_line[j].line;

    if( IS_SD( linsys_opts->video_format ) )
    {
        /* Include the first two lines which are visible but used for VBI data */
        num_vbi_lines = NUM_ACTIVE_VBI_LINES;
        if( !linsys_ctx->has_vanc )
        {
            /* skip line 283 for NTSC since libzvbi doesn't like unpaired lines */
            if( linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
            {
                y_src += output->stride[0] / 2;
                u_src += output->stride[1] / 2;
                v_src += output->stride[2] / 2;
                cur_line = sdi_next_line( linsys_opts->video_format, cur_line );
                last_line = first_active_line[j].line;
                first_line = sdi_next_line( linsys_opts->video_format, last_line );
            }

            /* Only the first two lines can be probed for VBI data */
            anc_buf = anc_buf_pos = av_malloc( NUM_ACTIVE_VBI_LINES * anc_line_stride );
            if( !anc_buf )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto fail;
            }
        }
        else
            num_vbi_lines += linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC;

        num_anc_lines += num_vbi_lines;
        /* last_line is the last line that has been written, whereas cur_line is the next line to be processed */
        last_line = sdi_next_line( linsys_opts->video_format, cur_line-1 );

        /* Add the visible VBI lines to the ancillary buffer */
        for( int i = 0; i < num_vbi_lines; i++ )
        {
            linsys_ctx->pack_line( y_src, u_src, v_src, anc_buf_pos, linsys_ctx->width );
            anc_buf_pos += anc_line_stride / 2;
            y_src += output->stride[0] / 2;
            u_src += output->stride[1] / 2;
            v_src += output->stride[2] / 2;
            last_line = sdi_next_line( linsys_opts->video_format, last_line );
        }

        vbi_buf = av_malloc( linsys_ctx->width * 2 * num_anc_lines );
        if( !vbi_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto fail;
        }

        /* Scale the lines from 10-bit to 8-bit */
        linsys_ctx->downscale_line( anc_buf, vbi_buf, num_anc_lines );
        anc_buf_pos = anc_buf;

        if( linsys_ctx->has_vanc )
        {
            /* Handle Video Index information */
            tmp_line = first_line;
            vii_line = linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC ? NTSC_VIDEO_INDEX_LINE : PAL_VIDEO_INDEX_LINE;
            while( tmp_line < vii_line )
            {
                anc_buf_pos += anc_line_stride / 2;
                tmp_line++;
            }

            if( decode_video_index_information( h, &linsys_ctx->non_display_parser, anc_buf_pos, raw_frame, vii_line ) < 0 )
                goto fail;
        }

        if( !linsys_ctx->has_setup_vbi )
        {
            vbi_raw_decoder_init( &linsys_ctx->non_display_parser.vbi_decoder );

            linsys_ctx->non_display_parser.ntsc = linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC;
            linsys_ctx->non_display_parser.vbi_decoder.start[0] = first_line;
            linsys_ctx->non_display_parser.vbi_decoder.start[1] = sdi_next_line( linsys_opts->video_format, first_line );
            linsys_ctx->non_display_parser.vbi_decoder.count[0] = last_line - linsys_ctx->non_display_parser.vbi_decoder.start[1] + 1;
            linsys_ctx->non_display_parser.vbi_decoder.count[1] = linsys_ctx->non_display_parser.vbi_decoder.count[0];

            if( setup_vbi_parser( &linsys_ctx->non_display_parser ) < 0 )
                goto fail;

            linsys_ctx->has_setup_vbi = 1;
        }

        if( decode_vbi( h, &linsys_ctx->non_display_parser, vbi_buf, raw_frame ) < 0 )
            goto fail;

        av_free( vbi_buf );
    }

    av_free( anc_buf );

    if( linsys_opts->probe )
    {
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }
    else
    {
        if( linsys_ctx->has_vanc )
        {
            /* Just present the coded picture to the encoder */
            y_src = (uint16_t*)output->plane[0];
            u_src = (uint16_t*)output->plane[1];
            v_src = (uint16_t*)output->plane[2];

            /* Skip the 6 top lines for NTSC */
            if( linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
            {
                y_src += output->stride[0] * LINSYS_NTSC_TOP_LINES / 2;
                u_src += output->stride[1] * LINSYS_NTSC_TOP_LINES / 2;
                v_src += output->stride[2] * LINSYS_NTSC_TOP_LINES / 2;
            }

            cur_line = first_line;

            while( cur_line != first_active_line[j].line )
            {
                y_src += output->stride[0] / 2;
                u_src += output->stride[1] / 2;
                v_src += output->stride[2] / 2;

                cur_line = sdi_next_line( linsys_opts->video_format, cur_line );
            }

            raw_frame->img.csp = PIX_FMT_YUV422P10;
            raw_frame->img.planes = av_pix_fmt_descriptors[raw_frame->img.csp].nb_components;
            raw_frame->img.plane[0] = (uint8_t*)y_src;
            raw_frame->img.plane[1] = (uint8_t*)u_src;
            raw_frame->img.plane[2] = (uint8_t*)v_src;

            memcpy( raw_frame->img.stride, output->stride, sizeof(output->stride) );
            raw_frame->img.width = linsys_opts->width;
            raw_frame->img.height = linsys_opts->height;
        }
        else
        {
            memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->img) );

            /* non-VANC mode adds 6 junk lines at the end */
            if( linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
                raw_frame->img.height = 480;
        }

        if( IS_SD( linsys_opts->video_format ) )
        {
            raw_frame->img.format     = linsys_opts->video_format;
            raw_frame->img.first_line = first_active_line[j].line;
        }

        raw_frame->timebase_num = linsys_opts->timebase_num;
        raw_frame->timebase_den = linsys_opts->timebase_den;

        /* If AFD is present and the stream is SD this will be changed in the video filter */
        raw_frame->sar_width = raw_frame->sar_height = 1;
        raw_frame->pts = pts = av_rescale_q( linsys_ctx->v_counter++, linsys_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );

        if( add_to_filter_queue( h, raw_frame ) < 0 )
            goto fail;

        if( send_vbi_and_ttx( h, &linsys_ctx->non_display_parser, pts ) < 0 )
            goto fail;

        linsys_ctx->non_display_parser.num_vbi = 0;
        linsys_ctx->non_display_parser.num_anc_vbi = 0;
    }

    return 0;

fail:
    if( raw_frame )
    {
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    return -1;
}

static int handle_audio_frame( linsys_opts_t *linsys_opts, uint8_t *data )
{
    linsys_ctx_t *linsys_ctx = &linsys_opts->linsys_ctx;

    obe_raw_frame_t *raw_frame = new_raw_frame();
    if( !raw_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    raw_frame->audio_frame.num_samples = linsys_ctx->abuffer_size / ( sizeof(int32_t) * linsys_opts->num_channels );
    raw_frame->audio_frame.num_channels = linsys_opts->num_channels;
    raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;

    if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, linsys_opts->num_channels,
                          raw_frame->audio_frame.num_samples, raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    if( avresample_convert( linsys_ctx->avr, raw_frame->audio_frame.audio_data, raw_frame->audio_frame.linesize,
                            raw_frame->audio_frame.num_samples, &data,
                            linsys_ctx->abuffer_size, raw_frame->audio_frame.num_samples ) < 0 )
    {
        syslog( LOG_ERR, "[linsys-sdiaudio] Sample format conversion failed\n" );
        return -1;
    }

    raw_frame->pts = av_rescale_q( linsys_ctx->a_counter, linsys_ctx->a_timebase, (AVRational){1, OBE_CLOCK} );
    linsys_ctx->a_counter += raw_frame->audio_frame.num_samples;

    raw_frame->release_data = obe_release_audio_data;
    raw_frame->release_frame = obe_release_frame;
    for( int i = 0; i < linsys_ctx->device->num_input_streams; i++ )
    {
        if( linsys_ctx->device->streams[i]->stream_format == AUDIO_PCM )
            raw_frame->input_stream_id = linsys_ctx->device->streams[i]->input_stream_id;
    }

    if( add_to_filter_queue( linsys_ctx->h, raw_frame ) < 0 )
    {
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        return -1;
    }

    return 0;
}

static int capture_data( linsys_opts_t *linsys_opts )
{
    struct pollfd pfd[2];
    linsys_ctx_t *linsys_ctx = &linsys_opts->linsys_ctx;

    pfd[0].fd = linsys_ctx->vfd;
    pfd[0].events = POLLIN | POLLPRI;

    pfd[1].fd = linsys_ctx->afd;
    pfd[1].events = POLLIN | POLLPRI;

    if( poll( pfd, 2, READ_TIMEOUT ) < 0 )
    {
        syslog( LOG_ERR, "couldn't poll(): %s", strerror( errno ) );
        return -1;
    }

    /* TODO: card-idx these messages */

    if( pfd[0].revents & POLLPRI )
    {
        unsigned int val;

        if( ioctl( linsys_ctx->vfd, SDIVIDEO_IOC_RXGETEVENTS, &val ) < 0 )
            syslog( LOG_WARNING, "[linsys-sdivideo] could not SDIVIDEO_IOC_RXGETEVENTS %s", strerror( errno ) );
        else
        {
            if( val & SDIVIDEO_EVENT_RX_BUFFER )
                syslog( LOG_WARNING, "[linsys-sdivideo] driver receive buffer queue overrun \n" );
            if( val & SDIVIDEO_EVENT_RX_FIFO )
                syslog( LOG_WARNING, "[linsys-sdivideo] onboard receive FIFO overrun \n");
            if( val & SDIVIDEO_EVENT_RX_CARRIER )
                syslog( LOG_WARNING, "[linsys-sdivideo] carrier status change \n");
            if( val & SDIVIDEO_EVENT_RX_DATA )
                syslog( LOG_WARNING, "[linsys-sdivideo] data status change \n");
            if( val & SDIVIDEO_EVENT_RX_STD )
            {
                /* TODO: support format switching (rare in SDI) */
                syslog( LOG_WARNING, "[linsys-sdivideo] format change \n");
            }
        }
    }

    if( pfd[1].revents & POLLPRI )
    {
        unsigned int val;

        if( ioctl( linsys_ctx->afd, SDIAUDIO_IOC_RXGETEVENTS, &val ) < 0 )
            syslog( LOG_WARNING, "[linsys-sdiaudio] could not SDIAUDIO_IOC_RXGETEVENTS %s", strerror( errno ) );
        else
        {
            if( val & SDIAUDIO_EVENT_RX_BUFFER )
                syslog( LOG_WARNING, "[linsys-sdiaudio] driver receive buffer queue overrun \n" );
            if( val & SDIAUDIO_EVENT_RX_FIFO )
                syslog( LOG_WARNING, "[linsys-sdiaudio] onboard receive FIFO overrun \n");
            if( val & SDIAUDIO_EVENT_RX_CARRIER )
                syslog( LOG_WARNING, "[linsys-sdiaudio] carrier status change \n");
            if( val & SDIAUDIO_EVENT_RX_DATA )
                syslog( LOG_WARNING, "[linsys-sdiaudio] data status change");
        }
    }

    if( pfd[0].revents & POLLIN )
    {
        if( ioctl( linsys_ctx->vfd, SDIVIDEO_IOC_DQBUF, linsys_ctx->current_vbuffer ) < 0 )
        {
            syslog( LOG_WARNING, "[linsys-sdivideo] couldn't SDIVIDEO_IOC_DQBUF %s", strerror( errno ) );
            return -1;
        }

        if( handle_video_frame( linsys_opts, linsys_ctx->vbuffers[linsys_ctx->current_vbuffer] ) < 0 )
            return -1;

        if( ioctl( linsys_ctx->vfd, SDIVIDEO_IOC_QBUF, linsys_ctx->current_vbuffer ) < 0 )
        {
            syslog( LOG_WARNING, "[linsys-sdivideo] couldn't SDIVIDEO_IOC_QBUF %s", strerror( errno ) );
            return -1;
        }

        linsys_ctx->current_vbuffer++;
        linsys_ctx->current_vbuffer %= linsys_ctx->num_vbuffers;
    }

    if( pfd[1].revents & POLLIN  )
    {
        if( ioctl( linsys_ctx->afd, SDIAUDIO_IOC_DQBUF, linsys_ctx->current_abuffer ) < 0 )
        {
            syslog( LOG_WARNING, "[linsys-sdiaudio] couldn't SDIAUDIO_IOC_DQBUF %s", strerror( errno ) );
            return -1;
        }

        if( !linsys_opts->probe && handle_audio_frame( linsys_opts, linsys_ctx->abuffers[linsys_ctx->current_abuffer] ) < 0 )
            return -1;

        if( ioctl( linsys_ctx->afd, SDIAUDIO_IOC_QBUF, linsys_ctx->current_abuffer ) < 0 )
        {
            syslog( LOG_WARNING, "[linsys-sdiaudio] couldn't SDIAUDIO_IOC_QBUF %s", strerror( errno ) );
            return -1;
        }

        linsys_ctx->current_abuffer++;
        linsys_ctx->current_abuffer %= linsys_ctx->num_abuffers;
    }

    return 0;
}

static int open_card( linsys_opts_t *linsys_opts )
{
    linsys_ctx_t *linsys_ctx = &linsys_opts->linsys_ctx;
    int          ret = 0, i, aligned_width, cpu_flags;
    const int    page_size = getpagesize();
    unsigned int bufmemsize, sample_rate;
    unsigned long int vanc;
    char vdev[MAXLEN], adev[MAXLEN], vanc_file[MAXLEN];

    /* TODO: get rid of linsys utilities */

    snprintf( vdev, sizeof(vdev), SDIVIDEO_DEVICE, linsys_opts->card_idx );
    vdev[sizeof(vdev) - 1] = '\0';
    if ( (linsys_ctx->vfd = open( vdev, O_RDONLY )) < 0 )
    {
        fprintf( stderr, "[linsys-sdivideo] couldn't open device %s \n", vdev );
        goto finish;
    }

    /* Wait for standard to settle down */
    while ( 1 )
    {
        struct pollfd pfd[1];

        pfd[0].fd = linsys_ctx->vfd;
        pfd[0].events = POLLIN | POLLPRI;

        if( poll( pfd, 1, READ_TIMEOUT ) < 0 )
        {
            fprintf( stderr, "[linsys-sdivideo] could not poll(): %s \n", vdev );
            ret = -1;
            goto finish;
        }

        if( pfd[0].revents & POLLPRI )
        {
            unsigned int val;

            if( ioctl( linsys_ctx->vfd, SDIVIDEO_IOC_RXGETEVENTS, &val ) < 0 )
                syslog( LOG_WARNING, "[linsys-sdivideo] could not SDIVIDEO_IOC_RXGETEVENTS %s", strerror( errno ) );
            else
            {
                if( val & SDIVIDEO_EVENT_RX_BUFFER )
                    syslog( LOG_WARNING, "[linsys-sdivideo] driver receive buffer queue overrun \n" );
                if( val & SDIVIDEO_EVENT_RX_FIFO )
                    syslog( LOG_WARNING, "[linsys-sdivideo] onboard receive FIFO overrun \n");
                if( val & SDIVIDEO_EVENT_RX_CARRIER )
                    syslog( LOG_WARNING, "[linsys-sdivideo] carrier status change \n");
                if( val & SDIVIDEO_EVENT_RX_DATA )
                    syslog( LOG_WARNING, "[linsys-sdivideo] data status change \n");
                if( val & SDIVIDEO_EVENT_RX_STD )
                {
                    syslog( LOG_WARNING, "[linsys-sdivideo] format change \n");
                    break;
                }
            }
        }
    }

    if( ioctl( linsys_ctx->vfd, SDIVIDEO_IOC_RXGETVIDSTATUS, &linsys_ctx->standard ) < 0 )
    {
        fprintf( stderr, "[linsys-sdivideo] could not SDIVIDEO_IOC_RXGETVIDSTATUS %s", strerror( errno ) );
        ret = -1;
        goto finish;
    }

    for( i = 0; video_format_tab[i].obe_name != -1; i++ )
    {
        if( video_format_tab[i].linsys_name == linsys_ctx->standard )
            break;
    }

    if( video_format_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[linsys-sdivideo] Unsupported video format\n" );
        ret = -1;
        goto finish;
    }

    linsys_opts->video_format = video_format_tab[i].obe_name;
    linsys_opts->width = linsys_ctx->width = video_format_tab[i].width;
    linsys_opts->height = linsys_ctx->coded_height = video_format_tab[i].height;
    /* Ignore any 6 junk lines */
    if( linsys_opts->video_format == INPUT_VIDEO_FORMAT_NTSC )
        linsys_opts->height = 480;

    linsys_opts->timebase_num = video_format_tab[i].timebase_num;
    linsys_opts->timebase_den = video_format_tab[i].timebase_den;
    linsys_opts->interlaced = IS_INTERLACED( linsys_opts->video_format );
    if( linsys_opts->interlaced )
        linsys_opts->tff = video_format_tab[i].tff;

    linsys_ctx->v_timebase.num = linsys_opts->timebase_num;
    linsys_ctx->v_timebase.den = linsys_opts->timebase_den;

    aligned_width = ((linsys_opts->width + 47) / 48) * 48;
    linsys_ctx->stride = aligned_width * 8 / 3;
    linsys_ctx->vbuffer_size = linsys_ctx->coded_height * linsys_ctx->stride;

    cpu_flags = av_get_cpu_flags();

    /* Setup unpack functions */
    linsys_ctx->unpack_line = obe_v210_planar_unpack_c;

    if( cpu_flags & AV_CPU_FLAG_SSSE3 )
        linsys_ctx->unpack_line = obe_v210_planar_unpack_aligned_ssse3;

    if( cpu_flags & AV_CPU_FLAG_AVX )
        linsys_ctx->unpack_line = obe_v210_planar_unpack_aligned_avx;

    /* Setup VBI and VANC pack functions */
    if( IS_SD( linsys_opts->video_format ) )
    {
        linsys_ctx->pack_line = obe_yuv422p10_line_to_uyvy_c;
        linsys_ctx->downscale_line = obe_downscale_line_c;

        if( cpu_flags & AV_CPU_FLAG_MMX )
            linsys_ctx->downscale_line = obe_downscale_line_mmx;

        if( cpu_flags & AV_CPU_FLAG_SSE2 )
            linsys_ctx->downscale_line = obe_downscale_line_sse2;
    }
    else
        linsys_ctx->pack_line = obe_yuv422p10_line_to_nv20_c;

    close( linsys_ctx->vfd );

    /* First open the audio for synchronization reasons */
    snprintf( adev, sizeof(adev), SDIAUDIO_DEVICE, linsys_opts->card_idx );
    adev[sizeof(adev) - 1] = '\0';

    if( (linsys_ctx->afd = open( adev, O_RDONLY )) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not open device %s \n", adev );
        ret = -1;
        goto finish;
    }

    if( ioctl( linsys_ctx->afd, SDIAUDIO_IOC_RXGETAUDRATE, &sample_rate ) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not SDIAUDIO_IOC_RXGETAUDRATE %s", strerror( errno ) );
        ret = -1;
        goto finish;
    }

    /* TODO: support non 48kHz sample rates */
    switch ( sample_rate )
    {
        case SDIAUDIO_CTL_ASYNC_48_KHZ:
        case SDIAUDIO_CTL_SYNC_48_KHZ:
            linsys_opts->sample_rate = 48000;
            break;
        default:
            fprintf( stderr, "[linsys-sdiaudio] unsupported sample rate \n");
            ret = -1;
            goto finish;
    }

    linsys_ctx->a_timebase.num = 1;
    linsys_ctx->a_timebase.den = linsys_opts->sample_rate;

    close( linsys_ctx->afd );

    /* Use 32-bit audio */
    if( write_ul_sysfs( SDIAUDIO_SAMPLESIZE_FILE, linsys_opts->card_idx, SDIAUDIO_CTL_AUDSAMP_SZ_32 ) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not write to SDIAUDIO_SAMPLESIZE_FILE \n");
        ret = -1;
        goto finish;
    }

    /* TODO: support multichannel */
    if( write_ul_sysfs( SDIAUDIO_CHANNELS_FILE, linsys_opts->card_idx, SDIAUDIO_CTL_AUDCH_EN_8 ) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not write to SDIAUDIO_CHANNELS_FILE \n");
        ret = -1;
        goto finish;
    }

    linsys_ctx->abuffer_size = linsys_opts->audio_samples * linsys_opts->num_channels * sizeof(int32_t);
    linsys_ctx->num_abuffers = NB_ABUFFERS;

    if( write_ul_sysfs( SDIAUDIO_BUFFERS_FILE, linsys_opts->card_idx, linsys_ctx->num_abuffers ) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not write NB_ABUFFERS \n");
        ret = -1;
        goto finish;
    }

    if( write_ul_sysfs( SDIAUDIO_BUFSIZE_FILE, linsys_opts->card_idx, linsys_ctx->abuffer_size ) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] could not write audio buffer size \n");
        ret = -1;
        goto finish;
    }

    if( !linsys_opts->probe )
    {
        linsys_ctx->avr = avresample_alloc_context();
        if( !linsys_ctx->avr )
        {
            fprintf( stderr, "[linsys-sdiaudio] couldn't setup sample rate conversion \n" );
            ret = -1;
            goto finish;
        }

        /* Give libavresample a made up channel map */
        av_opt_set_int( linsys_ctx->avr, "in_channel_layout",   (1 << linsys_opts->num_channels) - 1, 0 );
        av_opt_set_int( linsys_ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
        av_opt_set_int( linsys_ctx->avr, "in_sample_rate",      48000, 0 );
        av_opt_set_int( linsys_ctx->avr, "out_channel_layout",  (1 << linsys_opts->num_channels) - 1, 0 );
        av_opt_set_int( linsys_ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );

        if( avresample_open( linsys_ctx->avr ) < 0 )
        {
            fprintf( stderr, "Could not open AVResample\n" );
            goto finish;
        }
    }

    if( (linsys_ctx->afd = open( adev, O_RDONLY )) < 0 )
    {
        fprintf( stderr, "[linsys-sdiaudio] couldn't open device %s \n", adev );
        ret = -1;
        goto finish;
    }

    linsys_ctx->current_abuffer = 0;
    bufmemsize = ((linsys_ctx->abuffer_size + page_size - 1) / page_size) * page_size;

    linsys_ctx->abuffers = malloc( linsys_ctx->num_abuffers * sizeof(uint8_t*) );
    if( !linsys_ctx->abuffers )
    {
        fprintf( stderr, "malloc failed \n" );
        ret = -1;
        goto finish;
    }

    for( unsigned int j = 0; j < linsys_ctx->num_abuffers; j++ )
    {
        if( (linsys_ctx->abuffers[j] = mmap( NULL, linsys_ctx->abuffer_size, PROT_READ, MAP_SHARED,
                                             linsys_ctx->afd, j * bufmemsize )) == MAP_FAILED )
        {
            fprintf( stderr, "could not mmap audio buffer %u \n", j );
            ret = -1;
            goto finish;
        }
    }

    /* Use v210 pixel format */
    if( write_ul_sysfs( SDIVIDEO_MODE_FILE, linsys_opts->card_idx, SDIVIDEO_CTL_MODE_V210 ) < 0 )
    {
        fprintf( stderr, "[linsys-sdi] could not write SDIVIDEO_CTL_MODE_V210 \n");
        ret = -1;
        goto finish;
    }

    /* Some cards don't have VANC support so don't complain if we can't read or write */
    write_ul_sysfs( SDIVIDEO_VANC_FILE, linsys_opts->card_idx, 1 );
    snprintf( vanc_file, sizeof(vanc_file), SDIVIDEO_VANC_FILE, linsys_opts->card_idx );

    if( util_strtoul( vanc_file, &vanc ) > 0 )
        linsys_ctx->has_vanc = !!vanc;

    /* Increase the buffer size if VANC is being included and make the v210 decoder act on the full frame */
    if( linsys_ctx->has_vanc )
    {
        linsys_ctx->vbuffer_size += (video_format_tab[i].total_height - video_format_tab[i].height) * linsys_ctx->stride;
        linsys_ctx->coded_height = video_format_tab[i].total_height;
    }

    linsys_ctx->num_vbuffers = NB_VBUFFERS;

    if( write_ul_sysfs( SDIVIDEO_BUFFERS_FILE, linsys_opts->card_idx, linsys_ctx->num_vbuffers ) < 0 )
    {
        fprintf( stderr, "[linsys-sdi] could not write NB_VBUFFERS \n");
        ret = -1;
        goto finish;
    }

    if( write_ul_sysfs( SDIVIDEO_BUFSIZE_FILE, linsys_opts->card_idx, linsys_ctx->vbuffer_size ) < 0 )
    {
        fprintf( stderr, "[linsys-sdi] could not write video buffer size \n");
        ret = -1;
        goto finish;
    }

    if( (linsys_ctx->vfd = open( vdev, O_RDONLY )) < 0 )
    {
        fprintf( stderr, "[linsys-sdi] couldn't open device %s \n", vdev );
        ret = -1;
        goto finish;
    }

    linsys_ctx->current_vbuffer = 0;
    bufmemsize = ((linsys_ctx->vbuffer_size + page_size - 1) / page_size) * page_size;

    linsys_ctx->vbuffers = malloc( linsys_ctx->num_vbuffers * sizeof(uint8_t*) );
    if( !linsys_ctx->vbuffers )
    {
        fprintf( stderr, "malloc failed \n" );
        ret = -1;
        goto finish;
    }

    for( unsigned int j = 0; j < linsys_ctx->num_vbuffers; j++ )
    {
        if( (linsys_ctx->vbuffers[j] = mmap( NULL, linsys_ctx->vbuffer_size, PROT_READ, MAP_SHARED,
                                             linsys_ctx->vfd, j * bufmemsize )) == MAP_FAILED )
        {
            fprintf( stderr, "could not mmap video buffer %u \n", j );
            ret = -1;
            goto finish;
        }
    }

finish:
    if( ret )
        close_card( linsys_opts );

    return ret;
}

static void close_thread( void *handle )
{
    struct linsys_status *status = handle;

    if( status->linsys_opts )
    {
        close_card( status->linsys_opts );
        free( status->linsys_opts );
    }

    free( status->input );
}

static void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int num_streams = 0, vbi_stream_services = 0;
    obe_sdi_non_display_data_t *non_display_parser;

    linsys_opts_t linsys_opts;
    memset( &linsys_opts, 0, sizeof(linsys_opts_t) );
    non_display_parser = &linsys_opts.linsys_ctx.non_display_parser;
    linsys_opts.linsys_ctx.h = h;
    linsys_opts.linsys_ctx.last_frame_time = -1;
    linsys_opts.probe = non_display_parser->probe = 1;

    linsys_opts.num_channels = 8;
    linsys_opts.audio_samples = 2000; /* not important yet when probing */

    if( open_card( &linsys_opts ) < 0 )
        return NULL;

    int64_t start = obe_mdate();
    while( 1 )
    {
        capture_data( &linsys_opts );
        if( obe_mdate() - start >= 1000000 )
            break;
    }

    close_card( &linsys_opts );

    // TODO add support for DVB teletext

    for( int i = 0; i < non_display_parser->num_frame_data; i++ )
    {
        if( non_display_parser->frame_data[i].location == USER_DATA_LOCATION_DVB_STREAM )
            vbi_stream_services++;
    }

    num_streams = 2+!!vbi_stream_services;
    for( int i = 0; i < num_streams; i++ )
    {
        streams[i] = calloc( 1, sizeof(*streams[i]) );
        if( !streams[i] )
            goto finish;

        /* TODO: make it take a continuous set of stream-ids */
        pthread_mutex_lock( &h->device_list_mutex );
        streams[i]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        if( i == 0 )
        {
            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->width  = linsys_opts.width;
            streams[i]->height = linsys_opts.height;
            streams[i]->timebase_num = linsys_opts.timebase_num;
            streams[i]->timebase_den = linsys_opts.timebase_den;
            streams[i]->csp    = PIX_FMT_YUV422P10;
            streams[i]->interlaced = linsys_opts.interlaced;
            streams[i]->tff = linsys_opts.tff;
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */

            if( add_non_display_services( non_display_parser, streams[i], USER_DATA_LOCATION_FRAME ) < 0 )
                goto finish;
        }
        else if( i == 1 )
        {
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels = 8;
            streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
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
    device->device_type = INPUT_DEVICE_LINSYS_SDI;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );

    /* add device */
    add_device( h, device );

finish:
    free( probe_ctx );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    obe_input_t *user_opts = &device->user_opts;
    linsys_opts_t *linsys_opts;
    linsys_ctx_t *linsys_ctx;
    obe_sdi_non_display_data_t *non_display_parser;
    struct linsys_status status;

    linsys_opts = calloc( 1, sizeof(*linsys_opts) );
    if( !linsys_opts )
    {
        fprintf( stderr, "malloc failed \n" );
        return NULL;
    }

    status.input = input;
    status.linsys_opts = linsys_opts;
    pthread_cleanup_push( close_thread, (void*)&status );

    linsys_opts->num_channels = 8;
    linsys_opts->card_idx = user_opts->card_idx;
    linsys_opts->audio_samples = input->audio_samples;

    linsys_ctx = &linsys_opts->linsys_ctx;

    linsys_ctx->device = device;
    linsys_ctx->h = h;
    linsys_ctx->last_frame_time = -1;

    non_display_parser = &linsys_ctx->non_display_parser;
    non_display_parser->device = device;

    /* TODO: wait for encoder */

    if( open_card( linsys_opts ) < 0 )
        return NULL;

    while( 1 )
    {
        if( capture_data( linsys_opts ) < 0 )
            break;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_input_func_t linsys_sdi_input = { probe_stream, open_input };

