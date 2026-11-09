/***************************************************************************
 * Gens: SegaCD graphics functions. (Rotation / scaling "stamp" ASIC)      *
 *                                                                         *
 * Copyright (c) 1999-2002 by Stéphane Dallongeville                       *
 * Copyright (c) 2003-2004 by Stéphane Akhoun                              *
 * Copyright (c) 2008-2009 by David Korth                                  *
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
 * C port of gfx_cd.asm.
 *
 * Ram_Word_2M is accessed with the same byte offsets as the x86 asm.
 * (Memory is stored as host-endian 16-bit words, so the "byte swapped"
 * XORs from the asm are kept as-is.)
 *
 * Stamp_Map_Adr, Buffer_Adr and Vector_Adr are byte offsets into
 * Ram_Word_2M instead of raw pointers, and Jmp_Adr is an index into
 * Table_Jump_Rot[]. All of them are masked on use, so out-of-range values
 * (e.g. from a savestate made by a 32-bit build, which stored pointers)
 * can't access memory outside of Ram_Word_2M.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>

#include "gfx_cd.h"
#include "gens_core/mem/mem_s68k.h"
#include "gens_core/cpu/68k/star_68k.h"

#if defined(__GNUC__)
#define GFX_CD_INLINE static inline __attribute__((always_inline))
#else
#define GFX_CD_INLINE static inline
#endif

// Ram_Word_2M is 256 KB.
#define W2M_MASK 0x3FFFF

/** Global variables. (Exported for savestates and the debugger.) **/

// Rot_Comp: Reg_58 (Stamp_Size), Reg_5A, Reg_5C, Reg_5E, Reg_60, Reg_62,
// Reg_64 (V_Dot), Reg_66, Rotation_Running.
struct Rot_Comp_t Rot_Comp;

uint32_t Stamp_Map_Adr;
uint32_t Buffer_Adr;
uint32_t Vector_Adr;
uint32_t Jmp_Adr;
int Float_Part;
int Draw_Speed;

int XS, YS, DXS, DYS, XD, YD, XD_Mul, H_Dot;

const int Table_Rot_Time[4 * 4 * 4] =
{
	0x00054000, 0x00048000, 0x00040000, 0x00036000,	// 008-032	; briefing - sprite
	0x0002E000, 0x00028000, 0x00024000, 0x00022000,	// 036-064	; arbre souvent
	0x00021000, 0x00020000, 0x0001E000, 0x0001B800,	// 068-096	; map thunderstrike
	0x00019800, 0x00017A00, 0x00015C00, 0x00013E00,	// 100-128	; logo défoncé

	0x00012000, 0x00011800, 0x00011000, 0x00010800,	// 132-160	; briefing - map
	0x00010000, 0x0000F800, 0x0000F000, 0x0000E800,	// 164-192
	0x0000E000, 0x0000D800, 0x0000D000, 0x0000C800,	// 196-224
	0x0000C000, 0x0000B800, 0x0000B000, 0x0000A800,	// 228-256	; batman visage

	0x0000A000, 0x00009F00, 0x00009E00, 0x00009D00,	// 260-288
	0x00009C00, 0x00009B00, 0x00009A00, 0x00009900,	// 292-320
	0x00009800, 0x00009700, 0x00009600, 0x00009500,	// 324-352
	0x00009400, 0x00009300, 0x00009200, 0x00009100,	// 356-384

	0x00009000, 0x00008F00, 0x00008E00, 0x00008D00,	// 388-416
	0x00008C00, 0x00008B00, 0x00008A00, 0x00008900,	// 420-448
	0x00008800, 0x00008700, 0x00008600, 0x00008500,	// 452-476
	0x00008400, 0x00008300, 0x00008200, 0x00008100,	// 480-512
};


/** Ram_Word_2M access helpers. (Byte offsets, same as the asm.) **/

static inline uint8_t w2m_read8(uint32_t offset)
{
	return Ram_Word_2M.u8[offset & W2M_MASK];
}

static inline uint32_t w2m_read16(uint32_t offset)
{
	// Same as the low 16 bits of an x86 dword read at this byte offset.
	return Ram_Word_2M.u16[(offset & W2M_MASK) >> 1];
}

static inline void w2m_write8(uint32_t offset, uint8_t data)
{
	Ram_Word_2M.u8[offset & W2M_MASK] = data;
}


/**
 * stamp_pixel(): Read one source pixel from a stamp. (asm: %%No_Flip_0 etc.)
 * @param base Byte offset of the stamp pattern + row/column offsets.
 * @param e Sub-cell coordinate: (coord & 0x3800) ^ 0x1000 or 0x2800.
 * @return Pixel (4-bit, not yet masked).
 */
static inline unsigned int stamp_pixel(uint32_t base, uint32_t e)
{
	// asm: shr edi, 12 -> CF = bit 11 (low nibble), edi = byte in the row.
	unsigned int pix = w2m_read8(base + (e >> 12));
	if (!(e & 0x800))
		pix >>= 4;
	return pix;
}


/**
 * make_image_pixel(): Draw one destination pixel. (asm: MAKE_IMAGE_PIXEL)
 * @param tiled 0 for normal; 1 for tiled (repeat).
 * @param dot32 0 for 16x16 dot stamps; 1 for 32x32 dot stamps.
 * @param scr16 0 for 1x1 screen; 1 for 16x16 screen.
 * @param prio Priority mode: 0 = OFF, 1 = Underwrite, 2 = Overwrite.
 * @param data Stamp data. (Only the low 16 bits are used.)
 * @param X X position. (11-bit fractional part)
 * @param Y Y position. (11-bit fractional part)
 */
GFX_CD_INLINE void make_image_pixel(const int tiled, const int dot32,
				    const int scr16, const int prio,
				    uint32_t data, uint32_t X, uint32_t Y)
{
	unsigned int pix = 0;
	uint32_t s, a, b;
	int out = 0;

	if (!tiled)
	{
		if (scr16)
		{
			// 16x16 screen
			if ((Y & 0x00800000) || (X & 0x00800000))
				out = 1;
		}
		else
		{
			// 1x1 screen
			if ((Y & 0x00F80000) || (X & 0x00F80000))
				out = 1;
		}
	}

	s = data & 0x07FF;
	if (!out && s != 0)	// Stamp 0 = 0 FILL
	{
		s <<= 7;
		switch ((data >> 13) & 7)
		{
			case 0:	// No_Flip_0
				if (dot32) {
					b = (Y >> 9) & 0x7C;
					a = (X >> 7) & 0x180;
				} else {
					b = (Y >> 9) & 0x3C;
					a = (X >> 8) & 0x40;
				}
				pix = stamp_pixel(s + a + b, (X & 0x3800) ^ 0x1000);
				break;

			case 1:	// No_Flip_90
				if (dot32) {
					a = (X >> 9) & 0x7C;
					b = ((Y >> 7) & 0x180) ^ 0x180;
				} else {
					a = (X >> 9) & 0x3C;
					b = ((Y >> 8) & 0x40) ^ 0x40;
				}
				pix = stamp_pixel(s + a + b, (Y & 0x3800) ^ 0x2800);
				break;

			case 2:	// No_Flip_180
				if (dot32) {
					b = ((Y >> 9) & 0x7C) ^ 0x7C;
					a = ((X >> 7) & 0x180) ^ 0x180;
				} else {
					b = ((Y >> 9) & 0x3C) ^ 0x3C;
					a = ((X >> 8) & 0x40) ^ 0x40;
				}
				pix = stamp_pixel(s + a + b, (X & 0x3800) ^ 0x2800);
				break;

			case 3:	// No_Flip_270
				if (dot32) {
					a = ((X >> 9) & 0x7C) ^ 0x7C;
					b = (Y >> 7) & 0x180;
				} else {
					a = ((X >> 9) & 0x3C) ^ 0x3C;
					b = (Y >> 8) & 0x40;
				}
				pix = stamp_pixel(s + a + b, (Y & 0x3800) ^ 0x1000);
				break;

			case 4:	// Flip_0
				if (dot32) {
					b = (Y >> 9) & 0x7C;
					a = ((X >> 7) & 0x180) ^ 0x180;
				} else {
					b = (Y >> 9) & 0x3C;
					a = ((X >> 8) & 0x40) ^ 0x40;
				}
				pix = stamp_pixel(s + a + b, (X & 0x3800) ^ 0x2800);
				break;

			case 5:	// Flip_90
				if (dot32) {
					a = ((X >> 9) & 0x7C) ^ 0x7C;	// FLIP
					b = ((Y >> 7) & 0x180) ^ 0x180;
				} else {
					a = ((X >> 9) & 0x3C) ^ 0x3C;	// FLIP
					b = ((Y >> 8) & 0x40) ^ 0x40;
				}
				pix = stamp_pixel(s + a + b, (Y & 0x3800) ^ 0x2800);
				break;

			case 6:	// Flip_180
				if (dot32) {
					b = ((Y >> 9) & 0x7C) ^ 0x7C;
					a = (X >> 7) & 0x180;
				} else {
					b = ((Y >> 9) & 0x3C) ^ 0x3C;
					a = (X >> 8) & 0x40;
				}
				pix = stamp_pixel(s + a + b, (X & 0x3800) ^ 0x1000);
				break;

			case 7:	// Flip_270
			default:
				if (dot32) {
					a = (X >> 9) & 0x7C;
					b = (Y >> 7) & 0x180;
				} else {
					a = (X >> 9) & 0x3C;
					b = (Y >> 8) & 0x40;
				}
				pix = stamp_pixel(s + a + b, (Y & 0x3800) ^ 0x1000);
				break;
		}
	}

	// %%Finish
	pix &= 0x0F;
	if (prio == 2 && pix == 0)
		return;		// Overwrite mode: transparent pixels are not written.

	{
		// "xor edi, 2" because byte swapped; bit 0 selects the nibble.
		const uint32_t xd = (uint32_t)XD ^ 2;
		const uint32_t offset = Buffer_Adr + (xd >> 1);
		uint8_t dst = w2m_read8(offset);

		if (!(xd & 1))
		{
			// First_Pix: high nibble.
			if (prio == 1 && (dst & 0xF0))
				return;	// Underwrite mode: don't overwrite pixels.
			dst = (uint8_t)((dst & 0x0F) + (pix << 4));
		}
		else
		{
			// Other_Pix: low nibble.
			if (prio == 1 && (dst & 0x0F))
				return;	// Underwrite mode: don't overwrite pixels.
			dst = (uint8_t)((dst & 0xF0) + pix);
		}
		w2m_write8(offset, dst);
	}
}


/**
 * make_image_line(): Draw one destination line. (asm: MAKE_IMAGE_LINE)
 * @param X X start. (11-bit fractional part)
 * @param Y Y start. (11-bit fractional part)
 * H_Dot must be non-zero.
 */
GFX_CD_INLINE void make_image_line(const int tiled, const int dot32,
				   const int scr16, const int prio,
				   uint32_t X, uint32_t Y)
{
	uint32_t a, b, xd;

	do
	{
		if (dot32)
		{
			if (scr16)
			{
				// 32x32 dot, 16x16 screen
				b = (Y >> (11 - 2)) & 0x3F80;
				a = (X & 0x007F0000) >> (11 + 5);
			}
			else
			{
				// 32x32 dot, 1x1 screen
				b = (Y >> (11 + 2)) & 0x38;
				a = (X & 0x00070000) >> (11 + 5);
			}
		}
		else
		{
			if (scr16)
			{
				// 16x16 dot, 16x16 screen
				b = (Y >> (11 - 4)) & 0xFF00;
				a = (X & 0x007F8000) >> (11 + 4);
			}
			else
			{
				// 16x16 dot, 1x1 screen
				b = (Y >> (11 + 0)) & 0xF0;
				a = (X & 0x00078000) >> (11 + 4);
			}
		}

		// Stamp number -> stamp data.
		make_image_pixel(tiled, dot32, scr16, prio,
				 w2m_read16(Stamp_Map_Adr + (b + a) * 2), X, Y);

		// %%Next_Pixel
		xd = (uint32_t)XD;
		X += (uint32_t)DXS;
		xd++;
		Y += (uint32_t)DYS;
		if (xd >= 8)
		{
			// Next cell column.
			Buffer_Adr += ((Rot_Comp.IB_V_Cell_Size & 0x001F) << 5) + 32;
			xd = 0;
		}

		// %%Same_Col
		H_Dot--;
		XD = (int)xd;
	} while (H_Dot != 0);
}


/**
 * make_image(): Draw one line of the destination image. (asm: MAKE_IMAGE)
 * @return Non-zero if the GFX operation is completed. (V_Dot reached 0)
 */
GFX_CD_INLINE int make_image(const int tiled, const int dot32,
			     const int scr16, const int prio)
{
	uint32_t X, Y, vec;
	uint8_t v_dot;

	XD = (int)(Rot_Comp.IB_Offset & 0x07);
	Buffer_Adr = ((Rot_Comp.IB_Adr & 0xFFF8) * 4) + ((uint32_t)YD << 2);
	H_Dot = (int)(Rot_Comp.IB_H_Dot_Size & 0x01FF);

	// Trace vector: X start, Y start, X delta, Y delta.
	vec = Vector_Adr;
	X = (w2m_read16(vec) << 8) & 0x00FFFF00;
	Y = w2m_read16(vec + 2) << 8;
	DXS = (int16_t)w2m_read16(vec + 4);
	DYS = (int16_t)w2m_read16(vec + 6);
	Vector_Adr = vec + 8;

	if (H_Dot & 0x1FF)
		make_image_line(tiled, dot32, scr16, prio, X, Y);

	// %%Nothing_To_Draw
	YD++;

	// dec byte [V_Dot] (low byte of Reg_64 only)
	v_dot = (uint8_t)(Rot_Comp.IB_V_Dot_Size - 1);
	Rot_Comp.IB_V_Dot_Size = (Rot_Comp.IB_V_Dot_Size & ~0xFFU) | v_dot;
	return (v_dot == 0);
}


/** Renderer variants. (asm: Table_Jump_Rot) **/

typedef int (*rot_fn)(void);

#define ROT_FN(name, tiled, dot32, scr16, prio) \
	static int name(void) { return make_image(tiled, dot32, scr16, prio); }

ROT_FN(Norm_D16_S1,	0, 0, 0, 0)
ROT_FN(Titled_D16_S1,	1, 0, 0, 0)
ROT_FN(Norm_D32_S1,	0, 1, 0, 0)
ROT_FN(Titled_D32_S1,	1, 1, 0, 0)
ROT_FN(Norm_D16_S16,	0, 0, 1, 0)
ROT_FN(Titled_D16_S16,	1, 0, 1, 0)
ROT_FN(Norm_D32_S16,	0, 1, 1, 0)
ROT_FN(Titled_D32_S16,	1, 1, 1, 0)

ROT_FN(U_Norm_D16_S1,	0, 0, 0, 1)
ROT_FN(U_Titled_D16_S1,	1, 0, 0, 1)
ROT_FN(U_Norm_D32_S1,	0, 1, 0, 1)
ROT_FN(U_Titled_D32_S1,	1, 1, 0, 1)
ROT_FN(U_Norm_D16_S16,	0, 0, 1, 1)
ROT_FN(U_Titled_D16_S16,1, 0, 1, 1)
ROT_FN(U_Norm_D32_S16,	0, 1, 1, 1)
ROT_FN(U_Titled_D32_S16,1, 1, 1, 1)

ROT_FN(O_Norm_D16_S1,	0, 0, 0, 2)
ROT_FN(O_Titled_D16_S1,	1, 0, 0, 2)
ROT_FN(O_Norm_D32_S1,	0, 1, 0, 2)
ROT_FN(O_Titled_D32_S1,	1, 1, 0, 2)
ROT_FN(O_Norm_D16_S16,	0, 0, 1, 2)
ROT_FN(O_Titled_D16_S16,1, 0, 1, 2)
ROT_FN(O_Norm_D32_S16,	0, 1, 1, 2)
ROT_FN(O_Titled_D32_S16,1, 1, 1, 2)

static const rot_fn Table_Jump_Rot[32] =
{
	Norm_D16_S1,		Titled_D16_S1,
	Norm_D32_S1,		Titled_D32_S1,
	Norm_D16_S16,		Titled_D16_S16,
	Norm_D32_S16,		Titled_D32_S16,

	U_Norm_D16_S1,		U_Titled_D16_S1,
	U_Norm_D32_S1,		U_Titled_D32_S1,
	U_Norm_D16_S16,		U_Titled_D16_S16,
	U_Norm_D32_S16,		U_Titled_D32_S16,

	O_Norm_D16_S1,		O_Titled_D16_S1,
	O_Norm_D32_S1,		O_Titled_D32_S1,
	O_Norm_D16_S16,		O_Titled_D16_S16,
	O_Norm_D32_S16,		O_Titled_D32_S16,

	Norm_D16_S1,		Titled_D16_S1,
	Norm_D32_S1,		Titled_D32_S1,
	Norm_D16_S16,		Titled_D16_S16,
	Norm_D32_S16,		Titled_D32_S16,
};


/** Public functions. **/

/**
 * Init_RS_GFX(): Initialize the rotation/scaling registers.
 */
void Init_RS_GFX(void)
{
	Rot_Comp.Stamp_Size = 0;	// Reg_58
	Rot_Comp.Stamp_Map_Adr = 0;	// Reg_5A
	Rot_Comp.IB_V_Cell_Size = 0;	// Reg_5C
	Rot_Comp.IB_Adr = 0;		// Reg_5E
	Rot_Comp.IB_Offset = 0;		// Reg_60
	Rot_Comp.IB_H_Dot_Size = 0;	// Reg_62
	Rot_Comp.IB_V_Dot_Size = 0;	// Reg_64
	Rot_Comp.Vector_Adr = 0;	// Reg_66
}


/**
 * gfx_completed(): GFX operation completed. Raise INT1 if enabled.
 */
static void gfx_completed(void)
{
	Rot_Comp.Stamp_Size &= 0x7FFF;
	Rot_Comp.IB_V_Dot_Size = 0;	// GFX Completed
	if (Int_Mask_S68K & 0x02)
		sub68k_interrupt(1, -1);
}


/**
 * Calcul_Rot_Comp(): Start a new GFX operation.
 */
void Calcul_Rot_Comp(void)
{
	uint32_t jmp, stamp_size;

	if ((Ram_Word_State & 0xFF) > 1)
		return;

	XD_Mul = (int)(((Rot_Comp.IB_V_Cell_Size & 0x001F) * 4) + 4);

	jmp = ((Rot_Comp.Stamp_Size & 0x07) | S68K_Mem_PM) & 0x1F;
	Buffer_Adr = (Rot_Comp.IB_Adr & 0xFFF8) * 4;
	YD = (int)((Rot_Comp.IB_Offset >> 3) & 0x07);

	Vector_Adr = (Rot_Comp.Vector_Adr & 0xFFFE) * 4;
	Jmp_Adr = jmp;		// we save the jump address (for later use)

	Draw_Speed = Table_Rot_Time[(Rot_Comp.IB_H_Dot_Size & 0x1FF) >> 3];
	Float_Part = Draw_Speed;	// we start a new GFX operation

	stamp_size = Rot_Comp.Stamp_Size | 0x8000;	// we start a new GFX operation
	Rot_Comp.Stamp_Size = stamp_size;

	if (stamp_size & 0x04)
	{
		// 16x16 screen
		if (stamp_size & 0x02)
			Stamp_Map_Adr = (Rot_Comp.Stamp_Map_Adr & 0xE000) * 4;
		else
			Stamp_Map_Adr = 0x20000;
	}
	else
	{
		// 1x1 screen
		if (stamp_size & 0x02)
			Stamp_Map_Adr = (Rot_Comp.Stamp_Map_Adr & 0xFFE0) * 4;
		else
			Stamp_Map_Adr = (Rot_Comp.Stamp_Map_Adr & 0xFF80) * 4;
	}

	Update_Rot();
}


/**
 * Update_Rot(): Continue the current GFX operation.
 */
void Update_Rot(void)
{
	uint32_t fp, lines;

	if ((Rot_Comp.IB_V_Dot_Size & 0xFF) == 0)
	{
		gfx_completed();
		return;
	}

	fp = (uint32_t)Float_Part;
	if (!(fp & 0xFFFF0000))
	{
		Float_Part = (int)(fp + (uint32_t)Draw_Speed);
		return;
	}

	// Have_To_Draw: draw (Float_Part >> 16) lines.
	lines = fp >> 16;
	Float_Part = (int)((fp & 0xFFFF) + (uint32_t)Draw_Speed);

	for (;;)
	{
		if (Table_Jump_Rot[Jmp_Adr & 0x1F]())
		{
			// GFX_Completed
			gfx_completed();
			return;
		}

		if (--lines == 0)
			return;		// GFX_Part_Completed
	}
}
