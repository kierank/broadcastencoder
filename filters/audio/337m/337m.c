/*****************************************************************************
 * 337m.c : SMPTE 337M functions
 *****************************************************************************
 * Copyright (C) 2010 NAMETBD
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

#define 337M_SYNCWORD_1_16_BIT 0xf872
#define 337M_SYNCWORD_1_20_BIT 0x6f872
#define 337M_SYNCWORD_1_24_BIT 0x96f872

#define 337M_SYNCWORD_2_16_BIT 0x4e1f
#define 337M_SYNCWORD_2_20_BIT 0x54e1f
#define 337M_SYNCWORD_2_24_BIT 0xa54e1f

#define 337M_DATA_TYPE_NULL      0
#define 337M_DATA_TYPE_AC_3      1
#define 337M_DATA_TYPE_TIMESTAMP 2
#define 337M_DATA_TYPE_MP2       6
#define 337M_DATA_TYPE_AAC       10
#define 337M_DATA_TYPE_HE_AAC    11
#define 337M_DATA_TYPE_E_AC_3    16
#define 337M_DATA_TYPE_E_DIST    28

// 337M startcode search function TODO
