/*****************************************************************************
 * dither.h : OBE video dithering tables
 *****************************************************************************
 * Copyright (C) 2010 FFmpeg project
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

#ifndef OBE_FILTERS_DITHER_H
#define OBE_FILTERS_DITHER_H

DECLARE_ALIGNED(16, const uint16_t, obe_dithers)[8][8] =
{
    { 1,  2,  1,  2,  1,  2,  1,  2 },
    { 3,  0,  3,  0,  3,  0,  3,  0 },
    { 1,  2,  1,  2,  1,  2,  1,  2 },
    { 3,  0,  3,  0,  3,  0,  3,  0 },
    { 1,  2,  1,  2,  1,  2,  1,  2 },
    { 3,  0,  3,  0,  3,  0,  3,  0 },
    { 1,  2,  1,  2,  1,  2,  1,  2 },
    { 3,  0,  3,  0,  3,  0,  3,  0 },
};

uint16_t obe_dither_scale[15][16] =
{
{    2,    3,    3,    5,    5,    5,    5,    5,    5,    5,    5,    5,    5,    5,    5,    5,},
{    2,    3,    7,    7,   13,   13,   25,   25,   25,   25,   25,   25,   25,   25,   25,   25,},
{    3,    3,    4,   15,   15,   29,   57,   57,   57,  113,  113,  113,  113,  113,  113,  113,},
{    3,    4,    4,    5,   31,   31,   61,  121,  241,  241,  241,  241,  481,  481,  481,  481,},
{    3,    4,    5,    5,    6,   63,   63,  125,  249,  497,  993,  993,  993,  993,  993, 1985,},
{    3,    5,    6,    6,    6,    7,  127,  127,  253,  505, 1009, 2017, 4033, 4033, 4033, 4033,},
{    3,    5,    6,    7,    7,    7,    8,  255,  255,  509, 1017, 2033, 4065, 8129,16257,16257,},
{    3,    5,    6,    8,    8,    8,    8,    9,  511,  511, 1021, 2041, 4081, 8161,16321,32641,},
{    3,    5,    7,    8,    9,    9,    9,    9,   10, 1023, 1023, 2045, 4089, 8177,16353,32705,},
{    3,    5,    7,    8,   10,   10,   10,   10,   10,   11, 2047, 2047, 4093, 8185,16369,32737,},
{    3,    5,    7,    8,   10,   11,   11,   11,   11,   11,   12, 4095, 4095, 8189,16377,32753,},
{    3,    5,    7,    9,   10,   12,   12,   12,   12,   12,   12,   13, 8191, 8191,16381,32761,},
{    3,    5,    7,    9,   10,   12,   13,   13,   13,   13,   13,   13,   14,16383,16383,32765,},
{    3,    5,    7,    9,   10,   12,   14,   14,   14,   14,   14,   14,   14,   15,32767,32767,},
{    3,    5,    7,    9,   11,   12,   14,   15,   15,   15,   15,   15,   15,   15,   16,65535,},
};

#endif
