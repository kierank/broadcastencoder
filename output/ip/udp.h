/*****************************************************************************
 * udp.h : UDP output headers
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

#ifndef OBE_COMMON_UDP_H
#define OBE_COMMON_UDP_H

#include <netinet/in.h>

typedef struct
{
    int udp_fd;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    bool listener;
} obe_udp_ctx;

typedef struct obe_udp_opts_t
{
    char hostname[1024];
    int  port;
    int  local_port;
    int  reuse_socket;
    int  ttl;
    int  tos;
    int  bind_iface;
    char iface[100];
    bool listener;
} obe_udp_opts_t;

void udp_populate_opts( obe_udp_opts_t *udp_opts, char *uri );
int udp_open( hnd_t *p_handle, obe_udp_opts_t *udp_opts, int fd );
int udp_write( hnd_t p_handle, uint8_t *buf, int size );
void udp_close( hnd_t handle );

#endif /* OBE_COMMON_UDP_H */
