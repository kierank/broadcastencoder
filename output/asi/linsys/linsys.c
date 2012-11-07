/*****************************************************************************
 * linsys.c : UDP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 * DVB ASI stuffing code is Copyright (C) 2000-2004 Linear Systems Ltd
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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include "common/common.h"
#include "common/linsys/util.h"
#include "common/linsys/pci_ids.h"
#include "common/linsys/asi.h"
#include "output/output.h"
#include <libavutil/fifo.h>

#define BUFLEN 256

typedef struct
{
    int card_idx;

    int bufsize;
    uint8_t *buffer;

    int fd;
    struct pollfd pfd;

} linsys_asi_ctx;

struct linsys_status
{
    obe_output_params_t *output_params;
    hnd_t *linsys_handle;
    AVFifoBuffer *fifo_data;
};

/* Convert packet size and stuffing parameters to bitrate */
static double br( int packetsize, int ip_int, int normal_ip, int big_ip )
{
    if( ( normal_ip == 0 ) || ( big_ip == 0 ) )
    {
        normal_ip = 1;
        big_ip = 0;
    }
    return 270000000 * 0.8 * packetsize / (packetsize + ip_int + (double)big_ip / (normal_ip + big_ip) + 2);
}

/* Return the error in parts per million */
static double ppm( double testrate, double bitrate )
{
    return 1000000 * (testrate - bitrate) / bitrate;
}

/* Simulate a finetuning cycle with interleaving,
 * while measuring the differences between the
 * ideal packet transmission times and the actual
 * packet transmission times.
 * Return the maximum range of these differences in seconds.
 */
static double jitter( int ft0, int ft1, int il0, int il1, double frac )
{
    int il_cycles;
    double f_time = 0, il_time = 0;
    double jit = 0, min_jitter = 0, max_jitter = 0;

    /* The number of interleaved finetuning cycles to perform */
    if ((ft0 / il0) < (ft1 / il1))
        il_cycles = ft0 / il0;
    else
        il_cycles = ft1 / il1;

    /* Interleaved finetuning cycles */
    for( int i = 0; i < il_cycles; i++ )
    {
        /* Transmit IL0 packets */
        for( int j = 0; j < il0; j++ )
        {
            f_time += frac;
            jit = il_time - f_time;
            if( jit < min_jitter )
                min_jitter = jit;
            else if (jit > max_jitter)
                max_jitter = jit;
//          printf ("\t%i\t%f\n", il_time, f_time);
        }

        /* Transmit IL1 packets */
        for( int j = 0; j < il1; j++ )
        {
            il_time++;
            f_time += frac;
            jit = il_time - f_time;
            if( jit < min_jitter )
                min_jitter = jit;
            else if( jit > max_jitter )
                max_jitter = jit;
//          printf ("\t%i\t%f\n", il_time, f_time);
        }
    }

    /* The remainder of the finetuning cycle */
    for( int i = 0; i < (ft0 - il0 * il_cycles); i++ )
    {
        f_time += frac;
        jit = il_time - f_time;
        if( jit < min_jitter )
            min_jitter = jit;
        else if( jit > max_jitter )
            max_jitter = jit;
//      printf ("\t%i\t%f\n", il_time, f_time);
    }
    for( int i = 0; i < (ft1 - il1 * il_cycles); i++ )
    {
        il_time++;
        f_time += frac;
        jit = il_time - f_time;
        if( jit < min_jitter )
            min_jitter = jit;
        else if( jit > max_jitter )
            max_jitter = jit;
//      printf ("\t%i\t%f\n", il_time, f_time);
    }

    return (max_jitter - min_jitter) / 27000000;
}

static void linsys_asi_close( hnd_t handle )
{
    linsys_asi_ctx *s = handle;

    if( s->buffer )
        free( s->buffer );

    if( s->fd )
    {
        fsync( s->fd );
        close( s->fd );
    }

    free( handle );
}

static int linsys_asi_open( hnd_t *p_handle, int card_idx, double bitrate )
{
    int packetsize, ip_int, coarse_ip, normal_ip, big_ip;
    int il_normal, il_big, num;
    double ip, ip_frac, il_jitter;
    double testrate, tolerance;
    unsigned long int cap, transport, mode, bufsize;
    const char dev[] = "/dev/asitx%i";
    const char fmt[] = "/sys/class/asi/asitx%i/%s";
    char dev_buf[BUFLEN], fmt_buf[BUFLEN], str[BUFLEN], data[BUFLEN], *endptr;
    struct stat buf = {0};
    struct asi_txstuffing stuffing;

    memset( &stuffing, 0, sizeof(stuffing) );

    linsys_asi_ctx *s = calloc( 1, sizeof(*s) );
    *p_handle = NULL;
    if( !s )
        return -1;

    /* TODO: remove linsys utilities (and unsigned long...) */

    snprintf( dev_buf, sizeof(dev_buf), dev, card_idx );
    if( stat( dev_buf, &buf ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] could not get file status\n" );
        goto fail;
    }
    if( !S_ISCHR( buf.st_mode ) )
    {
        fprintf( stderr, "[linsys-asi] %s is not a character device\n", dev_buf );
        goto fail;
    }
    if( buf.st_rdev & 0x0080 )
    {
        fprintf( stderr, "[linsys-asi] %s: not a transmitter\n", dev_buf );
        goto fail;
    }
    num = buf.st_rdev & 0x007f;
    snprintf( fmt_buf, sizeof(fmt_buf), fmt, num, "dev" );
    memset( str, 0, sizeof(str) );
    if( util_read( fmt_buf, str, sizeof(str) ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
        perror( "unable to get the device number" );
        goto fail;
    }

    if( strtoul( str, &endptr, 0 ) != (buf.st_rdev >> 8) )
    {
        fprintf( stderr, "[linsys-asi] %s: not an ASI device\n", dev_buf );
        goto fail;
    }

    if( *endptr != ':' )
    {
        fprintf( stderr, "[linsys-asi] %s: error reading %s\n", dev_buf, fmt_buf );
        goto fail;
    }

    /* Open the file */
    if( (s->fd = open( dev_buf, O_WRONLY, 0 )) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: Unable to open file for writing\n", dev_buf );
        goto fail;
    }

    syslog( LOG_INFO, "Opened Linsys-ASI card %s", dev_buf );

    /* TODO: make it possible for the user to set transport */
    /* Get the transport type */
    snprintf( fmt_buf, sizeof(fmt_buf), fmt, num, "transport" );
    if( util_strtoul( fmt_buf, &transport ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
        perror( "Unable to get the transmitter transport type" );
        goto fail;
    }

    /* Get the transmitter capabilities */
    if( ioctl( s->fd, ASI_IOC_TXGETCAP, &cap ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
        perror( "Unable to get the transmitter capabilities" );
        goto fail;
    }

    /* Get the buffer size */
    snprintf( fmt_buf, sizeof(fmt_buf), fmt, num, "bufsize" );
    if( util_strtoul( fmt_buf, &bufsize ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
        perror( "Unable to get the transmitter buffer size" );
        goto fail;
    }

    close( s->fd );

    // fixme set buffer sizes

    /* TODO: support other types */
    /* Get the output packet size */
    snprintf( fmt_buf, sizeof(fmt_buf), fmt, num, "mode" );
    snprintf( data, sizeof(data), "%i\n", ASI_CTL_TX_MODE_188 );

    if( util_write( fmt_buf, data, sizeof(data) ) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
        perror( "Unable to write output packet size" );
        goto fail;
    }

    /* Open the file */
    if( (s->fd = open( dev_buf, O_WRONLY, 0 )) < 0 )
    {
        fprintf( stderr, "[linsys-asi] %s: Unable to open file for writing\n", dev_buf );
        goto fail;
    }

    /* TODO: make user selectable */
    packetsize = 188;
    tolerance = 0;

    /* Assume no interbyte stuffing.
     * This will allow bitrates down to about 2400 bps,
     * which is probably good enough! */
    ip = 270000000 * 0.8 * packetsize / bitrate - packetsize - 2;
    //printf ("\n%f bytes of stuffing required per packet.\n", ip);
    ip_int = ip;
    ip_frac = ip - ip_int;
    if( ip_frac > 0.5 )
        coarse_ip = ip_int + 1;
    else
        coarse_ip = ip_int;

    testrate = br( packetsize, coarse_ip, 1, 0 );
    //printf ("ib = 0, ip = %i  =>  bitrate = %f bps, err = %.0f ppm\n",
    //    coarse_ip, testrate, ppm( testrate, bitrate ));
    normal_ip = 0;
    big_ip = 0;
    il_normal = 0;
    il_big = 0;
    if( ( ip_frac > (double)1 / 256 / 2 ) && ( ip_frac < (1 - (double)1 / 256 / 2) ) &&
        ( fabs( ppm( testrate, bitrate ) ) > tolerance ) )
    {
        int n = 1, b = 1, il0, il1, il_normal_max, il_big_max;
        double best_error = 1.0, error, jit = 0.1;

        /* Find the finetuning parameters which
         * best approximate the desired bitrate.
         * Break at 1 us p-p network jitter or
         * the desired bitrate accuracy, whichever
         * gives less network jitter */
        //printf( "\nFinetuning ib = 0, ip = %i:\n", ip_int );
        while( ( b < 256 ) && ( n < 256 ) && ( ((double)(n * b) / (n + b)) <= 27 ) )
        {
            error = (double)b / (n + b) - ip_frac;
            if( fabs( error ) < fabs( best_error ) )
            {
                best_error = error;
                normal_ip = n;
                big_ip = b;
                testrate = br( packetsize, ip_int, n, b );
                //printf ("normal_ip = %i, big_ip = %i"
                //    "  =>  bitrate = %f bps, "
                //    "err = %.0f ppm\n",
                //    n, b,
                //    testrate, ppm( testrate, bitrate ));
                if( fabs( ppm( testrate, bitrate ) ) < tolerance )
                    break;
            }
            if( error < 0 )
                b++;
            else
                n++;
        }

        /* Calculate the network jitter produced by finetuning */
        //ft_jitter = jitter( normal_ip, big_ip, normal_ip, big_ip, (double)big_ip / (normal_ip + big_ip) );

        /* Find the interleaving parameters which
         * produce the least network jitter */
        if( ( normal_ip == 1 ) || ( big_ip == 1 ) )
            il_jitter = jitter( normal_ip, big_ip, normal_ip, big_ip, (double)big_ip / (normal_ip + big_ip) );
        else
        {
            il_jitter = 0.1;
            il_normal_max = ( normal_ip > 14 ) ? 14 : normal_ip;
            for( il0 = 1; il0 <= il_normal_max; il0++ )
            {
                il_big_max = ( big_ip > 14 ) ? 14 : big_ip;
                il1 = 1;
                while( (il1 <= il_big_max ) && ( il0 + il1 <= 15 ) )
                {
                    jit = jitter( normal_ip, big_ip, il0, il1, (double)big_ip / (normal_ip + big_ip) );
                    if( jit < il_jitter )
                    {
                        il_jitter = jit;
                        il_normal = il0;
                        il_big = il1;
                    }
                    il1++;
                }
            }
        }
    }
    else
    {
        ip_int = coarse_ip;
        //ft_jitter = 0;
        il_jitter = 0;
    }

    /* Has interleaved finetuning */
    if( cap & ASI_CAP_TX_INTERLEAVING )
    {
        stuffing.ib = 0;
        stuffing.ip = ip_int;
        stuffing.normal_ip = normal_ip;
        stuffing.big_ip = big_ip;
        stuffing.il_normal = il_normal;
        stuffing.il_big = il_big;
    }
    /* Has ordinary finetuning */
    else if( cap & ASI_CAP_TX_FINETUNING )
    {
        stuffing.ib = 0;
        stuffing.ip = ip_int;
        stuffing.normal_ip = normal_ip;
        stuffing.big_ip = big_ip;
    }
    else
    {
        stuffing.ib = 0;
        stuffing.ip = coarse_ip;
    }

    if( transport == ASI_CTL_TRANSPORT_DVB_ASI )
    {
        if( ioctl( s->fd, ASI_IOC_TXSETSTUFFING, &stuffing ) < 0 )
        {
            fprintf( stderr, "[linsys-asi] %s: ", dev_buf );
            perror( "Unable to set stuffing parameters" );
            goto fail;
        }
    }
    /* FIXME 310M */

    if( bufsize < BUFSIZ )
        bufsize = BUFSIZ;

    s->bufsize = bufsize;
    s->buffer = malloc( s->bufsize );
    if( !s->buffer )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto fail;
    }

    s->pfd.fd = s->fd;
    s->pfd.events = POLLOUT | POLLPRI;

    *p_handle = s;

    return 0;

fail:
    linsys_asi_close( s );

    return -1;
}

static int write_packets( hnd_t handle, AVFifoBuffer *fifo_data )
{
    linsys_asi_ctx *s = handle;
    int bufsize, bytes_written, write_ret;
    unsigned int val;

    int i = 0;

    while( av_fifo_size( fifo_data ) >= s->bufsize )
    {
        bufsize = s->bufsize;
        av_fifo_generic_read( fifo_data, s->buffer, bufsize, NULL );

        printf("\n polling iter %i fifosize %i bufsize %i \n", i++, av_fifo_size( fifo_data ), s->bufsize );

        if( poll( &s->pfd, 1, -1 ) < 0 )
        {
            syslog( LOG_ERR, "[linsys-asi] card %i: unable to poll device file", s->card_idx );
            return -1;
        }

        if( s->pfd.revents & POLLOUT )
        {
             bytes_written = 0;
             while( bytes_written < bufsize )
             {
                 if( (write_ret = write( s->fd, s->buffer + bytes_written, bufsize - bytes_written )) < 0 )
                 {
                     syslog( LOG_ERR, "[linsys-asi] card %i: unable to write to device file", s->card_idx );
                     return -1;
                 }
                 bytes_written += write_ret;
             }
        }

        if( s->pfd.revents & POLLPRI )
        {
            if( ioctl( s->fd, ASI_IOC_TXGETEVENTS, &val ) < 0 )
            {
                syslog( LOG_ERR, "[linsys-asi] card %i: unable to get the transmitter event flags", s->card_idx );
                return -1;
            }
            if( val & ASI_EVENT_TX_BUFFER )
                syslog( LOG_WARNING, "[linsys-asi] card %i driver transmit buffer queue underrun detected", s->card_idx );
            if( val & ASI_EVENT_TX_FIFO )
                syslog( LOG_WARNING, "[linsys-asi] card %i onboard transmit FIFO underrun detected", s->card_idx );
            if( val & ASI_EVENT_TX_DATA )
                syslog( LOG_WARNING, "[linsys-asi] card %i transmit data status change detected", s->card_idx );
        }
    }

    return 0;
}

static void close_output( void *handle )
{
    struct linsys_status *status = handle;

    if( status->fifo_data )
        av_fifo_free( status->fifo_data );
    if( *status->linsys_handle )
        linsys_asi_close( *status->linsys_handle );
    free( status->output_params );
}

static void *open_output( void *ptr )
{
    hnd_t linsys_handle = NULL;
    struct linsys_status status;
    obe_output_params_t *output_params = ptr;
    obe_output_opts_t *output_opts = &output_params->output_opts;
    obe_t *h = output_params->h;
    AVFifoBuffer *fifo_data = NULL;
    int num_muxed_data = 0;
    uint8_t **muxed_data;

    fifo_data = av_fifo_alloc( 188 );
    if( !fifo_data )
    {
        fprintf( stderr, "[linsys-asi] Could not allocate data fifo" );
        return NULL;
    }

    status.output_params = output_params;
    status.linsys_handle = &linsys_handle;
    status.fifo_data = fifo_data;
    pthread_cleanup_push( close_output, (void*)&status );

    if( linsys_asi_open( &linsys_handle, output_opts->card_idx, output_opts->asi_bitrate ) < 0 )
        return NULL;

    while( 1 )
    {
        pthread_mutex_lock( &h->output_queue.mutex );
        while( !h->output_queue.size )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &h->output_queue.in_cv, &h->output_queue.mutex );
        }

        num_muxed_data = h->output_queue.size;

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &h->output_queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, h->output_queue.queue, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &h->output_queue.mutex );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            if( av_fifo_realloc2( fifo_data, av_fifo_size( fifo_data ) + TS_PACKETS_SIZE ) < 0 )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }

            av_fifo_generic_write( fifo_data, &muxed_data[i][7*sizeof(int64_t)], TS_PACKETS_SIZE, NULL );

            remove_from_queue( &h->output_queue );
            free( muxed_data[i] );
        }

        write_packets( linsys_handle, fifo_data );

        free( muxed_data );
        num_muxed_data = 0;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t linsys_asi_output = { open_output };
