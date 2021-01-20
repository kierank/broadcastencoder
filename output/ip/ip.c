/*****************************************************************************
 * ip.c : IP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Large Portions of this code originate from FFmpeg
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

#include <libavutil/random_seed.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/fifo.h>

#include "common/common.h"
#include "output/output.h"
#include "common/bitstream.h"
#include "udp.h"
#include "arq.h"


#include <bitstream/ietf/rtp.h>
#include <bitstream/ietf/rtp3551.h>
#include <bitstream/mpeg/ts.h>
#include <bitstream/smpte/2022_1_fec.h>

#include <upipe/uref.h>

#define FEC_PAYLOAD_TYPE 96

#define RTP_PACKET_SIZE (RTP_HEADER_SIZE+TS_PACKETS_SIZE)
#define COP3_FEC_PACKET_SIZE (RTP_PACKET_SIZE+SMPTE_2022_FEC_HEADER_SIZE)

typedef struct
{
    hnd_t udp_handle;

    uint64_t seq;
    uint32_t ssrc;

    uint32_t pkt_cnt;
    uint32_t octet_cnt;

    /* Stream Duplication */
    AVFifoBuffer *dup_fifo;
    int dup_delay;

    bool arq;
    unsigned arq_latency;

    /* COP3 FEC */
    hnd_t column_handle;
    hnd_t row_handle;

    /* RTCP */
    hnd_t rtcp_handle;

    int fec_type;
    int fec_columns;
    int fec_rows;
    int column_phase;

    int fec_pkt_len;
    uint8_t *column_data;
    uint8_t *row_data;

    uint64_t column_seq;
    uint64_t row_seq;

    struct arq_ctx *arq_ctx;
} obe_rtp_ctx;

struct ip_status
{
    obe_output_t *output;
    hnd_t *ip_handle;
    struct uchain *queue;
};

static void xor_packet_c( uint8_t *dst, uint8_t *src, unsigned len )
{
    for( int i = 0; i < len; i++ )
        dst[i] ^= src[i];
}

static void rtp_close( hnd_t handle )
{
    obe_rtp_ctx *p_rtp = handle;
    AVBufferRef *output_buffer;

    if (p_rtp->arq)
        close_arq(p_rtp->arq_ctx);

    udp_close( p_rtp->udp_handle );

    if( p_rtp->dup_fifo )
    {
        while( av_fifo_size( p_rtp->dup_fifo ) > 0 )
        {
            av_fifo_drain( p_rtp->dup_fifo, sizeof(int64_t) );
            av_fifo_generic_read( p_rtp->dup_fifo, &output_buffer, sizeof(output_buffer), NULL );
            av_buffer_unref( &output_buffer );
        }

        av_fifo_freep( &p_rtp->dup_fifo );
    }

    /* COP3 FEC */
    free( p_rtp->column_data );
    free( p_rtp->row_data );
    if( p_rtp->column_handle )
        udp_close( p_rtp->column_handle );
    if( p_rtp->row_handle )
        udp_close( p_rtp->row_handle );
    if( p_rtp->rtcp_handle )
        udp_close( p_rtp->rtcp_handle );

    free( p_rtp );
}

static int rtp_open( hnd_t *p_handle, obe_udp_opts_t *udp_opts, obe_output_dest_t *output_dest )
{
    obe_rtp_ctx *p_rtp = calloc( 1, sizeof(*p_rtp) );
    if( !p_rtp )
    {
        fprintf( stderr, "[rtp] malloc failed \n" );
        return -1;
    }

    if( !udp_opts->local_port )
        udp_opts->local_port = udp_opts->port;
    udp_opts->reuse_socket = 1;
    if( udp_open( &p_rtp->udp_handle, udp_opts, -1 ) < 0 )
    {
        fprintf( stderr, "[rtp] Could not create udp output \n" );
        goto error;
    }

    p_rtp->ssrc = av_get_random_seed();

    p_rtp->dup_delay = output_dest->dup_delay;
    p_rtp->arq = output_dest->type == OUTPUT_ARQ;
    p_rtp->arq_latency = output_dest->arq_latency;
    if (!p_rtp->arq_latency)
        p_rtp->arq_latency = 100;
    p_rtp->fec_columns = output_dest->fec_columns;
    p_rtp->fec_rows = output_dest->fec_rows;

    obe_udp_ctx *s = p_rtp->udp_handle;
    int fd = s->udp_fd;

    int rtcp_port = udp_opts->port + 1;

    if( p_rtp->fec_columns && p_rtp->fec_rows )
    {
        p_rtp->fec_pkt_len = FFALIGN( COP3_FEC_PACKET_SIZE, 32 );
        p_rtp->column_data = calloc( output_dest->fec_columns*2, p_rtp->fec_pkt_len );
        p_rtp->row_data = calloc( output_dest->fec_rows, p_rtp->fec_pkt_len );
        if( !p_rtp->column_data || !p_rtp->row_data )
        {
            fprintf( stderr, "[rtp] malloc failed \n" );
            goto error;
        }

        udp_opts->port += 2;
        if( udp_open( &p_rtp->column_handle, udp_opts, fd ) < 0 )
        {
            fprintf( stderr, "[rtp] Could not create FEC column output \n" );
            goto error;
        }

        udp_opts->port += 2;
        if( udp_open( &p_rtp->row_handle, udp_opts, fd ) < 0 )
        {
            fprintf( stderr, "[rtp] Could not create FEC row output \n" );
            goto error;
        }
    }

    if (p_rtp->arq) {
        p_rtp->ssrc &= ~1;

        udp_opts->local_port = udp_opts->port = rtcp_port;
        if( udp_open( &p_rtp->rtcp_handle, udp_opts, -1) < 0 )
        {
            fprintf( stderr, "[rtp] Could not create RTCP output \n" );
            goto error;
        }
    }

    if( output_dest->dup_delay )
    {
        p_rtp->dup_fifo = av_fifo_alloc( sizeof(AVBufferRef) * 100 );
        if( !p_rtp->dup_fifo )
        {
            fprintf( stderr, "[rtp-dup] Could not allocate data fifo" );
            goto error;
        }
    }

    *p_handle = p_rtp;

    return 0;

error:
    rtp_close(p_rtp);

    return -1;
}

static void write_rtp_header( uint8_t *data, uint8_t payload_type, uint16_t seq, uint32_t ts_90, uint32_t ssrc )
{
    uint8_t ssrc_p[4] = {
        (ssrc >> 24) & 0xff,
        (ssrc >> 16) & 0xff,
        (ssrc >>  8) & 0xff,
        (ssrc      ) & 0xff,
    };

    rtp_set_hdr(data);
    rtp_set_type(data, payload_type);
    rtp_set_seqnum(data, seq);
    rtp_set_timestamp(data, ts_90);
    rtp_set_ssrc(data, ssrc_p);
}

static void write_fec_header( obe_rtp_ctx *p_rtp, uint8_t *rtp, int row, uint16_t snbase )
{
    uint8_t *data = &rtp[RTP_HEADER_SIZE];

    uint16_t length_recovery;
    uint8_t payload_recovery;

    if( row )
    {
        length_recovery = p_rtp->fec_columns & 1 ? TS_PACKETS_SIZE : 0;
        payload_recovery = p_rtp->fec_columns & 1 ? RTP_TYPE_MP2T : 0;
    }
    else
    {
        length_recovery = p_rtp->fec_rows & 1 ? TS_PACKETS_SIZE : 0;
        payload_recovery = p_rtp->fec_rows & 1 ? RTP_TYPE_MP2T : 0;
    }

    memset(data, 0, SMPTE_2022_FEC_HEADER_SIZE);
    smpte_fec_set_snbase_low(data, snbase);
    smpte_fec_set_length_rec(data, length_recovery);
    smpte_fec_set_extension(data);
    smpte_fec_set_pt_recovery(data, payload_recovery);
    if (row) {
        smpte_fec_set_d(data);
        smpte_fec_set_offset(data, 1);
        smpte_fec_set_na(data, p_rtp->fec_columns);
    } else {
        smpte_fec_set_offset(data, p_rtp->fec_columns);
        smpte_fec_set_na(data, p_rtp->fec_rows);
    }
    smpte_fec_set_snbase_ext(data, 0);
}

static int write_fec_packet(obe_rtp_ctx *p_rtp, hnd_t udp_handle, uint8_t *data,
        int64_t timestamp, struct uref **uref)
{
    int ret = 0;
    const size_t len = COP3_FEC_PACKET_SIZE;

    if (p_rtp->arq) {
        *uref = make_uref(p_rtp->arq_ctx, data, len, timestamp);
        if (!*uref)
            ret = -1;
    } else {
        if( udp_write( udp_handle, data, len) < 0 )
            ret = -1;
    }

    memset( data, 0, len);

    return ret;
}

static int dup_stream(obe_rtp_ctx *p_rtp, AVBufferRef *buf_ref,
        int64_t timestamp)
{
    AVFifoBuffer *dup_fifo = p_rtp->dup_fifo;
    AVBufferRef *output_buffer = av_buffer_ref( buf_ref );
    if( !output_buffer )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    if( av_fifo_grow( dup_fifo, av_fifo_size( dup_fifo ) + sizeof(*output_buffer) ) < 0 )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }

    av_fifo_generic_write( dup_fifo, &timestamp, sizeof(timestamp), NULL );
    av_fifo_generic_write( dup_fifo, &output_buffer, sizeof(output_buffer), NULL );

    while( av_fifo_size( dup_fifo ) >= sizeof(timestamp) + sizeof(output_buffer) )
    {
        uint8_t *peek = av_fifo_peek2( dup_fifo, 0 );
        int64_t ts_dup = AV_RL64( peek );
        if( ts_dup + (p_rtp->dup_delay * (OBE_CLOCK/1000)) >= timestamp )
            break;

        av_fifo_drain( dup_fifo, sizeof(timestamp) );
        av_fifo_generic_read( dup_fifo, &output_buffer, sizeof(output_buffer), NULL );

        bool ok = !udp_write( p_rtp->udp_handle, output_buffer->data, RTP_PACKET_SIZE );

        av_buffer_unref( &output_buffer );
        if (!ok)
            return -1;
    }

    return 0;
}

static int fec(obe_rtp_ctx *p_rtp, int fec_type, uint8_t *pkt_ptr,
        uint64_t timestamp, struct uref **row_uref, struct uref **col_uref)
{
    int ret = 0;
    const int rows = p_rtp->fec_rows;
    const int columns = p_rtp->fec_columns;
    const uint64_t seq = p_rtp->seq;
    uint32_t ts_90 = timestamp / 300;
    int row_idx = (seq / columns) % rows;
    int column_idx = seq % columns;

    if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED && row_idx == 0 && column_idx == 0 && seq >= columns*rows )
    {
        p_rtp->column_phase ^= 1;
    }

    uint8_t *row = &p_rtp->row_data[row_idx*p_rtp->fec_pkt_len];
    uint8_t *column = &p_rtp->column_data[(column_idx*2+p_rtp->column_phase)*p_rtp->fec_pkt_len];

    smpte_fec_set_ts_recovery(&row[RTP_HEADER_SIZE], ts_90);
    xor_packet_c( &row[RTP_HEADER_SIZE+SMPTE_2022_FEC_HEADER_SIZE], &pkt_ptr[RTP_HEADER_SIZE], TS_PACKETS_SIZE );

    if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED )
    {
        smpte_fec_set_ts_recovery(&column[RTP_HEADER_SIZE], ts_90);
        xor_packet_c( &column[RTP_HEADER_SIZE+SMPTE_2022_FEC_HEADER_SIZE], &pkt_ptr[RTP_HEADER_SIZE], TS_PACKETS_SIZE );
    }

    /* Check if we can send packets. Start with rows to match the suggestion in the ProMPEG spec */
    if( column_idx == (columns-1) )
    {
        write_rtp_header( row, FEC_PAYLOAD_TYPE, p_rtp->row_seq++, 0, 0 );
        write_fec_header( p_rtp, row, 1, seq - column_idx );

        if( write_fec_packet( p_rtp, p_rtp->row_handle, row, timestamp, row_uref ) < 0 )
            ret = -1;
    }

    if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED) {
        /* Pre-write the RTP and FEC header */
        if( row_idx == (rows-1) )
        {
            write_rtp_header( column, FEC_PAYLOAD_TYPE, p_rtp->column_seq++, 0, 0 );
            write_fec_header( p_rtp, column, 0, seq - (columns*(rows-1)) );
        }

        /* Interleave the column FEC data from the previous matrix */
        if( seq >= columns*rows && seq % rows == 0 )
        {
            uint64_t send_column_idx = (seq % (columns*rows)) / rows;
            uint8_t *column_tx = &p_rtp->column_data[(send_column_idx*2+!p_rtp->column_phase)*p_rtp->fec_pkt_len];

            if( write_fec_packet( p_rtp, p_rtp->column_handle, column_tx, timestamp, col_uref ) < 0 )
                ret = -1;
        }
    } else if( fec_type == FEC_TYPE_COP3_NON_BLOCK_ALIGNED ) {
        if( seq >= (columns * rows + column_idx*(columns+1)) )
        {
            int deoffsetted_seq = (seq - column_idx) - (column_idx*columns);
            if( deoffsetted_seq % ( columns * rows ) == 0 )
            {
                write_rtp_header( column, FEC_PAYLOAD_TYPE, p_rtp->column_seq++, 0, 0 );
                write_fec_header( p_rtp, column, 0, seq - (columns*rows) );

                if( write_fec_packet( p_rtp, p_rtp->column_handle, column, timestamp, col_uref ) < 0 )
                    ret = -1;
            }
        }

        if( seq >= column_idx*(columns+1) )
        {
            smpte_fec_set_ts_recovery(&column[RTP_HEADER_SIZE], ts_90);
            xor_packet_c( &column[RTP_HEADER_SIZE+SMPTE_2022_FEC_HEADER_SIZE], &pkt_ptr[RTP_HEADER_SIZE], TS_PACKETS_SIZE );
        }
    }

    return ret;
}

static int write_rtp_pkt( hnd_t handle, uint8_t *data, int len, int64_t timestamp, int fec_type )
{
    obe_rtp_ctx *p_rtp = handle;
    int ret = 0;
    uint8_t *pkt_ptr;

    /* Throughout this function, don't exit early because the decoder is expecting a sequence number increase
     * and consistent FEC packets. Return -1 at the end so the user knows there was a failure to submit a packet. */
    AVBufferRef *buf_ref = av_buffer_alloc( RTP_PACKET_SIZE );
    pkt_ptr = buf_ref->data;

    uint32_t ts_90 = timestamp / 300;
    write_rtp_header( pkt_ptr, RTP_TYPE_MP2T, p_rtp->seq, ts_90, p_rtp->ssrc );
    memcpy( &pkt_ptr[RTP_HEADER_SIZE], data, len );

    struct uref *uref = NULL;
    if (p_rtp->arq) {
        uref = make_uref(p_rtp->arq_ctx, pkt_ptr, RTP_PACKET_SIZE, timestamp);
        if (!uref)
            ret = -1;
    } else {
        if( udp_write( p_rtp->udp_handle, pkt_ptr, RTP_PACKET_SIZE ) < 0 )
            ret = -1;
    }

    /* Check and send duplicate packets */
    if( p_rtp->dup_fifo ) {
        if (dup_stream(p_rtp, buf_ref, timestamp))
            ret = -1;
        goto end;
    }

    struct uref *row_uref = NULL, *col_uref = NULL;

    if( p_rtp->fec_columns && p_rtp->fec_rows )
        ret |= fec(p_rtp, fec_type, pkt_ptr, timestamp, &row_uref, &col_uref);

    if (p_rtp->arq) {
        if (!uref) {
            uref_free(row_uref);
            uref_free(col_uref);
            goto end;
        }

        struct uchain **next = &uref->uchain.next;
        if (row_uref) {
            row_uref->priv = 1;
            *next = &row_uref->uchain;
            next = &row_uref->uchain.next;
        }
        if (col_uref) {
            *next = &col_uref->uchain;
        }

        arq_write(p_rtp->arq_ctx, uref);
    }

end:
    av_buffer_unref( &buf_ref );

    p_rtp->seq++;
    p_rtp->pkt_cnt++;
    p_rtp->octet_cnt += len;

    return ret;
}

static void close_output(struct ip_status *status)
{
    if( *status->ip_handle ) {
        if( status->output->output_dest.type == OUTPUT_RTP ||
                status->output->output_dest.type == OUTPUT_ARQ)
            rtp_close( *status->ip_handle );
        else
            udp_close( *status->ip_handle );
    }

    free( status->output->output_dest.target );
}

static void *open_output( void *ptr )
{
    obe_output_t *output = ptr;
    obe_output_dest_t *output_dest = &output->output_dest;
    struct ip_status status;
    hnd_t ip_handle = NULL;
    obe_udp_opts_t udp_opts;
    struct uchain queue;
    ulist_init( &queue );
    struct uchain *uchain, *uchain_tmp;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    status.output = output;
    status.ip_handle = &ip_handle;
    status.queue = &queue;

    udp_populate_opts( &udp_opts, output_dest->target );

    if( output_dest->type == OUTPUT_RTP ||
            output_dest->type == OUTPUT_ARQ )
    {
        if( rtp_open( &ip_handle, &udp_opts, output_dest ) < 0 )
            return NULL;

        obe_rtp_ctx *p_rtp = ip_handle;
        if (p_rtp->arq) {
            obe_udp_ctx *p_udp = p_rtp->udp_handle;
            p_rtp->arq_ctx = open_arq(p_udp, p_rtp->row_handle,
                    p_rtp->column_handle,  p_rtp->rtcp_handle, p_rtp->arq_latency);
            if (!p_rtp->arq_ctx) {
                rtp_close(p_rtp);
                fprintf( stderr, "[rtp] Could not create arq output" );
                return NULL;
            }

            output->handle = p_rtp;
        }
    }
    else
    {
        if( udp_open( &ip_handle, &udp_opts, -1 ) < 0 )
        {
            fprintf( stderr, "[udp] Could not create udp output" );
            return NULL;
        }
    }

    while(1)
    {
        bool end;
        pthread_mutex_lock( &output->queue.mutex );
        while( ulist_empty( &output->queue.ulist ) && !output->cancel_thread )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &output->queue.in_cv, &output->queue.mutex );
        }

        while( !ulist_empty( &output->queue.ulist ) )
        {
            struct uchain *out = ulist_pop( &output->queue.ulist );
            ulist_add( &queue, out );
        }

        end = output->cancel_thread;
        pthread_mutex_unlock(&output->queue.mutex);

        if (end)
            break;

//        printf("\n START %i \n", num_buf_refs );

        ulist_delete_foreach( &queue, uchain, uchain_tmp )
        {
            obe_buf_ref_t *buf_ref = obe_buf_ref_t_from_uchain( uchain );
            AVBufferRef *data_buf_ref = buf_ref->data_buf_ref;
            uint8_t *data = &data_buf_ref->data[7*sizeof(int64_t)];
            if( output_dest->type == OUTPUT_RTP ||
                    output_dest->type == OUTPUT_ARQ )
            {
                if( write_rtp_pkt( ip_handle, data, TS_PACKETS_SIZE, AV_RN64( data_buf_ref->data ), output_dest->fec_type ) < 0 )
                    syslog( LOG_ERR, "[rtp] Failed to write RTP packet\n" );
            }
            else
            {
                if( udp_write( ip_handle, data, TS_PACKETS_SIZE ) < 0 )
                    syslog( LOG_ERR, "[udp] Failed to write UDP packet\n" );
            }

            ulist_delete( uchain );
            av_buffer_unref( &data_buf_ref );
            av_buffer_unref( &buf_ref->self_buf_ref );
        }
    }

    close_output(&status);

    return NULL;
}

static int get_status( void *ptr )
{
    obe_output_t *output = ptr;
    obe_output_dest_t *output_dest = &output->output_dest;
    obe_rtp_ctx *p_rtp = output;

    if( !output )
        return 0;

    if( p_rtp->arq )
        return arq_bidirectional( p_rtp->arq_ctx );

    return 0;
}

const obe_output_func_t ip_output = { open_output, get_status };
