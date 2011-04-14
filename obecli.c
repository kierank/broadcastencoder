/*****************************************************************************
 * obecli.c: open broadcast encoder cli
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Some code originates from the x264 project
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
#include <assert.h>

#include <signal.h>
#define _GNU_SOURCE

#include <readline/readline.h>
#include <readline/history.h>

#include "obe.h"
#include "obecli.h"

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "obecli", __VA_ARGS__ )
#define RETURN_IF_ERROR( cond, ... ) RETURN_IF_ERR( cond, "options", NULL, __VA_ARGS__ )

obe_t *h = NULL;

/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;
static char *line_read = NULL;

static const char * const ts_types[] = { "generic", "dvb", "cablelabs", "atsc", "isdb", 0 };

static char **obe_split_string( char *string, char *sep, uint32_t limit )
{
    if( !string )
        return NULL;
    int sep_count = 0;
    char *tmp = string;
    while( ( tmp = ( tmp = strstr( tmp, sep ) ) ? tmp + strlen( sep ) : 0 ) )
        ++sep_count;
    if( sep_count == 0 )
    {
        if( string[0] == '\0' )
            return calloc( 1, sizeof( char** ) );
        char **ret = calloc( 2, sizeof( char** ) );
        ret[0] = strdup( string );
        return ret;
    }

    char **split = calloc( ( limit > 0 ? limit : sep_count ) + 2, sizeof(char**) );
    int i = 0;
    char *str = strdup( string );
    assert( str );
    char *esc = NULL;
    char *tok = str, *nexttok = str;
    do
    {
        nexttok = strstr( nexttok, sep );
        if( nexttok )
            *nexttok++ = '\0';
        if( ( limit > 0 && i >= limit ) ||
            ( i > 0 && ( ( esc = strrchr( split[i-1], '\\' ) ) ? esc[1] == '\0' : 0 ) ) ) // Allow escaping
        {
            int j = i-1;
            if( esc )
                esc[0] = '\0';
            split[j] = realloc( split[j], strlen( split[j] ) + strlen( sep ) + strlen( tok ) + 1 );
            assert( split[j] );
            strcat( split[j], sep );
            strcat( split[j], tok );
            esc = NULL;
        }
        else
        {
            split[i++] = strdup( tok );
            assert( split[i-1] );
        }
        tok = nexttok;
    } while ( tok );
    free( str );
    assert( !split[i] );

    return split;
}

static void obe_free_string_array( char **array )
{
    if( !array )
        return;
    for( int i = 0; array[i] != NULL; i++ )
        free( array[i] );
    free( array );
}

static char **obe_split_options( const char *opt_str, const char *options[] )
{
    if( !opt_str )
        return NULL;
    char *opt_str_dup = strdup( opt_str );
    char **split = obe_split_string( opt_str_dup, ",", 0 );
    free( opt_str_dup );
    int split_count = 0;
    while( split[split_count] != NULL )
        ++split_count;

    int options_count = 0;
    while( options[options_count] != NULL )
        ++options_count;

    char **opts = calloc( split_count * 2 + 2, sizeof( char ** ) );
    char **arg = NULL;
    int opt = 0, found_named = 0, invalid = 0;
    for( int i = 0; split[i] != NULL; i++, invalid = 0 )
    {
        arg = obe_split_string( split[i], "=", 2 );
        if( arg == NULL )
        {
            if( found_named )
                invalid = 1;
            else RETURN_IF_ERROR( i > options_count || options[i] == NULL, "Too many options given\n" )
            else
            {
                opts[opt++] = strdup( options[i] );
                opts[opt++] = strdup( "" );
            }
        }
        else if( arg[0] == NULL || arg[1] == NULL )
        {
            if( found_named )
                invalid = 1;
            else RETURN_IF_ERROR( i > options_count || options[i] == NULL, "Too many options given\n" )
            else
            {
                opts[opt++] = strdup( options[i] );
                if( arg[0] )
                    opts[opt++] = strdup( arg[0] );
                else
                    opts[opt++] = strdup( "" );
            }
        }
        else
        {
            found_named = 1;
            int j = 0;
            while( options[j] != NULL && strcmp( arg[0], options[j] ) )
                ++j;
            RETURN_IF_ERROR( options[j] == NULL, "Invalid option '%s'\n", arg[0] )
            else
            {
                opts[opt++] = strdup( arg[0] );
                opts[opt++] = strdup( arg[1] );
            }
        }
        RETURN_IF_ERROR( invalid, "Ordered option given after named\n" )
        obe_free_string_array( arg );
    }
    obe_free_string_array( split );
    return opts;
}

static char *obe_get_option( const char *name, char **split_options )
{
    if( !split_options )
        return NULL;
    int last_i = -1;
    for( int i = 0; split_options[i] != NULL; i += 2 )
        if( !strcmp( split_options[i], name ) )
            last_i = i;
    if( last_i >= 0 )
        return split_options[last_i+1][0] ? split_options[last_i+1] : NULL;
    return NULL;
}

static int obe_otob( char *str, int def )
{
   int ret = def;
   if( str )
       ret = !strcasecmp( str, "true" ) ||
             !strcmp( str, "1" ) ||
             !strcasecmp( str, "yes" );
   return ret;
}

static double obe_otof( char *str, double def )
{
   double ret = def;
   if( str )
   {
       char *end;
       ret = strtod( str, &end );
       if( end == str || *end != '\0' )
           ret = def;
   }
   return ret;
}

static int obe_otoi( char *str, int def )
{
    int ret = def;
    if( str )
    {
        char *end;
        ret = strtol( str, &end, 0 );
        if( end == str || *end != '\0' )
            ret = def;
    }
    return ret;
}

static char *obe_otos( char *str, char *def )
{
    return str ? str : def;
}

static int parse_enum_value( const char *arg, const char * const *names, int *dst )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
        {
            *dst = i;
            return 0;
        }
    return -1;
}

obe_input_t device = {0};
obe_input_program_t program = {0};
obe_output_stream_t *output_streams;
obe_mux_opts_t mux_opts = {0};
obe_output_opts_t output = {0};

/* set functions - TODO add lots more opts */
static int set_muxer( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    static const char *optlist[] = { "ts-type", "ts-muxrate", "cbr", NULL };

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "mpegts" ) )
        mux_opts.muxer = MUXERS_MPEGTS;
    else if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, optlist );
        if( !opts && params )
            return -1;

        char *ts_type = obe_get_option( optlist[0], opts );
        char *ts_muxrate  = obe_get_option( optlist[1], opts );
        char *ts_cbr      = obe_get_option( optlist[2], opts );
        if( ts_type )
            parse_enum_value( ts_type, ts_types, &mux_opts.ts_type );
        if( ts_muxrate )
            mux_opts.ts_muxrate = obe_otoi( ts_muxrate, 0 );
        if( ts_cbr )
            mux_opts.cbr = obe_otob( ts_cbr, 0 );
    }

    return 0;
}

static int set_stream( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    static const char *optlist[] = { "vbv-maxrate", "vbv-bufsize", "bitrate", NULL };
    int str_len = strlen( command );
    int id_len = strcspn( command, ":" );
    command[id_len] = 0;

    int stream_id = obe_otoi( command, -1 );

    if( stream_id < 0 || stream_id > program.num_streams-1 )
    {
        fprintf( stderr, "invalid stream id\n" );
        return -1;
    }

    if( str_len > id_len )
    {
        char *params = command + id_len + 1;
        char **opts = obe_split_options( params, optlist );
        if( !opts && params )
            return -1;

        char *vbv_maxrate = obe_get_option( optlist[0], opts );
        char *vbv_bufsize = obe_get_option( optlist[1], opts );
        char *bitrate     = obe_get_option( optlist[2], opts );
        output_streams[stream_id].avc_param.rc.i_vbv_max_bitrate = obe_otoi( vbv_maxrate, 0 );
        output_streams[stream_id].avc_param.rc.i_vbv_buffer_size = obe_otoi( vbv_bufsize, 0 );
        output_streams[stream_id].avc_param.rc.i_bitrate         = obe_otoi( bitrate, 0 );
    }

    return 0;
}

static int set_output( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    static const char *optlist[] = { "location", NULL };

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "udp" ) )
        output.output = OUTPUT_UDP;
    else if( !strcasecmp( command, "rtp" ) )
        output.output = OUTPUT_RTP;
    else if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, optlist );
        if( !opts && params )
            return -1;

        char *location = obe_get_option( optlist[0], opts );
        if( location )
        {
             if( output.location )
                 free( output.location );

             output.location = malloc( strlen( location ) + 1 );
             FAIL_IF_ERROR( !output.location, "malloc failed\n" );
             strcpy( output.location, location );
        }
    }
    else
    {
        fprintf( stderr, "Invalid output %s\n", command );
        return -1;
    }

    return 0;
}


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

#define H0 printf
    H0( "OBE Commands:\n" );

    H0( "\n" );

    H0( "show - Show supported items\n" );

    for( int i = 0; show_commands[i].name != 0; i++ )
        H0( "       %-*s          - %s \n", 8, show_commands[i].name, show_commands[i].description );

    H0( "\n" );

#if 0
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
#endif
    H0( "\n" );

//    H0( "load - Load configuration\n" );

    H0( "set  - Set parameter\n" );
    for( int i = 0; set_commands[i].name != 0; i++ )
        H0( "       %-*s          - %s \n", 8, set_commands[i].name, set_commands[i].description );

    H0( "\n" );

    H0( "Starting/Stopping OBE:\n" );
    H0( "start - Start encoding\n" );
    //H0( "stop  - Stop encoding\n" );

    H0( "\n" );

    return 0;
}

static int show_inputs( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Inputs: \n" );

    while( input_names[i].input_name )
    {
        printf( "       %-*s          - %s \n", 8, input_names[i].input_name, input_names[i].input_lib_name );
        i++;
    }

    return 0;
}

static int show_muxers( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Muxers: \n" );

    while( muxer_names[i].muxer_name )
    {
        printf( "       %-*s          - %s \n", 8, muxer_names[i].muxer_name, muxer_names[i].mux_lib_name );
        i++;
    }

    return 0;
}

static int show_outputs( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Outputs: \n" );

    while( output_names[i].output_name )
    {
        printf( "       %-*s          - %s \n", 8, output_names[i].output_name, output_names[i].output_lib_name );
        i++;
    }

    return 0;
}

static int start_encode( char *command, obecli_command_t *child )
{
    if( !program.num_streams )
    {
        fprintf( stderr, "No active devices \n" );
        return -1;
    }

    if( !output.location )
    {
        fprintf( stderr, "No output location \n" );
        return -1;
    }

    for( int i = 0; i < program.num_streams; i++ )
    {
        output_streams[i].stream_id = program.streams[i].stream_id;
        if( program.streams[i].stream_type == STREAM_TYPE_VIDEO )
        {
            output_streams[i].stream_action = STREAM_ENCODE;
            output_streams[i].stream_format = VIDEO_AVC;
        }
        else
            output_streams[i].stream_action = STREAM_PASSTHROUGH;
        output_streams[i].ts_opts.passthrough_opts = 1;
    }

    obe_setup_streams( h, output_streams, program.num_streams );
    mux_opts.passthrough = 1;
    obe_setup_muxer( h, &mux_opts );
    if( obe_start( h ) < 0 )
        return -1;

    return 0;
}

static char *get_format_shortname( int stream_format, const obecli_format_name_t *names )
{
    int i = 0;

    while( names[i].format_name != 0 && names[i].format != stream_format )
        i++;

    return names[i].format_name;
}

static int probe_device( char *command, obecli_command_t *child )
{
    obe_input_stream_t *stream;
    char buf[200];
    char *format_name;

    if( !strlen( command ) )
        return -1;

    device.input_type = INPUT_URL;
    device.location = command;

    obe_probe_device( h, &device, &program );

    printf("\n");

    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        format_name = get_format_shortname( stream->stream_format, format_names );
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
            printf( "Stream-id: %d - Teletext: Language: %s \n", stream->stream_id, stream->lang_code );
        }
        else
            printf( "Stream-id: %d \n", stream->stream_id );
    }

    printf("\n");

    if( program.num_streams )
    {
        output_streams = calloc( 1, program.num_streams * sizeof(*output_streams) );
        if( !output_streams )
        {
            fprintf( stderr, "Malloc failed \n" );
            return -1;
        }
        for( int i = 0; i < program.num_streams; i++ )
        {
            if( program.streams[i].stream_type == STREAM_TYPE_VIDEO )
                obe_populate_avc_encoder_params( h, program.streams[i].stream_id, &(output_streams[i].avc_param) );
        }
    }

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
                fprintf( stderr, "%s: command not found \n", line_read );
        }
    }

    obe_close( h );

    return 0;
}
