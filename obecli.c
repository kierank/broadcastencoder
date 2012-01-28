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

#include "config.h"

#include <signal.h>
#define _GNU_SOURCE

#include <readline/readline.h>
#include <readline/history.h>

#include "obe.h"
#include "obecli.h"

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "obecli", __VA_ARGS__ )
#define RETURN_IF_ERROR( cond, ... ) RETURN_IF_ERR( cond, "options", NULL, __VA_ARGS__ )

typedef struct
{
    obe_t *h;
    obe_input_t input;
    obe_input_program_t program;
    obe_output_stream_t *output_streams;
    obe_mux_opts_t mux_opts;
    obe_output_opts_t output;
    int avc_profile;
} obecli_ctx_t;

obecli_ctx_t cli;

/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;
static char *line_read = NULL;

static int running = 0;
static int system_type_value = OBE_SYSTEM_TYPE_GENERIC;

static const char * const system_types[]             = { "generic", "lowlatency", 0 };
static const char * const input_types[]              = { "url", "decklink", "linsys-sdi", 0 };
static const char * const input_video_formats[]      = { "pal", "ntsc", "720p50", "720p59.94", "720p60", "1080i50", "1080i59.94", "1080i60",
                                                         "1080p23.98", "1080p24", "1080p25", "1080p29.97", "1080p30", "1080p50", "1080p59.94",
                                                         "1080p60", 0 };
static const char * const input_video_connections[]  = { "sdi", "hdmi", "optical-sdi", "component", "composite", "s-video", 0 };
static const char * const input_audio_connections[]  = { "embedded", "aes-ebu", "analogue", 0 };
static const char * const ttx_locations[]            = { "dvb-ttx", "dvb-vbi", "both", 0 };
static const char * const stream_actions[]           = { "passthrough", "encode", 0 };
static const char * const encode_formats[]           = { "", "avc", "", "", "mp2", "ac3", "e-ac3", "aac-experimental", 0 };
static const char * const frame_packing_modes[]      = { "none", "checkerboard", "column", "row", "side-by-side", "top-bottom", "temporal", 0 };
static const char * const teletext_types[]           = { "", "initial", "subtitle", "additional-info", "program-schedule", "hearing-imp", 0 };
static const char * const audio_types[]              = { "undefined", "clean-effects", "hearing-impaired", "visual-impaired", 0 };
static const char * const aac_encapsulations[]       = { "adts", "latm", 0 };
static const char * const output_modules[]           = { "udp", "rtp", "linsys-asi", 0 };

static const char * system_opts[] = { "system-type", NULL };
static const char * input_opts[]  = { "location", "card-idx", "video-format", "video-connection", "audio-connection", "ttx-location", NULL };
/* TODO: split the stream options into general options, video options, ts options */
static const char * stream_opts[] = { "action", "format",
                                      /* Encoding options */
                                      "vbv-maxrate", "vbv-bufsize", "bitrate", "sar-width", "sar-height",
                                      "profile", "level", "keyint", "lookahead", "threads", "bframes", "b-pyramid", "weightp",
                                      "interlaced", "tff", "frame-packing", "csp", "filler",
                                      /* AAC options */
                                      "aac-encap",
                                      /* TS options */
                                      "pid", "lang", "audio-type", "num-ttx", "ttx-lang", "ttx-type", "ttx-mag", "ttx-page",
                                      NULL };
static const char * muxer_opts[]  = { "ts-type", "cbr", "ts-muxrate", "passthrough", "ts-id", "program-num", "pmt-pid", "pcr-pid",
                                      "pcr-period", "pat-period", NULL };
static const char * ts_types[]    = { "generic", "dvb", "cablelabs", "atsc", "isdb", NULL };
static const char * output_opts[] = { "target", NULL };

void obe_cli_printf( const char *name, const char *fmt, ... )
{
    fprintf( stderr, "%s: ", name );
    va_list arg;
    va_start( arg, fmt );
    vfprintf( stderr, fmt, arg );
    va_end( arg );
}

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

static int check_enum_value( const char *arg, const char * const *names )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
            return 0;

    return -1;
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

/* set functions - TODO add lots more opts */
static int set_obe( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, system_opts );
        if( !opts && params )
            return -1;

        char *system_type     = obe_get_option( system_opts[0], opts );

        FAIL_IF_ERROR( system_type && ( check_enum_value( system_type, system_types ) < 0 ),
                       "Invalid system type\n" );

        FAIL_IF_ERROR( cli.program.num_streams, "Cannot change OBE options after probing\n" )

        if( system_type )
        {
            parse_enum_value( system_type, system_types, &system_type_value );
            obe_set_config( cli.h, system_type_value );
	}

        obe_free_string_array( opts );
    }

    return 0;
}

static int set_input( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, input_opts );
        if( !opts && params )
            return -1;

        char *location     = obe_get_option( input_opts[0], opts );
        char *card_idx     = obe_get_option( input_opts[1], opts );
        char *video_format = obe_get_option( input_opts[2], opts );
        char *video_connection = obe_get_option( input_opts[3], opts );
        char *audio_connection = obe_get_option( input_opts[4], opts );
        char *ttx_location = obe_get_option( input_opts[5], opts );

        FAIL_IF_ERROR( video_format && ( check_enum_value( video_format, input_video_formats ) < 0 ),
                       "Invalid video format\n" );

        FAIL_IF_ERROR( video_connection && ( check_enum_value( video_connection, input_video_connections ) < 0 ),
                       "Invalid video connection\n" );

        FAIL_IF_ERROR( audio_connection && ( check_enum_value( audio_connection, input_audio_connections ) < 0 ),
                       "Invalid audio connection\n" );

        FAIL_IF_ERROR( ttx_location && ( check_enum_value( ttx_location, ttx_locations ) < 0 ),
                       "Invalid teletext location\n" );

        if( location )
        {
             if( cli.input.location )
                 free( cli.input.location );

             cli.input.location = malloc( strlen( location ) + 1 );
             FAIL_IF_ERROR( !cli.input.location, "malloc failed\n" );
             strcpy( cli.input.location, location );
        }

        cli.input.card_idx = obe_otoi( card_idx, cli.input.card_idx );
        if( video_format )
            parse_enum_value( video_format, input_video_formats, &cli.input.video_format );
        if( video_connection )
            parse_enum_value( video_connection, input_video_connections, &cli.input.video_connection );
        if( audio_connection )
            parse_enum_value( audio_connection, input_audio_connections, &cli.input.audio_connection );
        if( ttx_location )
            parse_enum_value( ttx_location, ttx_locations, &cli.input.teletext_location );

        obe_free_string_array( opts );
    }
    else
    {
        FAIL_IF_ERROR( ( check_enum_value( command, input_types ) < 0 ), "Invalid input type\n" );
        parse_enum_value( command, input_types, &cli.input.input_type );
    }

    return 0;
}

static int set_stream( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        command += tok_len+1;
        int tok_len2 = strcspn( command, ":" );
        int str_len2 = strlen( command );
        command[tok_len2] = 0;

        int stream_id = obe_otoi( command, -1 );


        FAIL_IF_ERROR( stream_id < 0 || stream_id > cli.program.num_streams-1,
                       "Invalid stream id\n" );

        if( str_len > str_len2 )
        {
            char *params = command + tok_len2 + 1;
            char **opts = obe_split_options( params, stream_opts );
            if( !opts && params )
                return -1;

            char *action      = obe_get_option( stream_opts[0], opts );
            char *format      = obe_get_option( stream_opts[1], opts );
            char *vbv_maxrate = obe_get_option( stream_opts[2], opts );
            char *vbv_bufsize = obe_get_option( stream_opts[3], opts );
            char *bitrate     = obe_get_option( stream_opts[4], opts );
            char *sar_width   = obe_get_option( stream_opts[5], opts );
            char *sar_height  = obe_get_option( stream_opts[6], opts );
            char *profile     = obe_get_option( stream_opts[7], opts );
            char *level       = obe_get_option( stream_opts[8], opts );
            char *keyint      = obe_get_option( stream_opts[9], opts );
            char *lookahead   = obe_get_option( stream_opts[10], opts );
            char *threads     = obe_get_option( stream_opts[11], opts );
            char *bframes     = obe_get_option( stream_opts[12], opts );
            char *b_pyramid   = obe_get_option( stream_opts[13], opts );
            char *weightp     = obe_get_option( stream_opts[14], opts );
            char *interlaced  = obe_get_option( stream_opts[15], opts );
            char *tff         = obe_get_option( stream_opts[16], opts );
            char *frame_packing = obe_get_option( stream_opts[17], opts );
            char *csp         = obe_get_option( stream_opts[18], opts );
            char *filler      = obe_get_option( stream_opts[19], opts );

            char *aac_encap   = obe_get_option( stream_opts[20], opts );

            /* NB: remap these and the ttx values below if more encoding options are added - TODO: split them up */
            char *pid         = obe_get_option( stream_opts[21], opts );
            char *lang        = obe_get_option( stream_opts[22], opts );
            char *audio_type  = obe_get_option( stream_opts[23], opts );

            if( cli.program.streams[stream_id].stream_type == STREAM_TYPE_VIDEO )
            {
                x264_param_t *avc_param = &cli.output_streams[stream_id].avc_param;

                FAIL_IF_ERROR( profile && ( check_enum_value( profile, x264_profile_names ) < 0 ),
                               "Invalid AVC profile\n" );

                FAIL_IF_ERROR( vbv_bufsize && system_type_value == OBE_SYSTEM_TYPE_LOW_LATENCY,
                               "VBV buffer size is not user-settable in low-latency mode\n" );

                FAIL_IF_ERROR( frame_packing && ( check_enum_value( frame_packing, frame_packing_modes ) < 0 ),
                               "Invalid frame packing mode\n" )

                /* Set it to encode by default */
                cli.output_streams[stream_id].stream_action = STREAM_ENCODE;
                cli.output_streams[stream_id].stream_format = VIDEO_AVC;
                avc_param->rc.i_vbv_max_bitrate = obe_otoi( vbv_maxrate, 0 );
                avc_param->rc.i_vbv_buffer_size = obe_otoi( vbv_bufsize, 0 );
                avc_param->rc.i_bitrate         = obe_otoi( bitrate, 0 );
                avc_param->vui.i_sar_width      = obe_otoi( sar_width, avc_param->vui.i_sar_width );
                avc_param->vui.i_sar_height     = obe_otoi( sar_height, avc_param->vui.i_sar_height );

                if( profile )
                    parse_enum_value( profile, x264_profile_names, &cli.avc_profile );

                if( level )
                {
                    if( !strcasecmp( level, "1b" ) )
                        avc_param->i_level_idc = 9;
                    else if( obe_otof( level, 7.0 ) < 6 )
                        avc_param->i_level_idc = (int)( 10*obe_otof( level, 0.0 ) + .5 );
                    else
                        avc_param->i_level_idc = obe_otoi( level, avc_param->i_level_idc );
                }
                avc_param->i_keyint_max        = obe_otoi( keyint, avc_param->i_keyint_max );
                avc_param->rc.i_lookahead      = obe_otoi( lookahead, avc_param->rc.i_lookahead );
                avc_param->i_threads           = obe_otoi( threads, avc_param->i_threads );
                avc_param->i_bframe            = obe_otoi( bframes, avc_param->i_bframe );
                avc_param->i_bframe_pyramid    = obe_otoi( b_pyramid, avc_param->i_bframe_pyramid );
                avc_param->analyse.i_weighted_pred = obe_otoi( weightp, avc_param->analyse.i_weighted_pred );
                avc_param->b_interlaced        = obe_otob( interlaced, avc_param->b_interlaced );
                avc_param->b_tff               = obe_otob( tff, avc_param->b_tff );
                if( frame_packing )
                {
                    parse_enum_value( frame_packing, frame_packing_modes, &avc_param->i_frame_packing );
                    avc_param->i_frame_packing--;
                }

                if( csp )
                {
                    avc_param->i_csp = obe_otoi( csp, 420 ) == 422 || strcasecmp( csp, "4:2:2" ) ? X264_CSP_I422 : X264_CSP_I420;
                    if( X264_BIT_DEPTH == 10 )
                        avc_param->i_csp |= X264_CSP_HIGH_DEPTH;
                }

                if( filler ) 
                    avc_param->i_nal_hrd = obe_otob( filler, 0 ) ? X264_NAL_HRD_FAKE_CBR : X264_NAL_HRD_FAKE_VBR;

                /* Turn on the 3DTV mux option automatically */
                if( avc_param->i_frame_packing >= 0 )
                    cli.mux_opts.is_3dtv = 1;

            }
            else if( cli.program.streams[stream_id].stream_type == STREAM_TYPE_AUDIO )
            {
                int default_bitrate = 0;

                /* Set it to encode by default */
                cli.output_streams[stream_id].stream_action = STREAM_ENCODE;

                FAIL_IF_ERROR( action && ( check_enum_value( action, stream_actions ) < 0 ),
                              "Invalid stream action\n" );

                FAIL_IF_ERROR( format && ( check_enum_value( format, encode_formats ) < 0 ),
                              "Invalid stream format\n" );

                FAIL_IF_ERROR( aac_encap && ( check_enum_value( aac_encap, aac_encapsulations ) < 0 ),
                              "Invalid aac encapsulation\n" );

                FAIL_IF_ERROR( audio_type && check_enum_value( audio_type, audio_types ) < 0,
                              "Invalid audio type\n" );

                FAIL_IF_ERROR( audio_type && check_enum_value( audio_type, audio_types ) >= 0 &&
                               !cli.output_streams[stream_id].ts_opts.write_lang_code && !( lang && strlen( lang ) >= 3 ),
                               "Audio type requires setting a language\n" );

                if( action )
                    parse_enum_value( action, stream_actions, &cli.output_streams[stream_id].stream_action );
                if( format )
                    parse_enum_value( format, encode_formats, &cli.output_streams[stream_id].stream_format );
                if( audio_type )
                    parse_enum_value( audio_type, audio_types, &cli.output_streams[stream_id].ts_opts.audio_type );

                if( cli.output_streams[stream_id].stream_format == AUDIO_MP2 )
                    default_bitrate = 256;
                else if( cli.output_streams[stream_id].stream_format == AUDIO_AC_3 )
                    default_bitrate = 192;
                else if( cli.output_streams[stream_id].stream_format == AUDIO_E_AC_3 )
                    default_bitrate = 192;
                else // AAC
                {
                    default_bitrate = 128;

                    if( aac_encap )
                        parse_enum_value( aac_encap, aac_encapsulations, &cli.output_streams[stream_id].aac_opts.latm_output );
                }

                cli.output_streams[stream_id].bitrate = obe_otoi( bitrate, default_bitrate );

                if( lang && strlen( lang ) >= 3 )
                {
                    cli.output_streams[stream_id].ts_opts.write_lang_code = 1;
                    memcpy( cli.output_streams[stream_id].ts_opts.lang_code, lang, 3 );
                    cli.output_streams[stream_id].ts_opts.lang_code[3] = 0;
                }
            }
            else if( cli.program.streams[stream_id].stream_format == MISC_TELETEXT ||
                     cli.program.streams[stream_id].stream_format == VBI_RAW )
            {
                /* NB: remap these if more encoding options are added - TODO: split them up */
                char *ttx_lang = obe_get_option( stream_opts[25], opts );
                char *ttx_type = obe_get_option( stream_opts[26], opts );
                char *ttx_mag  = obe_get_option( stream_opts[27], opts );
                char *ttx_page = obe_get_option( stream_opts[28], opts );

                FAIL_IF_ERROR( ttx_type && ( check_enum_value( ttx_type, teletext_types ) < 0 ),
                               "Invalid Teletext type\n" );

                /* TODO: find a nice way of supporting multiple teletexts in the CLI */
                cli.output_streams[stream_id].ts_opts.num_teletexts = 1;

                if( cli.output_streams[stream_id].ts_opts.teletext_opts )
                    free( cli.output_streams[stream_id].ts_opts.teletext_opts );

                cli.output_streams[stream_id].ts_opts.teletext_opts = calloc( 1, sizeof(*cli.output_streams[stream_id].ts_opts.teletext_opts) );
                FAIL_IF_ERROR( !cli.output_streams[stream_id].ts_opts.teletext_opts, "malloc failed\n" );

                obe_teletext_opts_t *ttx_opts = &cli.output_streams[stream_id].ts_opts.teletext_opts[0];

                if( ttx_lang && strlen( ttx_lang ) >= 3 )
                {
                    memcpy( ttx_opts->dvb_teletext_lang_code, ttx_lang, 3 );
                    ttx_opts->dvb_teletext_lang_code[3] = 0;
                }
                if( ttx_type )
                    parse_enum_value( ttx_type, teletext_types, &ttx_opts->dvb_teletext_type );
                ttx_opts->dvb_teletext_magazine_number = obe_otoi( ttx_mag, ttx_opts->dvb_teletext_magazine_number );
                ttx_opts->dvb_teletext_page_number = obe_otoi( ttx_page, ttx_opts->dvb_teletext_page_number );
            }

            cli.output_streams[stream_id].ts_opts.pid = obe_otoi( pid, cli.output_streams[stream_id].ts_opts.pid );
            obe_free_string_array( opts );
        }
    }

    return 0;
}

static int set_muxer( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "mpegts" ) )
        cli.mux_opts.muxer = MUXERS_MPEGTS;
    else if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, muxer_opts );
        if( !opts && params )
            return -1;

        char *ts_type     = obe_get_option( muxer_opts[0], opts );
        char *ts_cbr      = obe_get_option( muxer_opts[1], opts );
        char *ts_muxrate  = obe_get_option( muxer_opts[2], opts );
        char *passthrough = obe_get_option( muxer_opts[3], opts );
        char *ts_id       = obe_get_option( muxer_opts[4], opts );
        char *program_num = obe_get_option( muxer_opts[5], opts );
        char *pmt_pid     = obe_get_option( muxer_opts[6], opts );
        char *pcr_pid     = obe_get_option( muxer_opts[7], opts );
        char *pcr_period  = obe_get_option( muxer_opts[8], opts );
        char *pat_period  = obe_get_option( muxer_opts[9], opts );

        FAIL_IF_ERROR( ts_type && ( check_enum_value( ts_type, ts_types ) < 0 ),
                      "Invalid AVC profile\n" );

        if( ts_type )
            parse_enum_value( ts_type, ts_types, &cli.mux_opts.ts_type );

        cli.mux_opts.cbr = obe_otob( ts_cbr, cli.mux_opts.cbr );
        cli.mux_opts.ts_muxrate = obe_otoi( ts_muxrate, cli.mux_opts.ts_muxrate );

        cli.mux_opts.passthrough = obe_otob( passthrough, cli.mux_opts.passthrough );
        cli.mux_opts.ts_id = obe_otoi( ts_id, cli.mux_opts.ts_id );
        cli.mux_opts.program_num = obe_otoi( program_num, cli.mux_opts.program_num );
        cli.mux_opts.pmt_pid    = obe_otoi( pmt_pid, cli.mux_opts.pmt_pid );
        cli.mux_opts.pcr_pid    = obe_otoi( pcr_pid, cli.mux_opts.pcr_pid  );
        cli.mux_opts.pcr_period = obe_otoi( pcr_period, cli.mux_opts.pcr_period );
        cli.mux_opts.pat_period = obe_otoi( pat_period, cli.mux_opts.pat_period );
        obe_free_string_array( opts );
    }

    return 0;
}

static int set_output( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, output_opts );
        if( !opts && params )
            return -1;

        char *target = obe_get_option( output_opts[0], opts );
        if( target )
        {
             if( cli.output.target )
                 free( cli.output.target );

             cli.output.target = malloc( strlen( target ) + 1 );
             FAIL_IF_ERROR( !cli.output.target, "malloc failed\n" );
             strcpy( cli.output.target, target );
        }
        obe_free_string_array( opts );
    }
    else
    {
        FAIL_IF_ERROR( parse_enum_value( command, output_modules, &cli.output.output ) < 0,
                       "Invalid output %s\n", command )
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
    printf( "\nSupported Decoders: \n" );

    for( int i = 0; format_names[i].decoder_name != 0; i++ )
    {
        if( strcmp( format_names[i].decoder_name, "N/A" ) )
            printf( "       %-*s %-*s - %s \n", 7, format_names[i].format_name, 22, format_names[i].long_name, format_names[i].decoder_name );
    }

    return 0;
}

static int show_encoders( char *command, obecli_command_t *child )
{
    printf( "\nSupported Encoders: \n" );

    for( int i = 0; format_names[i].encoder_name != 0; i++ )
    {
        if( strcmp( format_names[i].encoder_name, "N/A" ) )
            printf( "       %-*s %-*s - %s \n", 7, format_names[i].format_name, 22, format_names[i].long_name, format_names[i].encoder_name );
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

    /* TODO: stream selection */
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

    H0( "\n" );

    H0( "load - Load configuration\n" );

#endif

    H0( "set  - Set parameter\n" );
    for( int i = 0; set_commands[i].name != 0; i++ )
        H0( "       %-*s %-*s  - %s \n", 8, set_commands[i].name, 21, set_commands[i].child_opts, set_commands[i].description );

    H0( "\n" );

    H0( "Starting/Stopping OBE:\n" );
    H0( "start - Start encoding\n" );
    H0( "stop  - Stop encoding\n" );

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
    FAIL_IF_ERROR( running, "Encoder already running\n" );
    FAIL_IF_ERROR( !cli.program.num_streams, "No active devices\n" );

    for( int i = 0; i < cli.program.num_streams; i++ )
    {
        if( cli.program.streams[i].stream_type == STREAM_TYPE_VIDEO )
        {
            /* x264 calculates the single-frame VBV size later on */
            FAIL_IF_ERROR( system_type_value == OBE_SYSTEM_TYPE_GENERIC && !cli.output_streams[i].avc_param.rc.i_vbv_buffer_size,
                           "No VBV buffer size chosen\n" );

            cli.output_streams[i].stream_action = STREAM_ENCODE;
            cli.output_streams[i].stream_format = VIDEO_AVC;
            if( cli.avc_profile >= 0 )
                x264_param_apply_profile( &cli.output_streams[i].avc_param, x264_profile_names[cli.avc_profile] );
        }
        else if( cli.program.streams[i].stream_type == STREAM_TYPE_AUDIO )
        {
            if( cli.output_streams[i].stream_action == STREAM_PASSTHROUGH && cli.program.streams[i].stream_format == AUDIO_PCM &&
                cli.output_streams[i].stream_format != AUDIO_MP2 && cli.output_streams[i].stream_format != AUDIO_AC_3 &&
                cli.output_streams[i].stream_format != AUDIO_AAC )
            {
                fprintf( stderr, "Uncompressed audio cannot yet be placed in TS\n" );
                return -1;
            }
            else if( cli.output_streams[i].stream_action == STREAM_ENCODE && !cli.output_streams[i].bitrate )
            {
                fprintf( stderr, "Audio stream requires bitrate\n" );
                return -1;
            }
        }
        else if( cli.program.streams[i].stream_format == MISC_TELETEXT || cli.program.streams[i].stream_format == VBI_RAW )
        {
            int found = cli.program.streams[i].stream_format == MISC_TELETEXT;

            /* Search the VBI streams for teletext and complain if teletext isn't set up properly */
            if( cli.program.streams[i].stream_format == VBI_RAW )
            {
                for( int j = 0; j < cli.program.streams[i].num_frame_data; j++ )
                {
                    if( cli.program.streams[i].frame_data[j].type == MISC_TELETEXT )
                    {
                        found = 1;
                        break;
                    }
                }
            }

            FAIL_IF_ERROR( found && !cli.output_streams[i].ts_opts.num_teletexts,
                           "Teletext stream setup is mandatory\n" );
        }
    }

    FAIL_IF_ERROR( !cli.mux_opts.ts_muxrate, "No mux rate selected\n" );

    if( ( cli.output.output == OUTPUT_UDP || cli.output.output == OUTPUT_RTP ) && !cli.output.target )
    {
        fprintf( stderr, "No output target chosen\n" );
        return -1;
    }

    obe_setup_streams( cli.h, cli.output_streams, cli.program.num_streams );
    obe_setup_muxer( cli.h, &cli.mux_opts );
    obe_setup_output( cli.h, &cli.output );
    if( obe_start( cli.h ) < 0 )
        return -1;

    running = 1;
    printf( "Encoding started\n" );

    return 0;
}

static int stop_encode( char *command, obecli_command_t *child )
{
    obe_close( cli.h );

    return 0;
}

static char *get_format_name( int stream_format, const obecli_format_name_t *names, int long_name )
{
    int i = 0;

    while( names[i].format_name != 0 && names[i].format != stream_format )
        i++;

    return  long_name ? names[i].long_name : names[i].format_name;
}

static int probe_device( char *command, obecli_command_t *child )
{
    obe_input_stream_t *stream;
    char buf[200];
    char *format_name;

    if( !strlen( command ) )
        return -1;

    FAIL_IF_ERROR( strcasecmp( command, "input" ), "%s is not a valid item to probe\n", command )

    /* TODO check for validity */

    if( obe_probe_device( cli.h, &cli.input, &cli.program ) < 0 )
        return -1;

    printf("\n");

    for( int i = 0; i < cli.program.num_streams; i++ )
    {
        stream = &cli.program.streams[i];
        format_name = get_format_name( stream->stream_format, format_names, 0 );
        if( stream->stream_type == STREAM_TYPE_VIDEO )
        {
            /* TODO: show profile, level, csp etc */
            printf( "Stream-id: %d - Video: %s %dx%d%s %d/%dfps \n", stream->stream_id,
                    format_name, stream->width, stream->height, stream->interlaced ? "i" : "p",
                    stream->timebase_den, stream->timebase_num );

            for( int j = 0; j < stream->num_frame_data; j++ )
            {
                format_name = get_format_name( stream->frame_data[j].type, format_names, 1 );
                /* TODO make this use the proper names */
                printf( "               %s:   %s\n", stream->frame_data[j].source == MISC_WSS ? "WSS (converted)" :
                        stream->frame_data[j].source == VBI_RAW ? "VBI" : stream->frame_data[j].source == VBI_VIDEO_INDEX ? "VII" : "VANC", format_name );
            }
        }
        else if( stream->stream_type == STREAM_TYPE_AUDIO )
        {
            /* let it work out the number of channels from the channel map */
            av_get_channel_layout_string( buf, 200, 0, stream->channel_layout );
            printf( "Stream-id: %d - Audio: %s%s %s %ikbps %ikHz Language: %s \n", stream->stream_id, format_name,
                    stream->stream_format == AUDIO_AAC ? stream->aac_is_latm ? " LATM" : " ADTS" : "",
                    buf, stream->bitrate / 1000, stream->sample_rate / 1000, strlen( stream->lang_code ) ? stream->lang_code : "none" );
        }
        else if( stream->stream_format == SUBTITLES_DVB )
        {
            printf( "Stream-id: %d - DVB Subtitles: Language: %s DDS: %s \n", stream->stream_id, stream->lang_code,
                    stream->dvb_has_dds ? "yes" : "no" );
        }
        else if( stream->stream_format == MISC_TELETEXT )
        {
            printf( "Stream-id: %d - DVB Teletext: \n", stream->stream_id );
        }
        else if( stream->stream_format == VBI_RAW )
        {
            printf( "Stream-id: %d - DVB-VBI: \n", stream->stream_id );
            for( int j = 0; j < stream->num_frame_data; j++ )
            {
                format_name = get_format_name( stream->frame_data[j].type, format_names, 1 );
                printf( "               %s:   %s\n", stream->frame_data[j].source == VBI_RAW ? "VBI" : "", format_name );
            }
        }
        else
            printf( "Stream-id: %d \n", stream->stream_id );
    }

    printf("\n");

    if( cli.program.num_streams )
    {
        if( cli.output_streams )
            free( cli.output_streams );

        cli.output_streams = calloc( 1, cli.program.num_streams * sizeof(*cli.output_streams) );
        if( !cli.output_streams )
        {
            fprintf( stderr, "Malloc failed \n" );
            return -1;
        }
        for( int i = 0; i < cli.program.num_streams; i++ )
        {
            cli.output_streams[i].stream_id = cli.program.streams[i].stream_id;
            if( cli.program.streams[i].stream_type == STREAM_TYPE_VIDEO )
                obe_populate_avc_encoder_params( cli.h, cli.program.streams[i].stream_id, &(cli.output_streams[i].avc_param) );
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
        commmand_list[i].cmd_func( command+tok_len+(str_len > tok_len), commmand_list[i].child_commands );
    else
        return -1;

    return 0;
}

int main( int argc, char **argv )
{
    char *home_dir = getenv( "HOME" );
    char *history_filename;
    char *prompt = "obecli> ";

    history_filename = malloc( strlen( home_dir ) + 16 + 1 );
    if( !history_filename )
    {
        fprintf( stderr, "malloc failed\n" );
        return -1;
    }

    sprintf( history_filename, "%s/.obecli_history", home_dir );
    read_history( history_filename );

    cli.h = obe_setup();
    if( !cli.h )
    {
        fprintf( stderr, "obe_setup failed\n" );
        return -1;
    }

    cli.avc_profile = -1;

    printf( "\nOpen Broadcast Encoder command line interface.\n" );
    printf( "Version 0.1-beta \n" );
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
            if( !strcasecmp( line_read, "exit" ) ||
                !strcasecmp( line_read, "quit" ) )
            {
                free( line_read );
                break;
            }

            add_history( line_read );

            int ret = parse_command( line_read, main_commands );
            if( ret == -1 )
                fprintf( stderr, "%s: command not found \n", line_read );
        }
    }

    write_history( history_filename );
    free( history_filename );

    if( cli.output_streams )
        free( cli.output_streams );

    if( cli.h )
        obe_close( cli.h );

    return 0;
}
