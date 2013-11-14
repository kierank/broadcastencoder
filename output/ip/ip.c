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
    uint16_t seq;
    uint32_t ssrc;

    uint32_t pkt_cnt;
    uint32_t octet_cnt;

    /* FEC */
    hnd_t column_handle;
    hnd_t row_handle;

    int fec_columns;
    int fec_rows;

    int fec_pkt_len;
    uint8_t *column_data;
    uint8_t *row_data;

    uint16_t column_seq;
    uint16_t row_seq;

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

    if( udp_open( &p_rtp->udp_handle, udp_opts ) < 0 )
    {
        fprintf( stderr, "[rtp] Could not create udp output" );
        return -1;
    }

    p_rtp->ssrc = av_get_random_seed();

    p_rtp->fec_columns = output_dest->fec_columns;
    p_rtp->fec_rows = output_dest->fec_rows;
    if( p_rtp->fec_columns || p_rtp->fec_rows )
    {
        p_rtp->fec_pkt_len = FFALIGN( FEC_PACKET_SIZE, 32 );
        p_rtp->column_data = calloc( output_dest->fec_columns, p_rtp->fec_pkt_len );
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
static void write_rtp_header( bs_t *s, uint8_t payload_type, uint16_t seq, uint32_t ts_90, uint32_t ssrc )
{
    bs_write( s, 2, RTP_VERSION ); // version
    bs_write1( s, 0 );             // padding
    bs_write1( s, 0 );             // extension
    bs_write( s, 4, 0 );           // CSRC count
    bs_write1( s, 0 );             // marker
    bs_write( s, 7, payload_type ); // payload type
    bs_write( s, 16, seq );         // sequence number
    bs_write32( s, ts_90 );         // timestamp
    bs_write32( s, ssrc );          // ssrc
}

static void write_fec_header( hnd_t handle, bs_t *s, int row, uint16_t snbase )
{
    obe_rtp_ctx *p_rtp = handle;

    bs_write( s, 16, snbase );     // SNBase low bits
    bs_write( s, 16, RTP_PACKET_SIZE ); // Length Recovery
    bs_write1( s, 1 );             // RFC2733 Extension
    bs_write1( s, 1 );             // E
    bs_write( s, 7, MPEG_TS_PAYLOAD_TYPE ); // PT Recovery
    bs_write( s, 24, 0 ); // Mask
    bs_flush( s );

    /* skip timestamp field */
    uint8_t *ptr = &s->p_start[(bs_pos( s ) + 32) >> 3];

    bs_init( s, ptr, 4 );
    bs_write1( s, 0 );    // N
    bs_write1( s, row );  // D
    bs_write( s, 3, 0 ); // Type
    bs_write( s, 3, 0 ); // Index
    bs_write( s, 8, row ? 1 : p_rtp->fec_columns ); // Offset
    bs_write( s, 8, row ? p_rtp->fec_columns : p_rtp->fec_rows ); // NA
    bs_write( s, 8, 0 ); // SNBase ext bits
}

static int write_fec_packet( hnd_t udp_handle, uint8_t *data, int len )
{
    if( udp_write( udp_handle, data, len ) < 0 )
        return -1;

    memset( data, 0, len );

    return 0;
}

static int write_rtp_pkt( hnd_t handle, uint8_t *data, int len, int64_t timestamp )
{
    obe_rtp_ctx *p_rtp = handle;
    bs_t s;
    bs_init( &s, p_rtp->pkt, RTP_HEADER_SIZE );

    uint32_t ts_90 = timestamp / 300;
    write_rtp_header( &s, MPEG_TS_PAYLOAD_TYPE, p_rtp->seq, ts_90, p_rtp->ssrc );
    bs_flush( &s );

    memcpy( &p_rtp->pkt[RTP_HEADER_SIZE], data, len );

    if( udp_write( p_rtp->udp_handle, p_rtp->pkt, RTP_PACKET_SIZE ) < 0 )
        return -1;

    if( p_rtp->fec_columns || p_rtp->fec_rows )
    {
        int column_idx = p_rtp->seq % p_rtp->fec_columns;
        int row_idx = p_rtp->seq % p_rtp->fec_rows;
        uint8_t *column = &p_rtp->column_data[column_idx*p_rtp->fec_pkt_len];
        uint8_t *row = &p_rtp->row_data[row_idx*p_rtp->fec_pkt_len];

        uint32_t *column_ts = (uint32_t*)&column[RTP_HEADER_SIZE+TS_OFFSET];
        uint32_t *row_ts = (uint32_t*)&row[RTP_HEADER_SIZE+TS_OFFSET];
        *column_ts ^= ts_90;
        *row_ts ^= ts_90;

        xor_packet_c( &column[RTP_HEADER_SIZE+FEC_HEADER_SIZE], p_rtp->pkt, TS_PACKETS_SIZE );

        /* Check if we can send packets. Start with rows to match other encoders
         * Write the headers in advance */
        if( row_idx == p_rtp->fec_rows-1 )
        {
            bs_init( &s, row, RTP_HEADER_SIZE+FEC_HEADER_SIZE );
            write_rtp_header( &s, FEC_PAYLOAD_TYPE, p_rtp->row_seq++, 0, 0 );
            write_fec_header( p_rtp, &s, 1, p_rtp->seq + 1 - p_rtp->fec_columns );
            bs_flush( &s );

            if( write_fec_packet( p_rtp->row_handle, row, FEC_PACKET_SIZE ) )
                return -1;
        }

        if( column_idx == p_rtp->fec_columns-1 )
        {
            bs_init( &s, column, RTP_HEADER_SIZE+FEC_HEADER_SIZE );
            write_rtp_header( &s, FEC_PAYLOAD_TYPE, p_rtp->column_seq++, 0, 0 );
            write_fec_header( p_rtp, &s, 0, p_rtp->seq + 1 - (p_rtp->fec_columns*p_rtp->fec_rows) );
            bs_flush( &s );

            if( write_fec_packet( p_rtp->column_handle, column, FEC_PACKET_SIZE ) )
                return -1;
        }
    }

    p_rtp->seq++;
    p_rtp->pkt_cnt++;
    p_rtp->octet_cnt += len;

    return 0;
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
                if( write_rtp_pkt( ip_handle, &muxed_data[i]->data[7*sizeof(int64_t)], TS_PACKETS_SIZE, AV_RN64( muxed_data[i]->data ) ) < 0 )
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
