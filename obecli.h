/*****************************************************************************
 * obecli.h : Open Broadcast Encoder CLI headers
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

#ifndef OBECLI_H
#define OBECLI_H

typedef struct obecli_command_t obecli_command_t;

static int show_bitdepth( char *command, obecli_command_t *child );
static int show_decoders( char *command, obecli_command_t *child );
static int show_encoders( char *command, obecli_command_t *child );
static int show_help( char *command, obecli_command_t *child );
static int parse_command( char *command, obecli_command_t *commmand_list );
static int probe_device( char *command, obecli_command_t *child );

struct obecli_command_t
{
    char *name;
    char *child_opts;
    char *description;
    int (*cmd_func)( char*, obecli_command_t* );
    obecli_command_t *child_commands;
};

typedef struct
{
    int format;
    char *format_name;
    char *long_format_name;
    char *decoder_name;
    char *encoder_name;
}obecli_format_name_t;

typedef struct
{
    int muxer;
    char *format_name;
    char *long_name;
}obecli_muxer_name_t;

/* Commands */

static obecli_command_t add_commands[] =
{
#if 0
    //{"filter",  "",  "Show supported filters"  , show_bitdepth, NULL },
    {"output",  "",  "Show supported outputs"  , show_bitdepth, NULL },
#endif
    {0}
};

static obecli_command_t show_commands[] =
{
    {"bitdepth", "",  "Show AVC encoder bit depth", show_bitdepth, NULL },
    {"decoders", "",  "Show supported decoders" , show_decoders, NULL },
    {"encoders", "",  "Show supported encoders" , show_encoders, NULL },
    //{"filters",  "",  "Show supported filters"  , show_bitdepth, NULL },
    {"inputs",   "",  "Show supported inputs"   , show_bitdepth, NULL },
    {"muxers",   "",  "Show supported muxers"   , show_bitdepth, NULL },
    {"outputs",  "",  "Show supported outputs"  , show_bitdepth, NULL },
    {0}
};

static obecli_command_t main_commands[] =
{
    //{"add",   "[item] ...", "Add stream"                , parse_command, add_commands },
    {"help",  "[item] ...", "Display help"            , show_help, NULL },
    //{"set",   "[item] ...", "Set parameter"           , set_param, NULL },
    {"show",  "[item] ...", "Show item"               , parse_command, show_commands },
    //{"start", ""          , "Start encoding"          , start_encode, NULL },
    {"probe", ""          , "Probe input"            , probe_device, NULL },
    {0}
};

/* Format names */

static obecli_format_name_t format_names[] =
{
    { VIDEO_AVC,    "AVC",      "Advanced Video Coding", "FFmpeg AVC decoder",        "x264 encoder" },
    { VIDEO_MPEG2,  "MPEG-2",   "MPEG-2",                "FFmpeg MPEG-2 decoder",     NULL  },
    { AUDIO_PCM,    "PCM",      "Pulse Code Modulation (Raw audio)", "N/A",           NULL },
    { AUDIO_MP2,    "MP2",      "MPEG-1 Layer II",       "FFmpeg MP2 audio decoder",  "twolame encoder" },
    { AUDIO_AC_3,   "AC3",      "ATSC A/52B / AC-3",     "FFmpeg AC-3 audio decoder", "FFmpeg AC-3 encoder" },
    { AUDIO_E_AC_3, "E-AC3",    "ATSC A/52B Annex E / Enhanced AC-3", "FFmpeg E-AC3 audio decoder", NULL  },
//    { AUDIO_E_DIST, "E-Dist", "E-distribution audio" },
    { AUDIO_AAC,    "AAC",      "Advanced Audio Coding", "FFmpeg AAC decoder",        "Quicktime AAC encoder" },
    { SUBTITLES_DVB, "DVB-SUB", "DVB Subtitles", NULL, NULL },
    { MISC_TELETEXT, "DVB-TTX", "DVB Teletext", NULL, NULL },
    { 0, 0, 0 },
};

/* Muxer names */

#endif
