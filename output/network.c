/*****************************************************************************
 * network.c : Networking functions
 *****************************************************************************
 * Copyright (C) 2004, 2009 VideoLAN
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This code originates from multicat.
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

#include "network.h"

/* TODO handle error conditions */
int64_t get_wallclock_in_mpeg_ticks( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );

    return ((int64_t)ts.tv_sec * (int64_t)27000000) + (int64_t)(ts.tv_nsec * 27 / 1000);
}

void sleep_mpeg_ticks( int64_t i_delay )
{
    struct timespec ts;
    ts.tv_sec = i_delay / 27000000;
    ts.tv_nsec = (i_delay % 27000000) * 1000 / 27;

    clock_nanosleep( CLOCK_MONOTONIC, 0, &ts, &ts );
}
