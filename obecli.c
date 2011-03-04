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
#include "obecli.h"

obe_t *h = NULL;

/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;
static char *line_read = NULL;
obe_output_opts_t output = {0};

/* show functions */
static int show_bitdepth( char *command, obecli_command_t *child )
{
    printf( "AVC output bit depth: %i bits per sample\n", x264_bit_depth );

    return 0;
}

static int show_decoders( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Decoders: \n" );

    while( format_names[i].decoder_name )
    {
        if( format_names[i].decoder_name )
            printf( "       %-*s          - %s \n", 8, format_names[i].format_name, format_names[i].decoder_name );
        i++;
    }

    return 0;
}

static int show_encoders( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Encoders: \n" );

    while( format_names[i].format_name )
    {
        if( format_names[i].encoder_name )
            printf( "       %-*s          - %s \n", 8, format_names[i].format_name, format_names[i].encoder_name );
        i++;
    }

    return 0;
}

static int show_help( char *command, obecli_command_t *child )
{
    int i = 0;

#define H0 printf
    H0( "OBE Commands:\n" );

    H0( "\n" );

    H0( "show - Show supported items\n" );

    while( show_commands[i].name )
    {
        H0( "       %-*s          - %s \n", 8, show_commands[i].name, show_commands[i].description );
        i++;
    }

    H0( "\n" );

    i = 0;

    H0( "add  - Add item\n" );
    while( add_commands[i].name )
    {
        H0( "       %-*s          - %s \n", 8, add_commands[i].name, add_commands[i].description );
        i++;
    }

    H0( "\n" );

    H0( "list - List current items\n" );
    H0( "       inputs               - List current inputs\n" );
    H0( "       muxers               - List current muxers\n" );
    H0( "       streams              - List current streams\n" );
    H0( "       outputs              - List current outputs\n" );

    H0( "\n" );

//    H0( "load - Load configuration\n" );

    H0( "set  - Set parameter\n" );
    H0( "       muxer  [opts]        - Set muxer parameters\n" );
    H0( "       output [opts]        - Set output parameters\n" );
    H0( "       stream [opts]        - Set stream parameters\n" );

    H0( "\n" );

    H0( "Starting/Stopping OBE:\n" );
    H0( "start - Start encoding\n" );
    H0( "stop  - Stop encoding\n" );


    H0( "\n" );

    return -1;
}

static char *get_shortname( int stream_format, obecli_format_name_t *names )
{
    int i = 0;

    while( names[i].format_name != 0 && names[i].format != stream_format )
        i++;

    return names[i].format_name;
}

static int probe_device( char *command, obecli_command_t *child )
{
    obe_input_stream_t *stream;
    obe_input_program_t program = {0};
    obe_input_t device = {0};
    char buf[200];
    char *format_name;

    if( !strlen( command ) )
    {
        return -1;
    }

    device.input_type = INPUT_URL;
    device.location = command;

    obe_probe_device( h, &device, &program );

    printf("\n");

    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        format_name = get_shortname( stream->stream_format, format_names );
        if( stream->stream_type == STREAM_TYPE_VIDEO )
        {
            /* TODO: show profile, level, csp etc */
            printf( "Stream-id: %d - Video: %s %dx%d%s %d/%dfps SAR: %d:%d \n", stream->stream_id,
                    format_name, stream->width, stream->height, stream->interlaced ? "i" : "p",
                    stream->timebase_den, stream->timebase_num, stream->sar_num, stream->sar_den );
        }
        else if( stream->stream_type == STREAM_TYPE_AUDIO )
        {
            /* let it work out the number of channels from the channel map */
            av_get_channel_layout_string( buf, 200, 0, stream->channel_layout );
            printf( "Stream-id: %d - Audio: %s%s %s %ikbps %ikHz Language: %s \n", stream->stream_id, format_name,
                    stream->stream_format == AUDIO_AAC ? stream->aac_is_latm ? " LATM" : " ADTS"  : "",
                    buf, stream->bitrate / 1000, stream->sample_rate / 1000, strlen( stream->lang_code ) ? stream->lang_code : "none" );
        }
        else if( stream->stream_format == SUBTITLES_DVB )
        {
            printf( "Stream-id: %d - DVB Subtitles: Language: %s DDS: %s \n", stream->stream_id, stream->lang_code,
                    stream->dvb_has_dds ? "yes" : "no" );
        }
        else if( stream->stream_format == MISC_TELETEXT )
        {
            printf( "Stream-id: %d - Teletext: Language: %s \n", stream->stream_id, stream->lang_code ); // TODO more metadata
        }
        else
            printf( "Stream-id: %d \n", stream->stream_id );
    }

    printf("\n");

    return 0;
}

static int parse_command( char *command, obecli_command_t *commmand_list )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    int i = 0;

    while( commmand_list[i].name != 0 && strcasecmp( commmand_list[i].name, command ) )
        i++;

    if( commmand_list[i].name )
    {
        if( str_len > tok_len )
            commmand_list[i].cmd_func( command+tok_len+1, commmand_list[i].child_commands );
        else
            commmand_list[i].cmd_func( command+tok_len, commmand_list[i].child_commands );
    }
    else
        return -1;

    return 0;
}

int main( int argc, char **argv )
{
    char *prompt = "obecli> ";
    h = obe_setup();
    if( !h )
    {
        fprintf( stderr, "obe_setup failed\n" );
        return -1;
    }

    printf( "\nOpen Broadcast Encoder command line interface.\n" );
    printf( "Version 0.1 \n" );
    printf( "\n" );

    //obe_start( h );

    while( 1 )
    {
        if( line_read )
        {
            free( line_read );
            line_read = NULL;
        }

        line_read = readline( prompt );

        if( line_read && *line_read )
        {
            add_history( line_read );

            int ret = parse_command( line_read, main_commands );
            if( ret == -1 )
                printf( "%s: command not found \n", line_read );
        }
    }

    obe_close( h );

    return 0;
}
