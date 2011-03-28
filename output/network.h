/*****************************************************************************
 * network.h : Networking headers
 *****************************************************************************
 * Copyright (C) 2010 FFmpeg
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This code originates from FFmpeg.
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

#ifndef OUTPUT_NETWORK_H
#define OUTPUT_NETWORK_H

#include "common/common.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <libavutil/avstring.h>
#include <libavutil/avurl.h>
#include <libavutil/parseutils.h>

#ifndef IN_MULTICAST
#define IN_MULTICAST(a) ((((uint32_t)(a)) & 0xf0000000) == 0xe0000000)
#endif
#ifndef IN6_IS_ADDR_MULTICAST
#define IN6_IS_ADDR_MULTICAST(a) (((uint8_t *) (a))[0] == 0xff)
#endif

static inline int is_multicast_address( struct sockaddr *addr )
{
    if( addr->sa_family == AF_INET )
    {
        return IN_MULTICAST( ntohl( ((struct sockaddr_in *)addr)->sin_addr.s_addr ) );
    }
#if HAVE_STRUCT_SOCKADDR_IN6
    if( addr->sa_family == AF_INET6 )
    {
        return IN6_IS_ADDR_MULTICAST( &((struct sockaddr_in6 *)addr)->sin6_addr );
    }
#endif

    return 0;
}

int64_t get_wallclock_in_mpeg_ticks( void );
void sleep_mpeg_ticks( int64_t i_delay );

#endif
