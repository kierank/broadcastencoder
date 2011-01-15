/*****************************************************************************
 * obecli.c: open broadcast encoder cli
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <signal.h>
#define _GNU_SOURCE

#include <readline/readline.h>
#include <readline/history.h>

#include "obe.h"

static char *line_read = NULL;

static void show_bitdepth( void )
{
    printf("AVC output bit depth: %i bits per sample\n", x264_bit_depth );
}

static void show_help( int longhelp )
{
#define H0 printf
#define H1 if(longhelp>=1) printf
#define H2 if(longhelp==2) printf
    H0( "OBE\n" );

}

int main( int argc, char **argv )
{
    printf("\nOpen Broadcast Encoder command line interface.\n");
    show_bitdepth();
    printf("\n");

    while( 1 )
    {
        if(line_read)
        {
          free (line_read);
          line_read = NULL;
        }

        line_read = readline ("obecli> ");

        if( line_read && *line_read )
	{
            if( 0 )
            {
            }
            else
                printf( "%s: command not found \n", line_read );

            add_history( line_read );
        }
    }

    return 0;
}
