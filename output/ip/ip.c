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
#include <sys/time.h>

#include "common/common.h"
#include "common/network/network.h"
#include "common/network/udp/udp.h"
#include "output/output.h"
#include "common/bitstream.h"

#define RTP_VERSION 2

#define MPEG_TS_PAYLOAD_TYPE 33
#define FEC_PAYLOAD_TYPE 96

#define RTP_HEADER_SIZE 12
#define FEC_HEADER_SIZE 16
#define TS_OFFSET 8

#define RTP_PACKET_SIZE RTP_HEADER_SIZE+TS_PACKETS_SIZE
#define FEC_PACKET_SIZE RTP_PACKET_SIZE+FEC_HEADER_SIZE

#define RTCP_SR_PACKET_TYPE 200
#define RTCP_PACKET_SIZE 28

#define NTP_OFFSET 2208988800ULL
#define NTP_OFFSET_US (NTP_OFFSET * 1000000ULL)

typedef struct
{
    hnd_t udp_handle;

    uint8_t pkt[FFALIGN( RTP_PACKET_SIZE, 32 )];
    uint64_t seq;
    uint32_t ssrc;

    uint32_t pkt_cnt;
    uint32_t octet_cnt;

    /* FEC */
    hnd_t column_handle;
    hnd_t row_handle;

    int fec_type;
    int fec_columns;
    int fec_rows;
    int column_phase;

    int fec_pkt_len;
    uint8_t *column_data;
    uint8_t *row_data;

    uint64_t column_seq;
    uint64_t row_seq;

} obe_rtp_ctx;

struct ip_status
{
    obe_output_t *output;
    hnd_t *ip_handle;
};

static void xor_packet_c( uint8_t *dst, uint8_t *src, int len )
{
    for( int i = 0; i < len; i++ )
        dst[i] = src[i] ^ dst[i];
}

static int rtp_open( hnd_t *p_handle, obe_udp_opts_t *udp_opts, obe_output_dest_t *output_dest )
{
    obe_rtp_ctx *p_rtp = calloc( 1, sizeof(*p_rtp) );
    if( !p_rtp )
    {
        fprintf( stderr, "[rtp] malloc failed" );
        return -1;
    }

    if( !udp_opts->local_port )
        udp_opts->local_port = udp_opts->port;
    udp_opts->reuse_socket = 1;
    if( udp_open( &p_rtp->udp_handle, udp_opts ) < 0 )
    {
        fprintf( stderr, "[rtp] Could not create udp output" );
        return -1;
    }

    p_rtp->ssrc = av_get_random_seed();

    p_rtp->fec_columns = output_dest->fec_columns;
    p_rtp->fec_rows = output_dest->fec_rows;
    if( p_rtp->fec_columns && p_rtp->fec_rows )
    {
        /* Set SSRC for both streams to zero as per specification */
        p_rtp->ssrc = 0;
        p_rtp->fec_pkt_len = FFALIGN( FEC_PACKET_SIZE, 32 );
        p_rtp->column_data = calloc( output_dest->fec_columns*2, p_rtp->fec_pkt_len );
        p_rtp->row_data = calloc( output_dest->fec_rows, p_rtp->fec_pkt_len );
        if( !p_rtp->column_data || !p_rtp->row_data )
        {
            fprintf( stderr, "[rtp] malloc failed" );
            return -1;
        }

        udp_opts->port += 2;
        if( udp_open( &p_rtp->column_handle, udp_opts ) < 0 )
        {
            fprintf( stderr, "[rtp] Could not create FEC column output" );
            return -1;
        }

        udp_opts->port += 2;
        if( udp_open( &p_rtp->row_handle, udp_opts ) < 0 )
        {
            fprintf( stderr, "[rtp] Could not create FEC row output" );
            return -1;
        }
    }

    *p_handle = p_rtp;

    return 0;
}
#if 0
static int64_t obe_gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static uint64_t obe_ntp_time(void)
{
  return (obe_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}

static int write_rtcp_pkt( hnd_t handle )
{
    obe_rtp_ctx *p_rtp = handle;
    uint64_t ntp_time = obe_ntp_time();
    uint8_t pkt[100];
    bs_t s;
    bs_init( &s, pkt, RTCP_PACKET_SIZE );

    bs_write( &s, 2, RTP_VERSION ); // version
    bs_write1( &s, 0 );             // padding
    bs_write( &s, 5, 0 );           // reception report count
    bs_write( &s, 8, RTCP_SR_PACKET_TYPE ); // packet type
    bs_write( &s, 8, 6 );           // length (length in words - 1)
    bs_write32( &s, p_rtp->ssrc );  // ssrc
    bs_write32( &s, ntp_time / 1000000 ); // NTP timestamp, most significant word
    bs_write32( &s, ((ntp_time % 1000000) << 32) / 1000000 ); // NTP timestamp, least significant word
    bs_write32( &s, 0 );            // RTP timestamp FIXME
    bs_write32( &s, p_rtp->pkt_cnt ); // sender's packet count
    bs_write32( &s, p_rtp->octet_cnt ); // sender's octet count
    bs_flush( &s );

    if( udp_write( p_rtp->udp_handle, pkt, RTCP_PACKET_SIZE ) < 0 )
        return -1;

    return 0;
}
#endif
static void write_rtp_header( uint8_t *data, uint8_t payload_type, uint16_t seq, uint32_t ts_90, uint32_t ssrc )
{
    *data++ = RTP_VERSION << 6;
    *data++ = payload_type & 0x7f;
    *data++ = seq >> 8;
    *data++ = seq & 0xff;
    *data++ = ts_90 >> 24;
    *data++ = (ts_90 >> 16) & 0xff;
    *data++ = (ts_90 >>  8) & 0xff;
    *data++ = ts_90 & 0xff;
    *data++ = ssrc >> 24;
    *data++ = (ssrc >> 16) & 0xff;
    *data++ = (ssrc >>  8) & 0xff;
    *data++ = ssrc & 0xff;
}

static void write_fec_header( hnd_t handle, uint8_t *data, int row, uint16_t snbase )
{
    obe_rtp_ctx *p_rtp = handle;

    uint16_t length_recovery;
    uint8_t payload_recovery;

    if( row )
    {
        length_recovery = p_rtp->fec_columns & 1 ? TS_PACKETS_SIZE : 0;
        payload_recovery = p_rtp->fec_columns & 1 ? MPEG_TS_PAYLOAD_TYPE : 0;
    }
    else
    {
        length_recovery = p_rtp->fec_rows & 1 ? TS_PACKETS_SIZE : 0;
        payload_recovery = p_rtp->fec_rows & 1 ? MPEG_TS_PAYLOAD_TYPE : 0;
    }

    *data++ = snbase >> 8;
    *data++ = snbase & 0xff;
    *data++ = length_recovery >> 8;
    *data++ = length_recovery & 0xff;
    *data++ = 1 << 7 | payload_recovery;
    /* marker */
    *data++ = 0;
    *data++ = 0;
    *data++ = 0;
     data   += 4; /* skip already written timestamp */
    *data++ = (row & 1) << 6;
    *data++ = row ? 1 : p_rtp->fec_columns;
    *data++ = row ? p_rtp->fec_columns : p_rtp->fec_rows;
    *data++ = 0; // SNBase ext bits
}

static int write_fec_packet( hnd_t udp_handle, uint8_t *data, int len )
{
    int ret = 0;

    if( udp_write( udp_handle, data, len ) < 0 )
        ret = -1;

    memset( data, 0, len );

    return ret;
}

static int write_rtp_pkt( hnd_t handle, uint8_t *data, int len, int64_t timestamp, int fec_type )
{
    obe_rtp_ctx *p_rtp = handle;
    int ret = 0;

    /* Throughout this function, don't exit early because the decoder is expecting a sequence number increase
     * and consistent FEC packets. Return -1 at the end so the user knows there was a failure to submit a packet. */

    uint32_t ts_90 = timestamp / 300;
    write_rtp_header( p_rtp->pkt, MPEG_TS_PAYLOAD_TYPE, p_rtp->seq & 0xffff, ts_90, p_rtp->ssrc );
    memcpy( &p_rtp->pkt[RTP_HEADER_SIZE], data, len );

    if( udp_write( p_rtp->udp_handle, p_rtp->pkt, RTP_PACKET_SIZE ) < 0 )
        ret = -1;

    if( p_rtp->fec_columns && p_rtp->fec_rows )
    {
        int row_idx = (p_rtp->seq / p_rtp->fec_columns) % p_rtp->fec_rows;
        int column_idx = p_rtp->seq % p_rtp->fec_columns;

        if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED && row_idx == 0 && column_idx == 0 && p_rtp->seq >= p_rtp->fec_columns*p_rtp->fec_rows )
        {
            p_rtp->column_phase ^= 1;
        }

        uint8_t *row = &p_rtp->row_data[row_idx*p_rtp->fec_pkt_len];
        uint8_t *column = &p_rtp->column_data[(column_idx*2+p_rtp->column_phase)*p_rtp->fec_pkt_len];

        uint8_t *row_ts = &row[RTP_HEADER_SIZE+TS_OFFSET];
        *row_ts++ ^= ts_90 >> 24;
        *row_ts++ ^= (ts_90 >> 16) & 0xff;
        *row_ts++ ^= (ts_90 >>  8) & 0xff;
        *row_ts++ ^= (ts_90) & 0xff;
        xor_packet_c( &row[RTP_HEADER_SIZE+FEC_HEADER_SIZE], &p_rtp->pkt[RTP_HEADER_SIZE], TS_PACKETS_SIZE );

        if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED )
        {
            uint8_t *column_ts = &column[RTP_HEADER_SIZE+TS_OFFSET];
            *column_ts++ ^= ts_90 >> 24;
            *column_ts++ ^= (ts_90 >> 16) & 0xff;
            *column_ts++ ^= (ts_90 >>  8) & 0xff;
            *column_ts++ ^= (ts_90) & 0xff;

            xor_packet_c( &column[RTP_HEADER_SIZE+FEC_HEADER_SIZE], &p_rtp->pkt[RTP_HEADER_SIZE], TS_PACKETS_SIZE );
        }

        /* Check if we can send packets. Start with rows to match the suggestion in the ProMPEG spec */
        if( column_idx == (p_rtp->fec_columns-1) )
        {
            write_rtp_header( row, FEC_PAYLOAD_TYPE, p_rtp->row_seq++ & 0xffff, 0, 0 );
            write_fec_header( p_rtp, &row[RTP_HEADER_SIZE], 1, (p_rtp->seq - column_idx) & 0xffff );

            if( write_fec_packet( p_rtp->row_handle, row, FEC_PACKET_SIZE ) < 0 )
                ret = -1;
        }

        /* Pre-write the RTP and FEC header */
        if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED && row_idx == (p_rtp->fec_rows-1) )
        {
            write_rtp_header( column, FEC_PAYLOAD_TYPE, p_rtp->column_seq++ & 0xffff, 0, 0 );
            write_fec_header( p_rtp, &column[RTP_HEADER_SIZE], 0, (p_rtp->seq - (p_rtp->fec_columns*(p_rtp->fec_rows-1))) & 0xffff );
        }

        /* Interleave the column FEC data from the previous matrix */
        if( fec_type == FEC_TYPE_COP3_BLOCK_ALIGNED && p_rtp->seq >= p_rtp->fec_columns*p_rtp->fec_rows && p_rtp->seq % p_rtp->fec_rows == 0 )
        {
            uint64_t send_column_idx = (p_rtp->seq % (p_rtp->fec_columns*p_rtp->fec_rows)) / p_rtp->fec_rows;
            uint8_t *column_tx = &p_rtp->column_data[(send_column_idx*2+!p_rtp->column_phase)*p_rtp->fec_pkt_len];

            if( write_fec_packet( p_rtp->column_handle, column_tx, FEC_PACKET_SIZE ) < 0 )
                ret = -1;
        }

        if( fec_type == FEC_TYPE_COP3_NON_BLOCK_ALIGNED && p_rtp->seq >= (p_rtp->fec_columns * p_rtp->fec_rows + column_idx*(p_rtp->fec_columns+1)) )
        {
            int deoffsetted_seq = (p_rtp->seq - column_idx) - (column_idx*p_rtp->fec_columns);
            if( deoffsetted_seq % ( p_rtp->fec_columns * p_rtp->fec_rows ) == 0 )
            {
                write_rtp_header( column, FEC_PAYLOAD_TYPE, p_rtp->column_seq++ & 0xffff, 0, 0 );
                write_fec_header( p_rtp, &column[RTP_HEADER_SIZE], 0, (p_rtp->seq - (p_rtp->fec_columns*p_rtp->fec_rows)) & 0xffff );

                if( write_fec_packet( p_rtp->column_handle, column, FEC_PACKET_SIZE ) < 0 )
                    ret = -1;
            }
        }

        if( fec_type == FEC_TYPE_COP3_NON_BLOCK_ALIGNED && p_rtp->seq >= column_idx*(p_rtp->fec_columns+1) )
        {
            uint8_t *column_ts = &column[RTP_HEADER_SIZE+TS_OFFSET];
            *column_ts++ ^= ts_90 >> 24;
            *column_ts++ ^= (ts_90 >> 16) & 0xff;
            *column_ts++ ^= (ts_90 >>  8) & 0xff;
            *column_ts++ ^= (ts_90) & 0xff;

            xor_packet_c( &column[RTP_HEADER_SIZE+FEC_HEADER_SIZE], &p_rtp->pkt[RTP_HEADER_SIZE], TS_PACKETS_SIZE );
        }
    }

    p_rtp->seq++;
    p_rtp->pkt_cnt++;
    p_rtp->octet_cnt += len;

    return ret;
}

static void rtp_close( hnd_t handle )
{
    obe_rtp_ctx *p_rtp = handle;

    udp_close( p_rtp->udp_handle );
    if( p_rtp->column_data )
        free( p_rtp->column_data );
    if( p_rtp->row_data )
        free( p_rtp->row_data );
    if( p_rtp->column_handle )
        udp_close( p_rtp->column_handle );
    if( p_rtp->row_handle )
        udp_close( p_rtp->row_handle );

    free( p_rtp );
}

static void close_output( void *handle )
{
    struct ip_status *status = handle;

    if( status->output->output_dest.type == OUTPUT_RTP )
    {
        if( *status->ip_handle )
            rtp_close( *status->ip_handle );
    }
    else
    {
        if( *status->ip_handle )
            udp_close( *status->ip_handle );
    }
    if( status->output->output_dest.target  )
        free( status->output->output_dest.target );

    pthread_mutex_unlock( &status->output->queue.mutex );
}

static void *open_output( void *ptr )
{
    obe_output_t *output = ptr;
    obe_output_dest_t *output_dest = &output->output_dest;
    struct ip_status status;
    hnd_t ip_handle = NULL;
    int num_muxed_data = 0;
    AVBufferRef **muxed_data;
    obe_udp_opts_t udp_opts;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    status.output = output;
    status.ip_handle = &ip_handle;
    pthread_cleanup_push( close_output, (void*)&status );

    udp_populate_opts( &udp_opts, output_dest->target );

    if( output_dest->type == OUTPUT_RTP )
    {
        if( rtp_open( &ip_handle, &udp_opts, output_dest ) < 0 )
            return NULL;
    }
    else
    {
        if( udp_open( &ip_handle, &udp_opts ) < 0 )
        {
            fprintf( stderr, "[udp] Could not create udp output" );
            return NULL;
        }
    }

    while( 1 )
    {
        pthread_mutex_lock( &output->queue.mutex );
        while( !output->queue.size && !output->cancel_thread )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &output->queue.in_cv, &output->queue.mutex );
        }

        if( output->cancel_thread )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            break;
        }

        num_muxed_data = output->queue.size;

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, output->queue.queue, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &output->queue.mutex );

//        printf("\n START %i \n", num_muxed_data );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            if( output_dest->type == OUTPUT_RTP )
            {
                if( write_rtp_pkt( ip_handle, &muxed_data[i]->data[7*sizeof(int64_t)], TS_PACKETS_SIZE, AV_RN64( muxed_data[i]->data ), output_dest->fec_type ) < 0 )
                    syslog( LOG_ERR, "[rtp] Failed to write RTP packet\n" );
            }
            else
            {
                if( udp_write( ip_handle, &muxed_data[i]->data[7*sizeof(int64_t)], TS_PACKETS_SIZE ) < 0 )
                    syslog( LOG_ERR, "[udp] Failed to write UDP packet\n" );
            }

            remove_from_queue( &output->queue );
            av_buffer_unref( &muxed_data[i] );
        }

        free( muxed_data );
        muxed_data = NULL;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t ip_output = { open_output };
