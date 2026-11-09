/***************************************************************************
 * Gens: Debugger - Cell Dump Functions. (C version)                       *
 *                                                                         *
 * Copyright (c) 1999-2002 by Stéphane Dallongeville                       *
 * Copyright (c) 2003-2004 by Stéphane Akhoun                              *
 * Copyright (c) 2008 by David Korth                                       *
 * C port for 64-bit builds: Copyright (c) 2026 by Tetsuro Mori            *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.           *
 ***************************************************************************/

/**
 * C port of mcd_cell_dump.asm.
 * Draws VDP / SegaCD Word RAM patterns into MD_Screen for the debugger.
 * The source data is stored as host-endian 16-bit words, like VRam and
 * Ram_Word_2M; each 8-pixel row is two words (pixels 0-3, pixels 4-7).
 */

#include "mcd_cell_dump.h"

#include <stdint.h>
#include <string.h>

#include "gens_core/vdp/vdp_rend.h"

// MD bpp. (Defined in emulator/g_main.cpp; g_main.hpp is a C++ header.)
extern uint8_t bppMD;

#define SCREEN_PITCH	336
#define SCREEN_ORIGIN	(((10 + 8) * SCREEN_PITCH) + 30)

/**
 * draw_rows(): Draw an 8-pixel-wide strip of rows.
 * @param src Source pattern data (4 bytes per row).
 * @param x Destination X (pixels from SCREEN_ORIGIN).
 * @param y Destination Y (lines from SCREEN_ORIGIN).
 * @param rows Number of rows.
 * @param pal Palette number.
 * @return Source pointer after the drawn rows.
 */
static const unsigned char *draw_rows(const unsigned char *src, int x, int y, int rows, int pal)
{
	static const int shifts[8] = {12, 8, 4, 0, 28, 24, 20, 16};
	const unsigned int pal_base = (unsigned int)pal * 16;
	int row, px;

	for (row = 0; row < rows; row++, src += 4)
	{
		uint16_t w[2];
		uint32_t data;
		unsigned int dst = SCREEN_ORIGIN + ((y + row) * SCREEN_PITCH) + x;

		memcpy(w, src, sizeof(w));
		data = (uint32_t)w[0] | ((uint32_t)w[1] << 16);

		if (bppMD == 32)
		{
			for (px = 0; px < 8; px++)
				MD_Screen.u32[dst + px] = MD_Palette.u32[(pal_base + ((data >> shifts[px]) & 0xF)) & 0xFF];
		}
		else
		{
			for (px = 0; px < 8; px++)
				MD_Screen.u16[dst + px] = MD_Palette.u16[(pal_base + ((data >> shifts[px]) & 0xF)) & 0xFF];
		}
	}

	return src;
}


/**
 * dump_columns(): Draw rows of 16 columns, each column 'height' rows tall,
 * with the source data read sequentially. (8x8, 8x16 and 32x32 dumps.)
 */
static void dump_columns(const unsigned char *src, int pal, int screen_rows, int height)
{
	int r, c;

	for (r = 0; r < screen_rows; r++)
	{
		for (c = 0; c < 16; c++)
			src = draw_rows(src, c * 8, r * height, height, pal);
	}
}


/** Cell_8x8_Dump(): MD VDP patterns, normal. **/
void Cell_8x8_Dump(unsigned char *Adr, int pal)
{
	dump_columns(Adr, pal, 24, 8);
}


/** Cell_8x16_Dump(): MD VDP patterns, interlaced. **/
void Cell_8x16_Dump(unsigned char *Adr, int pal)
{
	dump_columns(Adr, pal, 12, 16);
}


/**
 * Cell_16x16_Dump(): SegaCD Word RAM, 16x16 stamps.
 * Each stamp is 4 cells (top-left, bottom-left, top-right, bottom-right);
 * a screen row draws the first and third cells of 16 consecutive cell
 * pairs, then the second and fourth, exactly like the asm version.
 */
void Cell_16x16_Dump(unsigned char *Adr, int pal)
{
	const unsigned char *src = Adr;
	int r, c;

	for (r = 0; r < 12; r++)
	{
		for (c = 0; c < 16; c++)
		{
			src = draw_rows(src, c * 8, r * 16, 8, pal);
			src += 0x20;
		}
		src -= (0x400 - 0x20);

		for (c = 0; c < 16; c++)
		{
			src = draw_rows(src, c * 8, (r * 16) + 8, 8, pal);
			src += 0x20;
		}
		src -= 0x20;
	}
}


/** Cell_32x32_Dump(): SegaCD Word RAM, 32x32 stamps. **/
void Cell_32x32_Dump(unsigned char *Adr, int pal)
{
	dump_columns(Adr, pal, 6, 32);
}
