/*****************************************************************************
 * lavc.c: libavcodec common functions
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

#include "common/common.h"
#include <libavcodec/avcodec.h>

int obe_lavc_lockmgr( void **mutex, enum AVLockOp op )
{
    if( op == AV_LOCK_CREATE )
    {
        *mutex = malloc( sizeof(pthread_mutex_t) );
        if( !*mutex )
            return -1;

        pthread_mutex_init( *mutex, NULL );
    }
    else if( op == AV_LOCK_OBTAIN )
        pthread_mutex_lock( *mutex );
    else if( op == AV_LOCK_RELEASE )
        pthread_mutex_unlock( *mutex );
    else /* AV_LOCK_DESTROY */
    {
        pthread_mutex_destroy( *mutex );
        free( *mutex );
    }

    return 0;
}
