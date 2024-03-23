/*****************************************************************************
 * udp.c : UDP output functions
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

#include "common/common.h"
#include "output/output.h"
#include "udp.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>

static int udp_set_multicast_opts( int sockfd, obe_udp_ctx *s, int ttl )
{
    switch (s->dest_addr.ss_family) {
    case AF_INET:
#ifdef IP_MULTICAST_TTL
        if( setsockopt( sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv4 multicast\n" );
            return -1;
        }
#endif
        break;

#ifdef IPPROTO_IPV6
    case AF_INET6:
#ifdef IPV6_MULTICAST_HOPS
        if( setsockopt( sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv6 multicast\n" );
            return -1;
        }
#endif
    break;
#endif
    default:
        break;
    }

    return 0;
}

static int udp_set_tos_opts( int sockfd, obe_udp_ctx *s, int tos )
{
    switch (s->dest_addr.ss_family) {
    case AF_INET:
        if( setsockopt( sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv4 TOS\n" );
            return -1;
        }
        break;
    }

    return 0;
}

static int udp_resolve_host(obe_udp_ctx *s, obe_udp_opts_t *udp_opts)
{
    struct addrinfo hints, *res = 0;
    char sport[16];
    const char *node = 0, *service = "0";

    const char *hostname = udp_opts->hostname;
    const int port = udp_opts->port;

    if (port > 0) {
        snprintf(sport, sizeof(sport), "%d", port);
        service = sport;
    }
    if (hostname && *hostname && *hostname != '?')
        node = hostname;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family   = AF_UNSPEC;

    int error = getaddrinfo(node, service, &hints, &res);
    if (error) {
        fprintf(stderr, "[udp] error: %s \n", gai_strerror(error));
        return 1;
    }

    s->dest_addr_len = res->ai_addrlen;
    memcpy(&s->dest_addr, res->ai_addr, s->dest_addr_len );

    freeaddrinfo(res);

    return 0;
}

static inline int is_multicast_address( struct sockaddr *addr )
{
    if( addr->sa_family == AF_INET )
    {
#ifndef IN_MULTICAST
#define IN_MULTICAST(a) ((((uint32_t)(a)) & 0xf0000000) == 0xe0000000)
#endif
        return IN_MULTICAST( ntohl( ((struct sockaddr_in *)addr)->sin_addr.s_addr ) );
    }
#if HAVE_STRUCT_SOCKADDR_IN6
    if( addr->sa_family == AF_INET6 )
    {
#ifndef IN6_IS_ADDR_MULTICAST
#define IN6_IS_ADDR_MULTICAST(a) (((uint8_t *) (a))[0] == 0xff)
#endif
        return IN6_IS_ADDR_MULTICAST( &((struct sockaddr_in6 *)addr)->sin6_addr );
    }
#endif

    return 0;
}

void udp_populate_opts( obe_udp_opts_t *udp_opts, char *uri )
{
    char buf[256];
    const char *p = strchr( uri, '?' );

    memset( udp_opts, 0, sizeof(*udp_opts) );

    if( p )
    {
        if( av_find_info_tag( buf, sizeof(buf), "ttl", p ) )
            udp_opts->ttl = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "tos", p ) )
            udp_opts->tos = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "iface", p ) )
        {
            udp_opts->bind_iface = 1;
            strncpy( udp_opts->iface, buf, sizeof(udp_opts->iface) - 1 );
        }
    }

    /* fill the dest addr */
    av_url_split( NULL, 0, NULL, 0, udp_opts->hostname, sizeof(udp_opts->hostname), &udp_opts->port, NULL, 0, uri );
}

hnd_t *udp_open(obe_udp_opts_t *udp_opts, int fd)
{
    int udp_fd = -1;

    obe_udp_ctx *s = calloc( 1, sizeof(*s) );
    if (!s)
        return NULL;

    /* sets dst_addr */
    if (udp_resolve_host(s, udp_opts))
        goto fail;

    if (fd >= 0) {
        /* reuse same socket (FEC) */
        s->udp_fd = dup(fd);
        return (hnd_t*)s;
    }

    udp_fd = socket(s->dest_addr.ss_family , SOCK_DGRAM, 0);
    if( udp_fd < 0 )
        goto fail;

    int reuse_socket = 1;
    if( setsockopt( udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_socket, sizeof(reuse_socket) ) != 0)
        goto fail;

    if( udp_opts->bind_iface ) {
        if( setsockopt( udp_fd, SOL_SOCKET, SO_BINDTODEVICE, udp_opts->iface, strlen(udp_opts->iface ) ) )
            goto fail;
    } else {
        if( bind( udp_fd, (struct sockaddr *)&s->dest_addr, s->dest_addr_len ) < 0 )
            goto fail;
    }

    /* set output multicast ttl */
    bool is_multicast = is_multicast_address( (struct sockaddr*) &s->dest_addr );
    if( is_multicast && udp_set_multicast_opts( udp_fd, s, udp_opts->ttl) < 0 )
        goto fail;

    /* set tos/diffserv */
    if( udp_opts->tos && udp_set_tos_opts( udp_fd, s, udp_opts->tos) < 0 )
        goto fail;

    s->udp_fd = udp_fd;
    return (hnd_t*)s;

 fail:
    if( udp_fd >= 0 )
        close( udp_fd );

    free(s);
    return NULL;
}

int udp_write( hnd_t handle, uint8_t *buf, int size )
{
    obe_udp_ctx *s = handle;

    return sendto( s->udp_fd, buf, size, 0, (struct sockaddr *)&s->dest_addr, s->dest_addr_len );
}

void udp_close( hnd_t handle )
{
    obe_udp_ctx *s = handle;

    close( s->udp_fd );
    free( s );
}
