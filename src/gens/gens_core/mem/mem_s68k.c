/***************************************************************************
 * Gens: Sub 68000 memory management.                                      *
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
 * C port of mem_s68k.asm. (SegaCD sub 68000 memory map)
 *
 * Memory layout notes (same as the asm):
 * - Ram_Prg, Ram_Word_2M and Ram_Word_1M store 68000 memory as host-endian
 *   16-bit words: a 68000 byte at address A is at u8[A ^ 1], and a 68000
 *   word at (even) A is u16[A >> 1].
 * - Ram_Word_1M holds both 1M banks: bank 0 at 0x00000, bank 1 at 0x20000.
 * - The "dot image" area (0x080000-0x0BFFFF in 1M mode) maps each 68000
 *   byte to one nybble of Word RAM.
 * - Ram_Backup is NOT byte-swapped. Backup RAM byte n is at Ram_Backup[n].
 * - Several asm variables are accessed with partial (byte/word) loads and
 *   stores at fixed offsets. Those accesses are done here with shifts and
 *   masks (little-endian byte numbering, as in the asm).
 */

#include <stdint.h>

#include "mem_s68k.h"

#include "gens_core/cpu/68k/star_68k.h"
#include "gens_core/cpu/68k/cpu_68k.h"
#include "gens_core/gfx/gfx_cd.h"
#include "gens_core/sound/pcm.h"
#include "segacd/lc89510.h"
#include "segacd/cd_sys.hpp"


/** Data. **/

// The headers declare these with anonymous struct/union types.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++-compat"

__typeof__(Ram_Prg)     Ram_Prg     __attribute__((aligned(64)));
__typeof__(Ram_Word_2M) Ram_Word_2M __attribute__((aligned(64)));
__typeof__(Ram_Word_1M) Ram_Word_1M __attribute__((aligned(64)));
unsigned short Cell_Conv_Tab[64 * 1024] __attribute__((aligned(64)));
unsigned char Ram_Backup[8 * 1024] __attribute__((aligned(64)));

__typeof__(COMM) COMM __attribute__((aligned(16)));
__typeof__(CDD)  CDD  __attribute__((aligned(16)));
__typeof__(CDC)  CDC  __attribute__((aligned(16)));

#pragma GCC diagnostic pop

unsigned int LED_Status __attribute__((aligned(16)));
unsigned int S68K_Mem_WP;
unsigned int S68K_Mem_PM;
unsigned int Ram_Word_State;
unsigned int Init_Timer_INT3;
unsigned int Timer_INT3;
unsigned int Timer_Step;
unsigned int Int_Mask_S68K;
unsigned int Font_COLOR;
unsigned int Font_BITS;
unsigned int CD_Access_Timer;

/**
 * Memory_Control_Status: four status bytes, indexed by (Ram_Word_State & 3).
 * asm: db 1, 2, 4 + 0, 5 + 0 (byte 0 is the least significant byte.)
 */
unsigned int Memory_Control_Status = 0x05040201U;


/** Helpers for the asm's partial loads/stores. **/

static inline uint8_t get_byte(uint32_t var, unsigned int n)
{
	return (uint8_t)(var >> (n * 8));
}

static inline uint32_t set_byte(uint32_t var, unsigned int n, uint8_t val)
{
	return (var & ~(0xFFU << (n * 8))) | ((uint32_t)val << (n * 8));
}

static inline uint32_t set_low_word(uint32_t var, uint16_t val)
{
	return (var & 0xFFFF0000U) | val;
}

/** Byte n of an array of 16-bit words, as stored in (little-endian) memory. **/
static inline uint8_t u16arr_get_byte(const unsigned short *arr, unsigned int n)
{
	return (uint8_t)(arr[n >> 1] >> ((n & 1) * 8));
}

static inline void u16arr_set_byte(unsigned short *arr, unsigned int n, uint8_t val)
{
	const unsigned int sh = (n & 1) * 8;
	arr[n >> 1] = (unsigned short)((arr[n >> 1] & ~(0xFFU << sh)) | ((unsigned int)val << sh));
}

/** Memory_Control_Status byte for the current Word RAM state. **/
static inline uint8_t mcs_byte(void)
{
	return get_byte(Memory_Control_Status, Ram_Word_State & 3);
}

/** Word RAM 1M bank offset. (Ram_Word_State: 2 == 1M_1, 3 == 1M_0) **/
static inline unsigned int word_ram_1m_bank(void)
{
	return ((Ram_Word_State & 3) == 2 ? 0x20000 : 0);
}

/**
 * Backup RAM word access.
 * The asm does a 16-bit access at byte offset ((Adr & 0x3FFF) >> 1), so the
 * high byte of the last word is past the end of Ram_Backup. In the asm
 * layout, that byte is the low byte of COMM.Flag (COMM directly follows
 * Ram_Backup), so that's emulated here.
 */
static inline uint8_t backup_get_byte(unsigned int n)
{
	if (n < sizeof(Ram_Backup))
		return Ram_Backup[n];
	return get_byte(COMM.Flag, 0);
}

static inline void backup_set_byte(unsigned int n, uint8_t val)
{
	if (n < sizeof(Ram_Backup))
		Ram_Backup[n] = val;
	else
		COMM.Flag = set_byte(COMM.Flag, 0, val);
}

/** PCM channel address register byte. (reg = Adr & 0x1E) **/
static inline uint8_t pcm_read_addr(unsigned int reg)
{
	const unsigned int idx = reg >> 1;
	const unsigned int chan = idx >> 1;
	const unsigned int shift = 11 + ((idx & 1) * 8);	// PCM_STEP_SHIFT
	return (uint8_t)(PCM_Chip.Channel[chan].Addr >> shift);
}

/** PCM RAM byte offset. **/
static inline unsigned int pcm_ram_offset(unsigned int Adr)
{
	return ((Adr >> 1) & 0xFFF) + (unsigned int)PCM_Chip.Bank;
}

/** Font_COLOR entry: byte 0 (background) or byte 2 (foreground). **/
static inline uint8_t font_color_b(unsigned int bit)
{
	return get_byte(Font_COLOR, bit ? 2 : 0);
}

static inline uint16_t font_color_w(unsigned int bit)
{
	return (uint16_t)(Font_COLOR >> (bit ? 16 : 0));
}

static inline unsigned int font_bit(unsigned int n)
{
	return (Font_BITS >> n) & 1;
}


/** Word RAM swap. **/

static void Swap_2M_To_1M(void)
{
	unsigned int i;
	for (i = 0; i < (256 * 1024) / 4; i++)
	{
		const uint32_t d = Ram_Word_2M.u32[i];
		Ram_Word_1M.u16[i] = (uint16_t)d;
		Ram_Word_1M.u16[i + 0x10000] = (uint16_t)(d >> 16);
	}
}

static void Swap_1M_To_2M(void)
{
	unsigned int i;
	for (i = 0; i < (256 * 1024) / 4; i++)
	{
		Ram_Word_2M.u32[i] = ((uint32_t)Ram_Word_1M.u16[i + 0x10000] << 16) |
				     Ram_Word_1M.u16[i];
	}
}


/** Register writes shared by the byte and word handlers. **/

/**
 * Write to the memory mode register (0xFF8003). Only the low byte is used.
 */
static void s68k_write_memory_mode(uint8_t al)
{
	const uint8_t bl = al & 0x18;

	if (!(al & 4))
	{
		// Mode 2M.
		Memory_Control_Status &= 0xFDFDFFFFU;	// DMNA bit = 0
		S68K_Mem_PM = set_byte(S68K_Mem_PM, 0, bl);

		if (al & 1)
		{
			// RET
			const int was_1M = (Ram_Word_State & 2);
			Ram_Word_State = set_byte(Ram_Word_State, 0, 0);
			if (was_1M)
				Swap_1M_To_2M();
		}
		else
		{
			uint8_t st = get_byte(Ram_Word_State, 0);
			if (st & 2)
			{
				// Switching to 2M mode.
				st &= 1;
				Memory_Control_Status &= 0xFFFFFDFEU;	// RET & DMNA bit = 0
				Ram_Word_State = set_byte(Ram_Word_State, 0, st);
				Swap_1M_To_2M();
			}
		}

		MS68K_Set_Word_Ram();
	}
	else
	{
		// Mode 1M.
		const uint8_t a = al & 1;
		const uint8_t bh = get_byte(Ram_Word_State, 0);
		const uint8_t ah = a ^ bh;

		S68K_Mem_PM = set_byte(S68K_Mem_PM, 0, bl);
		Ram_Word_State = set_byte(Ram_Word_State, 0, (uint8_t)(a + 2));

		if (!(bh & 2))
		{
			Memory_Control_Status &= 0xFDFDFFFFU;	// DMNA bit = 0
			Swap_2M_To_1M();
		}
		else if (ah & 1)
		{
			// Bank change.
			Memory_Control_Status &= 0xFDFDFFFFU;	// DMNA bit = 0
		}

		MS68K_Set_Word_Ram();
	}
}

/** Write to the interrupt mask register (0xFF8033). **/
static void s68k_write_int_mask(uint8_t al)
{
	if ((al & 0x10) && (CDD.Control & 0x4) && !(Int_Mask_S68K & 0x10))
	{
		Int_Mask_S68K = set_byte(Int_Mask_S68K, 0, al);
		CDD_Processing();
		return;
	}

	Int_Mask_S68K = set_byte(Int_Mask_S68K, 0, al);
}

/** Write to the CDD control register (0xFF8037). **/
static void s68k_write_cdd_ctrl(uint8_t al)
{
	al &= 0x4;
	if (al && !(CDD.Control & 0x4) && (Int_Mask_S68K & 0x10))
		CDD_Processing();

	CDD.Control |= al;
}

/** Write to the font color register (0xFF804D). **/
static void s68k_write_font_color(uint8_t al)
{
	Font_COLOR = set_byte(Font_COLOR, 0, al & 0x0F);
	Font_COLOR = set_byte(Font_COLOR, 2, al >> 4);
}

/** Read the font color register. (8-bit) **/
static uint8_t s68k_read_font_color(void)
{
	const uint8_t ah = (uint8_t)(get_byte(Font_COLOR, 2) << 4);
	return get_byte(Font_COLOR, 0) | ah;
}


/** Read Byte **/

static uint8_t s68k_rb_word_ram(unsigned int Adr)
{
	unsigned int bank;
	uint8_t b;

	if ((Ram_Word_State & 3) < 2)
	{
		// 2M
		if (Adr > 0x0BFFFF)
			return 0;
		return Ram_Word_2M.u8[(Adr ^ 1) - 0x080000];
	}

	// 1M
	bank = word_ram_1m_bank();
	if (Adr < 0x0C0000)
	{
		// Dot image.
		b = Ram_Word_1M.u8[((Adr >> 1) ^ 1) - 0x040000 + bank];
		return (Adr & 1) ? (b & 0x0F) : (b >> 4);
	}
	if (Adr > 0x0DFFFF)
		return 0;
	return Ram_Word_1M.u8[(Adr ^ 1) - 0x0C0000 + bank];
}

static uint8_t s68k_rb_reg(unsigned int Adr)
{
	const unsigned int r = Adr & 0x7F;

	switch (r)
	{
		case 0x00:	return get_byte(LED_Status, 0);
		case 0x01:	return 1;
		case 0x02:	return get_byte(S68K_Mem_WP, 0);
		case 0x03:	return get_byte(S68K_Mem_PM, 0) | mcs_byte();
		case 0x04:	return get_byte(CDC.RS0, 1);
		case 0x05:	return get_byte(CDC.RS0, 0);
		case 0x06:	return 0;
		case 0x07:	return CDC_Read_Reg();
		case 0x08:	return 0;
		case 0x09:	return 0;
		case 0x0A:	return get_byte(CDC.DMA_Adr, 1);
		case 0x0B:	return get_byte(CDC.DMA_Adr, 0);
		case 0x0C:	return get_byte(CDC.Stop_Watch, 3);
		case 0x0D:	return get_byte(CDC.Stop_Watch, 2);
		case 0x0E:	return get_byte(COMM.Flag, 1);
		case 0x0F:	return get_byte(COMM.Flag, 0);

		case 0x10: case 0x11: case 0x12: case 0x13:
		case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1A: case 0x1B:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			return u16arr_get_byte(COMM.Command, (r ^ 1) - 0x10);

		case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x28: case 0x29: case 0x2A: case 0x2B:
		case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			return u16arr_get_byte(COMM.Status, (r ^ 1) - 0x20);

		case 0x30:	return 0;
		case 0x31:	return get_byte(Timer_INT3, 2);
		case 0x32:	return 0;
		case 0x33:	return get_byte(Int_Mask_S68K, 0);
		case 0x34:	return get_byte(CDD.Fader, 1) & 0x80;
		case 0x35:	return 0;
		case 0x36:	return get_byte(CDD.Control, 1);
		case 0x37:	return get_byte(CDD.Control, 0);

		case 0x38: case 0x39: case 0x3A: case 0x3B:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
		case 0x40: case 0x41:
			// CDD status 0-4.
			return CDD.Rcv_Status[(r ^ 1) - 0x38];

		case 0x42: case 0x43: case 0x44: case 0x45:
		case 0x46: case 0x47: case 0x48: case 0x49:
		case 0x4A: case 0x4B:
			// CDD status 5-9: write-only.
			return 0;

		case 0x4C:	return 0;
		case 0x4D:	return s68k_read_font_color();
		case 0x4E:	return get_byte(Font_BITS, 1);
		case 0x4F:	return get_byte(Font_BITS, 0);

		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
		{
			// Font data: two pixels (nybbles) per byte.
			// 0x50 uses Font_BITS bits 15-14; 0x57 uses bits 1-0.
			const unsigned int sh = 14 - ((r - 0x50) * 2);
			const uint8_t hi = (uint8_t)(font_color_b(font_bit(sh + 1)) << 4);
			return hi | font_color_b(font_bit(sh));
		}

		case 0x58:	return get_byte(Rot_Comp.Stamp_Size, 1);
		case 0x59:	return get_byte(Rot_Comp.Stamp_Size, 0);
		case 0x5A:	return get_byte(Rot_Comp.Stamp_Map_Adr, 1);
		case 0x5B:	return get_byte(Rot_Comp.Stamp_Map_Adr, 0);
		case 0x5C:	return 0;
		case 0x5D:	return get_byte(Rot_Comp.IB_V_Cell_Size, 0);
		case 0x5E:	return get_byte(Rot_Comp.IB_Adr, 1);
		case 0x5F:	return get_byte(Rot_Comp.IB_Adr, 0);
		case 0x60:	return 0;
		case 0x61:	return get_byte(Rot_Comp.IB_Offset, 0);
		case 0x62:	return get_byte(Rot_Comp.IB_H_Dot_Size, 1);
		case 0x63:	return get_byte(Rot_Comp.IB_H_Dot_Size, 0);
		case 0x64:	return get_byte(Rot_Comp.IB_V_Dot_Size, 1);
		case 0x65:	return get_byte(Rot_Comp.IB_V_Dot_Size, 0);
		case 0x66:	return 0;	// Vector address: write-only.
		case 0x67:	return 0;
		case 0x68:	return 0;	// Subcode address.

		case 0x69:
			// The asm passes the masked register offset (0x69) here,
			// and returns 0.
			SCD_Read_Byte(r);
			return 0;

		default:
			return 0;
	}
}

static uint8_t s68k_rb_io(unsigned int Adr)
{
	if (Adr > 0xFF7FFF)
	{
		// Sub 68000 registers.
		if (Adr > 0xFF807F)
		{
			// Subcode buffer.
			if (Adr > 0xFF81FF)
				return 0;
			return SCD_Read_Byte(Adr);
		}
		return s68k_rb_reg(Adr);
	}

	if (Adr > 0xFEFFFF)
	{
		// PCM.
		if (Adr > 0xFF1FFF)
			return Ram_PCM[pcm_ram_offset(Adr)];
		if (Adr < 0xFF0020)
			return 0;
		return pcm_read_addr(Adr & 0x1E);
	}

	// Backup RAM.
	return Ram_Backup[(Adr & 0x3FFF) >> 1];
}

/**
 * S68K_RB(): Read a byte from the Sub 68000 address space.
 * @param Adr Address. (24-bit)
 * @return Byte.
 */
unsigned char S68K_RB(unsigned int Adr)
{
	if (Adr > 0xFDFFFF)
		return s68k_rb_io(Adr);

	if (Adr <= 0x07FFFF)
		return Ram_Prg.u8[Adr ^ 1];

	return s68k_rb_word_ram(Adr);
}


/** Read Word **/

static uint16_t s68k_rw_word_ram(unsigned int Adr)
{
	unsigned int bank;
	uint8_t b;

	if ((Ram_Word_State & 3) < 2)
	{
		// 2M
		if (Adr > 0x0BFFFF)
			return 0;
		return Ram_Word_2M.u16[(Adr - 0x080000) >> 1];
	}

	// 1M
	bank = word_ram_1m_bank();
	if (Adr < 0x0C0000)
	{
		// Dot image: one byte (two nybbles) of Word RAM per 68000 word.
		b = Ram_Word_1M.u8[((Adr >> 1) ^ 1) - 0x040000 + bank];
		return (uint16_t)(((b >> 4) << 8) | (b & 0x0F));
	}
	if (Adr > 0x0DFFFF)
		return 0;
	return Ram_Word_1M.u16[(Adr - 0x0C0000 + bank) >> 1];
}

static uint16_t s68k_rw_reg(unsigned int Adr)
{
	const unsigned int r = Adr & 0x7E;

	switch (r)
	{
		case 0x00:
			return (uint16_t)((get_byte(LED_Status, 0) << 8) | 1);
		case 0x02:
			return (uint16_t)((get_byte(S68K_Mem_WP, 0) << 8) |
					  (get_byte(S68K_Mem_PM, 0) | mcs_byte()));
		case 0x04:	return (uint16_t)CDC.RS0;
		case 0x06:	return CDC_Read_Reg();
		case 0x08:	return Read_CDC_Host_SUB();
		case 0x0A:	return (uint16_t)CDC.DMA_Adr;
		case 0x0C:	return (uint16_t)(CDC.Stop_Watch >> 16);
		case 0x0E:	return (uint16_t)COMM.Flag;

		case 0x10: case 0x12: case 0x14: case 0x16:
		case 0x18: case 0x1A: case 0x1C: case 0x1E:
			return COMM.Command[(r - 0x10) >> 1];

		case 0x20: case 0x22: case 0x24: case 0x26:
		case 0x28: case 0x2A: case 0x2C: case 0x2E:
			return COMM.Status[(r - 0x20) >> 1];

		case 0x30:	return (uint16_t)(Timer_INT3 >> 16);
		case 0x32:	return (uint16_t)Int_Mask_S68K;
		case 0x34:	return (uint16_t)(CDD.Fader & 0x8000);
		case 0x36:	return (uint16_t)CDD.Control;

		case 0x38: case 0x3A: case 0x3C: case 0x3E: case 0x40:
		{
			// CDD status 0-4.
			const unsigned int o = r - 0x38;
			return (uint16_t)(CDD.Rcv_Status[o] | (CDD.Rcv_Status[o + 1] << 8));
		}

		case 0x42: case 0x44: case 0x46: case 0x48: case 0x4A:
			// CDD status 5-9: write-only.
			return 0;

		case 0x4C:	return s68k_read_font_color();
		case 0x4E:	return (uint16_t)Font_BITS;

		case 0x50: case 0x52: case 0x54: case 0x56:
		{
			// Font data: four pixels (nybbles) per word.
			// 0x50 uses Font_BITS bits 15-12; 0x56 uses bits 3-0.
			const unsigned int sh = 12 - ((r - 0x50) * 2);
			uint16_t ax = font_color_w(font_bit(sh + 3));
			ax = (uint16_t)((ax << 4) | font_color_w(font_bit(sh + 2)));
			ax = (uint16_t)((ax << 4) | font_color_w(font_bit(sh + 1)));
			ax = (uint16_t)((ax << 4) | font_color_w(font_bit(sh)));
			return ax;
		}

		case 0x58:	return (uint16_t)Rot_Comp.Stamp_Size;
		case 0x5A:	return (uint16_t)Rot_Comp.Stamp_Map_Adr;
		case 0x5C:	return (uint16_t)Rot_Comp.IB_V_Cell_Size;
		case 0x5E:	return (uint16_t)Rot_Comp.IB_Adr;
		case 0x60:	return (uint16_t)Rot_Comp.IB_Offset;
		case 0x62:	return (uint16_t)Rot_Comp.IB_H_Dot_Size;
		case 0x64:	return (uint16_t)Rot_Comp.IB_V_Dot_Size;
		case 0x66:	return 0;	// Vector address: write-only.
		case 0x68:	return 0;	// Subcode address.

		default:
			return 0;
	}
}

static uint16_t s68k_rw_io(unsigned int Adr)
{
	if (Adr > 0xFF7FFF)
	{
		// Sub 68000 registers.
		if (Adr > 0xFF807F)
		{
			// Subcode buffer.
			if (Adr > 0xFF81FF)
				return 0;
			return SCD_Read_Word(Adr);
		}
		return s68k_rw_reg(Adr);
	}

	if (Adr > 0xFEFFFF)
	{
		// PCM.
		if (Adr > 0xFF1FFF)
			return Ram_PCM[pcm_ram_offset(Adr)];
		if (Adr < 0xFF0020)
			return 0;
		return pcm_read_addr(Adr & 0x1E);
	}

	// Backup RAM.
	// TODO (from the asm): This doesn't seem to be right.
	// It should read a byte, not a word.
	{
		const unsigned int n = (Adr & 0x3FFF) >> 1;
		return (uint16_t)(backup_get_byte(n) | (backup_get_byte(n + 1) << 8));
	}
}

/**
 * S68K_RW(): Read a word from the Sub 68000 address space.
 * @param Adr Address. (24-bit)
 * @return Word.
 */
unsigned short S68K_RW(unsigned int Adr)
{
	if (Adr > 0xFDFFFF)
		return s68k_rw_io(Adr);

	if (Adr <= 0x07FFFF)
		return Ram_Prg.u16[Adr >> 1];

	return s68k_rw_word_ram(Adr);
}


/** Write Byte **/

/**
 * Write one dot image pixel (byte write: one nybble).
 * @param p Word RAM byte.
 * @param odd If nonzero, write the low nybble; otherwise, the high nybble.
 * @param data Data. (low nybble is used)
 */
static void s68k_wb_dot(uint8_t *p, int odd, uint8_t data)
{
	const uint8_t pm = get_byte(S68K_Mem_PM, 0);

	if (odd)
	{
		const uint8_t al = data & 0x0F;
		if (pm == 0x08)
		{
			// Underwrite.
			if (*p & 0x0F)
				return;
		}
		else if (pm > 0x08)
		{
			// Overwrite.
			if (!al)
				return;
		}
		*p = al | (*p & 0xF0);
	}
	else
	{
		const uint8_t al = (uint8_t)(data << 4);
		if (pm == 0x08)
		{
			// Underwrite.
			if (*p & 0xF0)
				return;
		}
		else if (pm > 0x08)
		{
			// Overwrite.
			if (!al)
				return;
		}
		*p = al | (*p & 0x0F);
	}
}

static void s68k_wb_word_ram(unsigned int Adr, uint8_t data)
{
	unsigned int bank;

	if ((Ram_Word_State & 3) < 2)
	{
		// 2M
		if (Adr > 0x0BFFFF)
			return;
		Ram_Word_2M.u8[(Adr ^ 1) - 0x080000] = data;
		return;
	}

	// 1M
	bank = word_ram_1m_bank();
	if (Adr < 0x0C0000)
	{
		// Dot image.
		s68k_wb_dot(&Ram_Word_1M.u8[((Adr >> 1) ^ 1) - 0x040000 + bank],
			    (Adr & 1), data);
		return;
	}
	if (Adr > 0x0DFFFF)
		return;
	Ram_Word_1M.u8[(Adr ^ 1) - 0x0C0000 + bank] = data;
}

static void s68k_wb_reg(unsigned int Adr, uint8_t data)
{
	const unsigned int r = Adr & 0x7F;

	switch (r)
	{
		case 0x00:
			LED_Status = set_byte(LED_Status, 0, data);
			break;

		case 0x03:
			s68k_write_memory_mode(data);
			break;

		case 0x04:
		{
			// CDC mode (high byte).
			const uint8_t cl = get_byte(CDC.RS0, 1) & 0xC0;
			CDC.DMA_Adr = 0;
			CDC.RS0 = set_byte(CDC.RS0, 1, (uint8_t)((data & 0x07) | cl));
			break;
		}

		case 0x05:
			CDC.RS0 = set_byte(CDC.RS0, 0, data & 0x0F);
			break;

		case 0x07:
			CDC_Write_Reg(data);
			break;

		case 0x0A:
			CDC.DMA_Adr = set_byte(CDC.DMA_Adr, 1, data);
			break;
		case 0x0B:
			CDC.DMA_Adr = set_byte(CDC.DMA_Adr, 0, data);
			break;

		case 0x0C:
		case 0x0D:
			CDC.Stop_Watch = 0;
			break;

		case 0x0E:
			// Dragons lair: "ror al, 1", stored in the low byte.
			COMM.Flag = set_byte(COMM.Flag, 0,
					(uint8_t)((data >> 1) | (data << 7)));
			break;
		case 0x0F:
			COMM.Flag = set_byte(COMM.Flag, 0, data);
			break;

		case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x28: case 0x29: case 0x2A: case 0x2B:
		case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			u16arr_set_byte(COMM.Status, (r ^ 1) - 0x20, data);
			break;

		case 0x31:
			// Timer.
			Init_Timer_INT3 = (uint32_t)data << 16;
			Timer_INT3 = Init_Timer_INT3;
			break;

		case 0x33:
			s68k_write_int_mask(data);
			break;

		case 0x37:
			s68k_write_cdd_ctrl(data);
			break;

		case 0x42: case 0x43: case 0x44: case 0x45:
		case 0x46: case 0x47: case 0x48: case 0x49:
		case 0x4A:
			// CDD command 0-8, and the high byte of command 9.
			CDD.Trans_Comm[(r ^ 1) - 0x42] = data;
			break;

		case 0x4B:
			CDD.Trans_Comm[8] = data;
			CDD_Import_Command();
			break;

		case 0x4D:
			s68k_write_font_color(data);
			break;
		case 0x4E:
			Font_BITS = set_byte(Font_BITS, 1, data);
			break;
		case 0x4F:
			Font_BITS = set_byte(Font_BITS, 0, data);
			break;

		case 0x59:
			Rot_Comp.Stamp_Size = set_byte(Rot_Comp.Stamp_Size, 0, data & 0x07);
			break;

		case 0x5A:
		case 0x5B:
			Rot_Comp.Stamp_Map_Adr = set_low_word(Rot_Comp.Stamp_Map_Adr,
						(uint16_t)(((data << 8) | data) & 0xFFE0));
			break;

		case 0x5D:
			Rot_Comp.IB_V_Cell_Size = set_byte(Rot_Comp.IB_V_Cell_Size, 0, data & 0x1F);
			break;

		case 0x5E:
		case 0x5F:
			Rot_Comp.IB_Adr = set_low_word(Rot_Comp.IB_Adr,
						(uint16_t)(((data << 8) | data) & 0xFFF8));
			break;

		case 0x61:
			Rot_Comp.IB_Offset = set_byte(Rot_Comp.IB_Offset, 0, data & 0x3F);
			break;

		case 0x62:
		case 0x63:
			Rot_Comp.IB_H_Dot_Size = set_low_word(Rot_Comp.IB_H_Dot_Size,
						(uint16_t)(((data << 8) | data) & 0x01FF));
			break;

		case 0x64:
		case 0x65:
			// Full 32-bit store. (The asm needs eax for timing.)
			Rot_Comp.IB_V_Dot_Size = data;
			break;

		case 0x66:
		case 0x67:
			Rot_Comp.Vector_Adr = set_low_word(Rot_Comp.Vector_Adr,
						(uint16_t)(((data << 8) | data) & 0xFFFE));
			Calcul_Rot_Comp();
			break;

		default:
			// Read-only or unused registers.
			break;
	}
}

static void s68k_wb_io(unsigned int Adr, uint8_t data)
{
	if (Adr > 0xFF7FFF)
	{
		// Sub 68000 registers.
		// (Writes to the subcode buffer are ignored.)
		if (Adr <= 0xFF807F)
			s68k_wb_reg(Adr, data);
		return;
	}

	if (Adr > 0xFEFFFF)
	{
		// PCM.
		if (Adr > 0xFF1FFF)
		{
			Ram_PCM[pcm_ram_offset(Adr)] = data;
			return;
		}
		if (Adr > 0xFF0011)
			return;
		PCM_Write_Reg((Adr >> 1) & 0x1F, data);
		return;
	}

	// Backup RAM.
	Ram_Backup[(Adr & 0x3FFF) >> 1] = data;
}

/**
 * S68K_WB(): Write a byte to the Sub 68000 address space.
 * @param Adr Address. (24-bit)
 * @param Data Byte.
 */
void S68K_WB(unsigned int Adr, unsigned char Data)
{
	if (Adr > 0xFDFFFF)
	{
		s68k_wb_io(Adr, Data);
		return;
	}

	if (Adr <= 0x07FFFF)
	{
		// Program RAM, with write protection.
		if ((uint32_t)(S68K_Mem_WP << 8) > (Adr ^ 1))
			return;
		Ram_Prg.u8[Adr ^ 1] = Data;
		return;
	}

	s68k_wb_word_ram(Adr, Data);
}


/** Write Word **/

static void s68k_ww_word_ram(unsigned int Adr, uint16_t data)
{
	unsigned int bank;

	if ((Ram_Word_State & 3) < 2)
	{
		// 2M
		if (Adr > 0x0BFFFF)
			return;
		Ram_Word_2M.u16[(Adr - 0x080000) >> 1] = data;
		return;
	}

	// 1M
	bank = word_ram_1m_bank();
	if (Adr < 0x0C0000)
	{
		// Dot image: two pixels (nybbles) per 68000 word.
		uint8_t *p = &Ram_Word_1M.u8[((Adr >> 1) ^ 1) - 0x040000 + bank];
		const uint8_t pm = get_byte(S68K_Mem_PM, 0);
		const uint8_t lo = (uint8_t)(data & 0x0F);
		const uint8_t hi = (uint8_t)((data >> 8) << 4);

		if (pm == 0x08)
		{
			// Underwrite: only if both pixels are 0.
			if (*p == 0)
				*p = lo | hi;
		}
		else if (pm > 0x08)
		{
			// Overwrite: only if both pixels are nonzero.
			if (lo && hi)
				*p = lo | hi;
		}
		else
		{
			*p = lo | hi;
		}
		return;
	}
	if (Adr > 0x0DFFFF)
		return;
	Ram_Word_1M.u16[(Adr - 0x0C0000 + bank) >> 1] = data;
}

static void s68k_ww_reg(unsigned int Adr, uint16_t data)
{
	const unsigned int r = Adr & 0x7E;
	const uint8_t al = (uint8_t)data;

	switch (r)
	{
		case 0x00:
			LED_Status = set_byte(LED_Status, 0, (uint8_t)((data >> 8) & 3));
			break;

		case 0x02:
			s68k_write_memory_mode(al);
			break;

		case 0x04:
			CDC.DMA_Adr = 0;
			CDC.RS0 = set_low_word(CDC.RS0,
					(uint16_t)((data & 0x070F) | (CDC.RS0 & 0xC000)));
			break;

		case 0x06:
			CDC_Write_Reg(al);
			break;

		case 0x0A:
			CDC.DMA_Adr = set_low_word(CDC.DMA_Adr, data);
			break;

		case 0x0C:
			CDC.Stop_Watch = 0;
			break;

		case 0x0E:
			COMM.Flag = set_byte(COMM.Flag, 0, al);
			break;

		case 0x20: case 0x22: case 0x24: case 0x26:
		case 0x28: case 0x2A: case 0x2C: case 0x2E:
			COMM.Status[(r - 0x20) >> 1] = data;
			break;

		case 0x30:
			// Timer.
			Init_Timer_INT3 = (uint32_t)al << 16;
			Timer_INT3 = Init_Timer_INT3;
			break;

		case 0x32:
			s68k_write_int_mask(al);
			break;

		case 0x34:
			CDD.Fader = set_low_word(CDD.Fader, data & 0x7FFE);
			break;

		case 0x36:
			s68k_write_cdd_ctrl(al);
			break;

		case 0x42: case 0x44: case 0x46: case 0x48:
		{
			// CDD command 0-7.
			const unsigned int o = r - 0x42;
			CDD.Trans_Comm[o] = al;
			CDD.Trans_Comm[o + 1] = (uint8_t)(data >> 8);
			break;
		}

		case 0x4A:
			// CDD command 8-9.
			CDD.Trans_Comm[8] = al;
			CDD.Trans_Comm[9] = (uint8_t)(data >> 8);
			CDD_Import_Command();
			break;

		case 0x4C:
			s68k_write_font_color(al);
			break;

		case 0x4E:
			Font_BITS = set_low_word(Font_BITS, data);
			break;

		case 0x58:
			Rot_Comp.Stamp_Size = set_low_word(Rot_Comp.Stamp_Size, data & 0x0007);
			break;
		case 0x5A:
			Rot_Comp.Stamp_Map_Adr = set_low_word(Rot_Comp.Stamp_Map_Adr, data & 0xFFE0);
			break;
		case 0x5C:
			Rot_Comp.IB_V_Cell_Size = set_low_word(Rot_Comp.IB_V_Cell_Size, data & 0x001F);
			break;
		case 0x5E:
			Rot_Comp.IB_Adr = set_low_word(Rot_Comp.IB_Adr, data & 0xFFF8);
			break;
		case 0x60:
			Rot_Comp.IB_Offset = set_low_word(Rot_Comp.IB_Offset, data & 0x003F);
			break;
		case 0x62:
			Rot_Comp.IB_H_Dot_Size = set_low_word(Rot_Comp.IB_H_Dot_Size, data & 0x01FF);
			break;
		case 0x64:
			// Full 32-bit store. (The asm needs eax for timing.)
			Rot_Comp.IB_V_Dot_Size = data & 0x00FF;
			break;
		case 0x66:
			Rot_Comp.Vector_Adr = set_low_word(Rot_Comp.Vector_Adr, data & 0xFFFE);
			Calcul_Rot_Comp();
			break;

		default:
			// Read-only or unused registers.
			break;
	}
}

static void s68k_ww_io(unsigned int Adr, uint16_t data)
{
	if (Adr > 0xFF7FFF)
	{
		// Sub 68000 registers.
		// (Writes to the subcode buffer are ignored.)
		if (Adr <= 0xFF807F)
			s68k_ww_reg(Adr, data);
		return;
	}

	if (Adr > 0xFEFFFF)
	{
		// PCM.
		if (Adr > 0xFF1FFF)
		{
			Ram_PCM[pcm_ram_offset(Adr)] = (uint8_t)data;
			return;
		}
		if (Adr > 0xFF0011)
			return;
		PCM_Write_Reg((Adr >> 1) & 0x1F, data);
		return;
	}

	// Backup RAM.
	// TODO (from the asm): This doesn't seem to be right.
	// It should write a byte, not a word.
	{
		const unsigned int n = (Adr & 0x3FFF) >> 1;
		backup_set_byte(n, (uint8_t)data);
		backup_set_byte(n + 1, (uint8_t)(data >> 8));
	}
}

/**
 * S68K_WW(): Write a word to the Sub 68000 address space.
 * @param Adr Address. (24-bit)
 * @param Data Word.
 */
void S68K_WW(unsigned int Adr, unsigned short Data)
{
	if (Adr > 0xFDFFFF)
	{
		s68k_ww_io(Adr, Data);
		return;
	}

	if (Adr <= 0x07FFFF)
	{
		// Program RAM, with write protection.
		if ((uint32_t)(S68K_Mem_WP << 8) > Adr)
			return;
		Ram_Prg.u16[Adr >> 1] = Data;
		return;
	}

	s68k_ww_word_ram(Adr, Data);
}


/** Special SegaCD routines. **/

/**
 * Update_SegaCD_Timer(): Update the stopwatch, the level 3 timer interrupt,
 * the CD drive timer, and the graphics (rotation) operation.
 */
void Update_SegaCD_Timer(void)
{
	uint32_t timer = Timer_INT3;
	uint16_t cnt = (uint16_t)CD_Timer_Counter;

	CDC.Stop_Watch = (CDC.Stop_Watch + Timer_Step) & 0x0FFFFFFF;
	cnt += 10;

	if (Init_Timer_INT3 != 0)
	{
		const int borrow = (timer < Timer_Step);
		timer -= Timer_Step;
		if (borrow && (Int_Mask_S68K & 0x08))
		{
			timer += Init_Timer_INT3;
			sub68k_interrupt(3, -1);
		}
	}

	timer &= 0xFFFFFF;
	Timer_INT3 = timer;

	// 209.6 * 10 = 75th of a second
	if (cnt >= (uint16_t)CD_Access_Timer)
	{
		cnt -= (uint16_t)CD_Access_Timer;
		Check_CD_Command();
	}

	CD_Timer_Counter = (int)set_low_word((uint32_t)CD_Timer_Counter, cnt);

	if (Rot_Comp.Stamp_Size & 0x8000)
		Update_Rot();

	// RET & DMNA bit mode 2M = 1
	Memory_Control_Status |= 0x00000201;
}
