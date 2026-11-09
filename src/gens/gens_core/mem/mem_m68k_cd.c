/***************************************************************************
 * Gens: Main 68000 memory management. (SegaCD)                            *
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
 * C port of mem_m68k_cd.asm.
 *
 * Memory layout notes: (same conventions as mem_m68k.c)
 * - Rom_Data, Ram_Prg, Ram_Word_2M and Ram_Word_1M store 68000 memory as
 *   host-endian 16-bit words. A 68000 byte at address A is at u8[A ^ 1];
 *   a 68000 word at (even) A is u16[A >> 1].
 * - Ram_Backup_Ex (RAM cart) is a plain byte array, one byte per 68000 word.
 * - All handlers take a 24-bit address. (address & 0xFFFFFF)
 */

#include <stdint.h>

#include "mem_m68k.h"
#include "mem_m68k_handlers.h"
#include "mem_m68k_cd.h"
#include "mem_s68k.h"

#include "libgsft/gsft_fastcall.h"

// CPU cores.
#include "gens_core/cpu/68k/star_68k.h"
#include "gens_core/cpu/68k/cpu_68k.h"
#include "mdZ80/mdZ80.h"

// I/O, sound, SegaCD.
#include "gens_core/io/io.h"
#include "gens_core/sound/ym2612.hpp"
#include "segacd/lc89510.h"

#define CYCLE_FOR_TAKE_Z80_BUS_SEGACD 32

/**
 * Z80 bus tables. (Defined in mem_z80.c; not declared in mem_z80.h.)
 * These typedefs must match the ones in mem_z80.c.
 */
typedef uint8_t FASTCALL (*z80_ReadB_fn) (uint32_t address);
typedef void    FASTCALL (*z80_WriteB_fn)(uint32_t address, uint8_t data);
extern const z80_ReadB_fn  Z80_ReadB_Table[16];
extern const z80_WriteB_fn Z80_WriteB_Table[16];


/** Data. **/

int BRAM_Ex_State;
int BRAM_Ex_Size;

int S68K_State;
int CPL_S68K;
int Cycles_S68K;

// RAM cart. (Declared in mem_m68k.h.)
unsigned char Ram_Backup_Ex[512 * 1024];


/**
 * Helpers for the asm's partial-register stores into 32-bit variables.
 * (e.g. "mov [var], al" / "setnz [var]" only modify the low byte.)
 */
static inline int set_low_byte_int(int var, uint8_t val)
{
	return (int)(((uint32_t)var & 0xFFFFFF00U) | val);
}
static inline unsigned int set_low_byte_uint(unsigned int var, uint8_t val)
{
	return ((var & 0xFFFFFF00U) | val);
}
static inline unsigned int set_low_word_uint(unsigned int var, uint16_t val)
{
	return ((var & 0xFFFF0000U) | val);
}

/**
 * COMM.Command / COMM.Status byte access.
 * The asm indexes these unsigned short arrays as bytes with (offset ^ 1),
 * i.e. 68000 byte order on top of host-endian words.
 * @param b 68000 byte offset into the array.
 */
static inline uint8_t comm_read_byte(const unsigned short *arr, unsigned int b)
{
	const uint16_t w = arr[b >> 1];
	return ((b & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8));
}
static inline void comm_write_byte(unsigned short *arr, unsigned int b, uint8_t data)
{
	const uint16_t w = arr[b >> 1];
	if (b & 1)
		arr[b >> 1] = (unsigned short)((w & 0xFF00) | data);
	else
		arr[b >> 1] = (unsigned short)((w & 0x00FF) | (data << 8));
}

/**
 * Memory_Control_Status is a 4-byte table in the asm. (db 1, 2, 4, 5)
 * It's declared as an unsigned int in mem_s68k.h, so access it bytewise.
 */
static inline uint8_t *mem_ctrl_status_bytes(void)
{
	return (uint8_t*)&Memory_Control_Status;
}


/** Forward declarations. **/

static uint8_t M68K_Read_Byte_Bios_CD(uint32_t address);
static uint8_t M68K_Read_Byte_WRam(uint32_t address);
static uint8_t M68K_Read_Byte_BRAM_L(uint32_t address);
static uint8_t M68K_Read_Byte_BRAM(uint32_t address);
static uint8_t M68K_Read_Byte_BRAM_W(uint32_t address);
static uint8_t M68K_Read_Byte_Misc_CD(uint32_t address);

static uint16_t M68K_Read_Word_Bios_CD(uint32_t address);
static uint16_t M68K_Read_Word_WRam(uint32_t address);
static uint16_t M68K_Read_Word_BRAM_L(uint32_t address);
static uint16_t M68K_Read_Word_BRAM(uint32_t address);
static uint16_t M68K_Read_Word_BRAM_W(uint32_t address);
static uint16_t M68K_Read_Word_Misc_CD(uint32_t address);

static void M68K_Write_Byte_Bios_CD(uint32_t address, uint8_t data);
static void M68K_Write_Byte_WRam(uint32_t address, uint8_t data);
static void M68K_Write_Byte_BRAM(uint32_t address, uint8_t data);
static void M68K_Write_Byte_BRAM_W(uint32_t address, uint8_t data);
static void M68K_Write_Byte_Misc_CD(uint32_t address, uint8_t data);

static void M68K_Write_Word_Bios_CD(uint32_t address, uint16_t data);
static void M68K_Write_Word_WRam(uint32_t address, uint16_t data);
static void M68K_Write_Word_BRAM(uint32_t address, uint16_t data);
static void M68K_Write_Word_BRAM_W(uint32_t address, uint16_t data);
static void M68K_Write_Word_Misc_CD(uint32_t address, uint16_t data);


/** SegaCD Default Jump Tables. **/

const M68K_Read_Byte_fn SegaCD_M68K_Read_Byte_Table[0x20] =
{
	M68K_Read_Byte_Bios_CD,		// 0x000000 - 0x07FFFF
	M68K_Read_Byte_Bad,		// 0x080000 - 0x0FFFFF
	M68K_Read_Byte_Bad,		// 0x100000 - 0x17FFFF
	M68K_Read_Byte_Bad,		// 0x180000 - 0x1FFFFF
	M68K_Read_Byte_WRam,		// 0x200000 - 0x27FFFF
	M68K_Read_Byte_Bad,		// 0x280000 - 0x2FFFFF
	M68K_Read_Byte_Bad,		// 0x300000 - 0x37FFFF
	M68K_Read_Byte_Bad,		// 0x380000 - 0x3FFFFF
	M68K_Read_Byte_BRAM_L,		// 0x400000 - 0x47FFFF
	M68K_Read_Byte_Bad,		// 0x480000 - 0x4FFFFF
	M68K_Read_Byte_Bad,		// 0x500000 - 0x57FFFF
	M68K_Read_Byte_Bad,		// 0x580000 - 0x5FFFFF
	M68K_Read_Byte_BRAM,		// 0x600000 - 0x67FFFF
	M68K_Read_Byte_BRAM,		// 0x680000 - 0x6FFFFF
	M68K_Read_Byte_Bad,		// 0x700000 - 0x77FFFF
	M68K_Read_Byte_BRAM_W,		// 0x780000 - 0x7FFFFF
	M68K_Read_Byte_Bad,		// 0x800000 - 0x87FFFF
	M68K_Read_Byte_Bad,		// 0x880000 - 0x8FFFFF
	M68K_Read_Byte_Bad,		// 0x900000 - 0x97FFFF
	M68K_Read_Byte_Bad,		// 0x980000 - 0x9FFFFF
	M68K_Read_Byte_Misc_CD,		// 0xA00000 - 0xA7FFFF
	M68K_Read_Byte_Bad,		// 0xA80000 - 0xAFFFFF
	M68K_Read_Byte_Bad,		// 0xB00000 - 0xB7FFFF
	M68K_Read_Byte_Bad,		// 0xB80000 - 0xBFFFFF
	M68K_Read_Byte_VDP,		// 0xC00000 - 0xC7FFFF
	M68K_Read_Byte_Bad,		// 0xC80000 - 0xCFFFFF
	M68K_Read_Byte_Bad,		// 0xD00000 - 0xD7FFFF
	M68K_Read_Byte_Bad,		// 0xD80000 - 0xDFFFFF
	M68K_Read_Byte_Ram,		// 0xE00000 - 0xE7FFFF
	M68K_Read_Byte_Ram,		// 0xE80000 - 0xEFFFFF
	M68K_Read_Byte_Ram,		// 0xF00000 - 0xF7FFFF
	M68K_Read_Byte_Ram,		// 0xF80000 - 0xFFFFFF
};

const M68K_Read_Word_fn SegaCD_M68K_Read_Word_Table[0x20] =
{
	M68K_Read_Word_Bios_CD,		// 0x000000 - 0x07FFFF
	M68K_Read_Word_Bad,		// 0x080000 - 0x0FFFFF
	M68K_Read_Word_Bad,		// 0x100000 - 0x17FFFF
	M68K_Read_Word_Bad,		// 0x180000 - 0x1FFFFF
	M68K_Read_Word_WRam,		// 0x200000 - 0x27FFFF
	M68K_Read_Word_Bad,		// 0x280000 - 0x2FFFFF
	M68K_Read_Word_Bad,		// 0x300000 - 0x37FFFF
	M68K_Read_Word_Bad,		// 0x380000 - 0x3FFFFF
	M68K_Read_Word_BRAM_L,		// 0x400000 - 0x47FFFF
	M68K_Read_Word_Bad,		// 0x480000 - 0x4FFFFF
	M68K_Read_Word_Bad,		// 0x500000 - 0x57FFFF
	M68K_Read_Word_Bad,		// 0x580000 - 0x5FFFFF
	M68K_Read_Word_BRAM,		// 0x600000 - 0x67FFFF
	M68K_Read_Word_BRAM,		// 0x680000 - 0x6FFFFF
	M68K_Read_Word_Bad,		// 0x700000 - 0x77FFFF
	M68K_Read_Word_BRAM_W,		// 0x780000 - 0x7FFFFF
	M68K_Read_Word_Bad,		// 0x800000 - 0x87FFFF
	M68K_Read_Word_Bad,		// 0x880000 - 0x8FFFFF
	M68K_Read_Word_Bad,		// 0x900000 - 0x97FFFF
	M68K_Read_Word_Bad,		// 0x980000 - 0x9FFFFF
	M68K_Read_Word_Misc_CD,		// 0xA00000 - 0xA7FFFF
	M68K_Read_Word_Bad,		// 0xA80000 - 0xAFFFFF
	M68K_Read_Word_Bad,		// 0xB00000 - 0xB7FFFF
	M68K_Read_Word_Bad,		// 0xB80000 - 0xBFFFFF
	M68K_Read_Word_VDP,		// 0xC00000 - 0xC7FFFF
	M68K_Read_Word_Bad,		// 0xC80000 - 0xCFFFFF
	M68K_Read_Word_Bad,		// 0xD00000 - 0xD7FFFF
	M68K_Read_Word_Bad,		// 0xD80000 - 0xDFFFFF
	M68K_Read_Word_Ram,		// 0xE00000 - 0xE7FFFF
	M68K_Read_Word_Ram,		// 0xE80000 - 0xEFFFFF
	M68K_Read_Word_Ram,		// 0xF00000 - 0xF7FFFF
	M68K_Read_Word_Ram,		// 0xF80000 - 0xFFFFFF
};

const M68K_Write_Byte_fn SegaCD_M68K_Write_Byte_Table[0x10] =
{
	M68K_Write_Byte_Bios_CD,	// 0x000000 - 0x0FFFFF
	M68K_Write_Byte_Bad,		// 0x100000 - 0x1FFFFF
	M68K_Write_Byte_WRam,		// 0x200000 - 0x2FFFFF
	M68K_Write_Byte_Bad,		// 0x300000 - 0x3FFFFF
	M68K_Write_Byte_Bad,		// 0x400000 - 0x4FFFFF
	M68K_Write_Byte_Bad,		// 0x500000 - 0x5FFFFF
	M68K_Write_Byte_BRAM,		// 0x600000 - 0x6FFFFF
	M68K_Write_Byte_BRAM_W,		// 0x700000 - 0x7FFFFF
	M68K_Write_Byte_Bad,		// 0x800000 - 0x8FFFFF
	M68K_Write_Byte_Bad,		// 0x900000 - 0x9FFFFF
	M68K_Write_Byte_Misc_CD,	// 0xA00000 - 0xAFFFFF
	M68K_Write_Byte_Bad,		// 0xB00000 - 0xBFFFFF
	M68K_Write_Byte_VDP,		// 0xC00000 - 0xCFFFFF
	M68K_Write_Byte_Bad,		// 0xD00000 - 0xDFFFFF
	M68K_Write_Byte_Ram,		// 0xE00000 - 0xEFFFFF
	M68K_Write_Byte_Ram,		// 0xF00000 - 0xFFFFFF
};

const M68K_Write_Word_fn SegaCD_M68K_Write_Word_Table[0x10] =
{
	M68K_Write_Word_Bios_CD,	// 0x000000 - 0x0FFFFF
	M68K_Write_Word_Bad,		// 0x100000 - 0x1FFFFF
	M68K_Write_Word_WRam,		// 0x200000 - 0x2FFFFF
	M68K_Write_Word_Bad,		// 0x300000 - 0x3FFFFF
	M68K_Write_Word_Bad,		// 0x400000 - 0x4FFFFF
	M68K_Write_Word_Bad,		// 0x500000 - 0x5FFFFF
	M68K_Write_Word_BRAM,		// 0x600000 - 0x6FFFFF
	M68K_Write_Word_BRAM_W,		// 0x700000 - 0x7FFFFF
	M68K_Write_Word_Bad,		// 0x800000 - 0x8FFFFF
	M68K_Write_Word_Bad,		// 0x900000 - 0x9FFFFF
	M68K_Write_Word_Misc_CD,	// 0xA00000 - 0xAFFFFF
	M68K_Write_Word_Bad,		// 0xB00000 - 0xBFFFFF
	M68K_Write_Word_VDP,		// 0xC00000 - 0xCFFFFF
	M68K_Write_Word_Bad,		// 0xD00000 - 0xDFFFFF
	M68K_Write_Word_Ram,		// 0xE00000 - 0xEFFFFF
	M68K_Write_Word_Ram,		// 0xF00000 - 0xFFFFFF
};


/** Shared helpers. **/

/**
 * md_version_register(): Genesis version register.
 * Format: [MODE VMOD DISK RSV VER3 VER2 VER1 VER0]
 * MODE: Region. (0 == East; 1 == West)
 * VMOD: Video mode. (0 == NTSC; 1 == PAL)
 * DISK: Floppy disk drive. (0 == connected; 1 == not connected.)
 * RSV: Reserved. (TODO: I think this is used for SegaCD, but I'm not sure.)
 * VER 3-0: HW version. (0 == no TMSS; 1 = TMSS)
 * TODO: Set VER to 1 once TMSS support is added, if TMSS is enabled.
 */
static inline uint8_t md_version_register(void)
{
	// Set region and video mode.
	uint32_t ver = (((uint32_t)Game_Mode << 1) | (uint32_t)CPU_Mode) << 6;

	// Mark floppy drive as not connected.
	ver |= 0x20;

	// Mask off any high bits and return.
	return (uint8_t)(ver & 0xFF);
}

/**
 * prg_ram_bank_offset(): Program RAM offset for 0x020000 - 0x03FFFF.
 * asm: add ebx, [Bank_M68K]; ... [Ram_Prg + ebx - 0x20000]
 */
static inline uint32_t prg_ram_bank_offset(uint32_t address)
{
	return (address + (uint32_t)Bank_M68K - 0x20000);
}

/**
 * cell_conv(): Cell-arranged Word RAM address conversion. (0x220000 - 0x23FFFF)
 * asm: movzx ebx, word [Cell_Conv_Tab + (address & ~1) - 0x220000]
 * @return Word index into the selected 1M Word RAM bank.
 */
static inline uint32_t cell_conv(uint32_t address)
{
	return Cell_Conv_Tab[(address - 0x220000) >> 1];
}

/**
 * fake_fetch_next(): Fake the next fetched instruction. (random)
 * asm: movzx eax, byte [Fake_Fetch]; xor al, 0xFF; mov [Fake_Fetch], al
 * @return New low byte of Fake_Fetch.
 */
static inline uint8_t fake_fetch_next(void)
{
	uint8_t al = (uint8_t)(Fake_Fetch ^ 0xFF);
	Fake_Fetch = set_low_byte_int(Fake_Fetch, al);
	return al;
}


/******** SegaCD extended Read Byte ********/

static uint8_t M68K_Read_Byte_Bios_CD(uint32_t address)
{
	if (address <= 0x1FFFF)
		return Rom_Data.u8[address ^ 1];

	// Bank RAM.
	if (address > 0x3FFFF)
		return M68K_Read_Byte_Bad(address);

	if ((uint8_t)S68K_State == 1)	// BUS available ?
		return M68K_Read_Byte_Bad(address);

	return Ram_Prg.u8[prg_ram_bank_offset(address) ^ 1];
}

static uint8_t M68K_Read_Byte_WRam(uint32_t address)
{
	if (address > 0x23FFFF)
		return 0;

	switch (Ram_Word_State & 3)
	{
		case 0:
		case 1:
		default:
			// Word RAM: 2M mode.
			return Ram_Word_2M.u8[(address - 0x200000) ^ 1];

		case 2:
			// Word RAM: 1M mode, bank 0.
			if (address <= 0x21FFFF)
				return Ram_Word_1M.u8[(address - 0x200000) ^ 1];

			// Cell arranged.
			// NOTE: The asm adds (address & 1) to the byte offset without
			// the usual (^ 1), so this reads the *other* byte of the word.
			return Ram_Word_1M.u8[(cell_conv(address) * 2) + (address & 1)];

		case 3:
			// Word RAM: 1M mode, bank 1.
			if (address <= 0x21FFFF)
				return Ram_Word_1M.u8[((address - 0x200000) ^ 1) + 0x20000];

			// Cell arranged. (See NOTE above.)
			return Ram_Word_1M.u8[(cell_conv(address) * 2) + (address & 1) + 0x20000];
	}
}

static uint8_t M68K_Read_Byte_BRAM_L(uint32_t address)
{
	if (address != 0x400001)
		return 0;

	// Get the BRAM size identifier.
	return (uint8_t)(BRAM_Ex_Size & 7);
}

static uint8_t M68K_Read_Byte_BRAM(uint32_t address)
{
	// TODO: Restrict accessible area based on BRAM size.
	if (!(BRAM_Ex_State & 0x100))
		return 0;

	// BRAM is written on odd (?) addresses only.
	// Thus, for 512 KB, a mask of 0xFFFFF (1 MB) is needed.
	return Ram_Backup_Ex[(address & 0xFFFFF) >> 1];
}

static uint8_t M68K_Read_Byte_BRAM_W(uint32_t address)
{
	if (address != 0x7FFFFF)
		return 0;

	return (uint8_t)BRAM_Ex_State;
}

/**
 * cd_reg_read_byte(): Read a byte from the SegaCD registers. (0xA12000 - 0xA1202F)
 * @param reg Register offset. (address & 0x3F)
 */
static uint8_t cd_reg_read_byte(unsigned int reg)
{
	switch (reg)
	{
		case 0x00:	// S68K_Ctrl_H
			return (uint8_t)(((uint8_t)Int_Mask_S68K & 4) << 5);

		case 0x01:	// S68K_Ctrl_L
			return (uint8_t)S68K_State;

		case 0x02:	// Memory_Ctrl_H
			return (uint8_t)S68K_Mem_WP;

		case 0x03:	// Memory_Ctrl_L
			return (uint8_t)(((uint32_t)Bank_M68K >> 11) |
				mem_ctrl_status_bytes()[Ram_Word_State & 3]);

		case 0x04:	// CDC_Mode_H
			return (uint8_t)(CDC.RS0 >> 8);

		case 0x06:	// HINT_Vector_H
			return Rom_Data.u8[0x73];

		case 0x07:	// HINT_Vector_L
			return Rom_Data.u8[0x72];

		case 0x0C:	// Stop_Watch_H
			return (uint8_t)(CDC.Stop_Watch >> 24);

		case 0x0D:	// Stop_Watch_L
			return (uint8_t)(CDC.Stop_Watch >> 16);

		case 0x0E:	// Com_Flag_H
			return (uint8_t)(COMM.Flag >> 8);

		case 0x0F:	// Com_Flag_L
			return (uint8_t)COMM.Flag;

		case 0x05:	// CDC_Mode_L
		case 0x08:	// CDC_Host_Data_H
		case 0x09:	// CDC_Host_Data_L
		case 0x0A:	// Unknow
		case 0x0B:	// Unknow
			return 0;

		default:
			break;
	}

	if (reg < 0x20)
	{
		// Com_D0 - Com_D7
		return comm_read_byte(COMM.Command, reg - 0x10);
	}

	// Com_S0 - Com_S7
	return comm_read_byte(COMM.Status, reg - 0x20);
}

static uint8_t M68K_Read_Byte_Misc_CD(uint32_t address)
{
	if (address <= 0xA0FFFF)
	{
		// Z80 memory.
		if (Z80_State & (Z80_STATE_BUSREQ | Z80_STATE_RESET))
			return 0;

		return Z80_ReadB_Table[(address & 0x7000) >> 12](address & 0x7FFF);
	}

	if (address == 0xA11100)
	{
		// Z80 bus request.
		if (Z80_State & Z80_STATE_BUSREQ)
			return 0x81;	// Z80 on.

		// Z80 off. (unsigned compare: ja)
		if ((uint32_t)(main68k_readOdometer() - (uint32_t)Last_BUS_REQ_Cnt) >
		    CYCLE_FOR_TAKE_Z80_BUS_SEGACD)
		{
			// Bus taken.
			return 0x80;
		}

		return ((uint8_t)Last_BUS_REQ_St | 0x80);
	}

	if (address <= 0xA1000D)
	{
		// I/O ports.
		switch (address & 0x0E)
		{
			case 0x00:	// MD_Spec
				return md_version_register();
			case 0x02:	// Pad_1
				return RD_Controller_1();
			case 0x04:	// Pad_2
				return RD_Controller_2();
			case 0x08:	// CT_Pad_1
				return (uint8_t)Controller_1_COM;
			case 0x0A:	// CT_Pad_2
				return (uint8_t)Controller_2_COM;
			default:	// bad
				return 0;
		}
	}

	// SegaCD registers.
	if (address < 0xA12000 || address > 0xA1202F)
		return 0;

	return cd_reg_read_byte(address & 0x3F);
}


/******** SegaCD extended Read Word ********/

static uint16_t M68K_Read_Word_Bios_CD(uint32_t address)
{
	if (address <= 0x1FFFF)
		return Rom_Data.u16[address >> 1];

	// Bank RAM.
	if (address > 0x3FFFF)
		return M68K_Read_Word_Bad(address);

	if ((uint8_t)S68K_State == 1)	// BUS available ?
		return M68K_Read_Byte_Bad(address);	// (sic; returns 0)

	return Ram_Prg.u16[prg_ram_bank_offset(address) >> 1];
}

static uint16_t M68K_Read_Word_WRam(uint32_t address)
{
	if (address > 0x23FFFF)
		return 0;

	switch (Ram_Word_State & 3)
	{
		case 0:
		case 1:
		default:
			// Word RAM: 2M mode.
			return Ram_Word_2M.u16[(address - 0x200000) >> 1];

		case 2:
			// Word RAM: 1M mode, bank 0.
			if (address <= 0x21FFFF)
				return Ram_Word_1M.u16[(address - 0x200000) >> 1];

			// Cell arranged.
			return Ram_Word_1M.u16[cell_conv(address)];

		case 3:
			// Word RAM: 1M mode, bank 1.
			if (address <= 0x21FFFF)
				return Ram_Word_1M.u16[((address - 0x200000) + 0x20000) >> 1];

			// Cell arranged.
			return Ram_Word_1M.u16[cell_conv(address) + (0x20000 >> 1)];
	}
}

static uint16_t M68K_Read_Word_BRAM_L(uint32_t address)
{
	if (address != 0x400000)
		return 0;

	// Get the BRAM size identifier.
	return (uint16_t)(BRAM_Ex_Size & 7);
}

static uint16_t M68K_Read_Word_BRAM(uint32_t address)
{
	// TODO: Restrict accessible area based on BRAM size.
	if (!(BRAM_Ex_State & 0x100))
		return 0;

	// BRAM is written on odd (?) addresses only.
	// Thus, for 512 KB, a mask of 0xFFFFF (1 MB) is needed.
	return Ram_Backup_Ex[(address & 0xFFFFF) >> 1];
}

static uint16_t M68K_Read_Word_BRAM_W(uint32_t address)
{
	if (address != 0x7FFFFE)
		return 0;

	return (uint8_t)BRAM_Ex_State;
}

/**
 * cd_reg_read_word(): Read a word from the SegaCD registers. (0xA12000 - 0xA1202E)
 * @param reg Register offset. (address & 0x3E)
 */
static uint16_t cd_reg_read_word(unsigned int reg)
{
	switch (reg)
	{
		case 0x00:	// S68K_Ctrl
			return (uint16_t)(((((uint8_t)Int_Mask_S68K & 4) << 5) << 8) |
					  (uint8_t)S68K_State);

		case 0x02:	// Memory_Ctrl
			return (uint16_t)(((uint8_t)S68K_Mem_WP << 8) |
				(uint8_t)(((uint32_t)Bank_M68K >> 11) |
					  mem_ctrl_status_bytes()[Ram_Word_State & 3]));

		case 0x04:	// CDC_Mode
			return (uint16_t)(((CDC.RS0 >> 8) & 0xFF) << 8);

		case 0x06:	// HINT_Vector
			return Rom_Data.u16[0x72 >> 1];

		case 0x08:	// CDC_Host_Data
			return Read_CDC_Host_MAIN();

		case 0x0A:	// Unknow
			return 0;

		case 0x0C:	// Stop_Watch
			return (uint16_t)(CDC.Stop_Watch >> 16);

		case 0x0E:	// Com_Flag
			return (uint16_t)COMM.Flag;

		default:
			break;
	}

	if (reg < 0x20)
	{
		// Com_D0 - Com_D7
		return COMM.Command[(reg - 0x10) >> 1];
	}

	// Com_S0 - Com_S7
	return COMM.Status[(reg - 0x20) >> 1];
}

static uint16_t M68K_Read_Word_Misc_CD(uint32_t address)
{
	if (address <= 0xA0FFFF)
	{
		// Z80 memory.
		// NOTE: The asm calls the Z80 *byte* read function and returns its
		// result as-is, so the word's high byte is 0.
		if (Z80_State & (Z80_STATE_BUSREQ | Z80_STATE_RESET))
			return 0;

		return Z80_ReadB_Table[(address & 0x7000) >> 12](address & 0x7FFF);
	}

	if (address == 0xA11100)
	{
		// Z80 bus request.
		uint8_t al, ah;

		if (Z80_State & Z80_STATE_BUSREQ)
		{
			// Z80 on.
			al = fake_fetch_next();
			return (uint16_t)(0x8100 | al);
		}

		// Z80 off. (unsigned compare: ja)
		if ((uint32_t)(main68k_readOdometer() - (uint32_t)Last_BUS_REQ_Cnt) >
		    CYCLE_FOR_TAKE_Z80_BUS_SEGACD)
		{
			// Bus taken.
			al = fake_fetch_next();
			return (uint16_t)(0x8000 | al);
		}

		ah = (uint8_t)((uint8_t)Last_BUS_REQ_St | 0x80);
		al = fake_fetch_next();
		return (uint16_t)((ah << 8) | al);
	}

	if (address <= 0xA1000D)
	{
		// I/O ports.
		switch (address & 0x0E)
		{
			case 0x00:	// MD_Spec
				return md_version_register();
			case 0x02:	// Pad_1
				return RD_Controller_1();
			case 0x04:	// Pad_2
				return RD_Controller_2();
			case 0x08:	// CT_Pad_1 (asm: movzx eax, word [Controller_1_COM])
				return (uint16_t)Controller_1_COM;
			case 0x0A:	// CT_Pad_2 (asm: movzx eax, word [Controller_2_COM])
				return (uint16_t)Controller_2_COM;
			default:	// bad
				return 0;
		}
	}

	// SegaCD registers.
	if (address < 0xA12000 || address > 0xA1202F)
		return 0;

	return cd_reg_read_word(address & 0x3E);
}


/******** Shared write helpers ********/

/**
 * z80_busreq_write(): Handle a write to the Z80 bus request register. (0xA11100)
 * Same as the Genesis version in mem_m68k.c.
 * @param release Nonzero if bit 0 of the relevant data byte is set.
 * (Byte write: data & 1; word write: (data >> 8) & 1.)
 */
static void z80_busreq_write(int release)
{
	uint32_t odo;
	int32_t cyc_idx;

	Controller_1_Counter = 0;
	Controller_1_Delay = 0;
	Controller_2_Counter = 0;
	Controller_2_Delay = 0;

	if (!release)
	{
		// TODO: Should this be checking Z80_STATE_ENABLED?
		if (Z80_State & Z80_STATE_BUSREQ)
			return;	// already activated

		// asm: or ah, Z80_STATE_BUSREQ; mov [Z80_State], ah (low byte only)
		Z80_State |= Z80_STATE_BUSREQ;

		odo = main68k_readOdometer();
		cyc_idx = (int32_t)((uint32_t)Cycles_M68K - odo);
		mdZ80_set_odo(&M_Z80,
			(unsigned int)((uint32_t)Cycles_Z80 - (uint32_t)Z80_M68K_Cycle_Tab[cyc_idx]));
		return;
	}

	// Deactivated.
	odo = main68k_readOdometer();
	Last_BUS_REQ_Cnt = (int)odo;
	// asm: setnz byte [Last_BUS_REQ_St]
	Last_BUS_REQ_St = set_low_byte_int(Last_BUS_REQ_St,
				((Z80_State & Z80_STATE_BUSREQ) ? 1 : 0));
	if (!(Z80_State & Z80_STATE_BUSREQ))
		return;	// already deactivated

	// asm: and cl, ~Z80_STATE_BUSREQ; mov [Z80_State], cl (low byte only)
	Z80_State &= ~(unsigned int)Z80_STATE_BUSREQ;

	cyc_idx = (int32_t)((uint32_t)Cycles_M68K - odo);
	z80_Exec(&M_Z80,
		(int)((uint32_t)Cycles_Z80 - (uint32_t)Z80_M68K_Cycle_Tab[cyc_idx]));
}

/**
 * z80_reset_write(): Handle a write to the Z80 reset register. (0xA11200)
 * @param no_reset Nonzero if bit 0 of the relevant data byte is set.
 */
static void z80_reset_write(int no_reset)
{
	if (!no_reset)
	{
		mdZ80_reset(&M_Z80);
		Z80_State |= Z80_STATE_RESET;
		YM2612_Reset();
	}
	else
	{
		Z80_State &= ~(unsigned int)Z80_STATE_RESET;
	}
}

/**
 * s68k_ctrl_write_low(): Sub 68000 control, low byte. (0xA12001)
 * Bit 0: SRES (0 == reset; 1 == run), Bit 1: SBRQ (bus request).
 */
static void s68k_ctrl_write_low(uint8_t data)
{
	if ((data & 1) && !((uint8_t)S68K_State & 1))
	{
		// S68K_Restart.
		sub68k_reset();
	}

	// S68K_Reseting / S68K_Already_Running.
	// asm: and al, 3; mov [S68K_State], al (low byte only)
	S68K_State = set_low_byte_int(S68K_State, (uint8_t)(data & 3));
}

/**
 * s68k_ctrl_write_high(): Sub 68000 control, high byte. (0xA12000)
 * Bit 0: IFL2. (Trigger INT2 on the Sub 68000.)
 */
static void s68k_ctrl_write_high(uint8_t data)
{
	if (!(data & 1))
		return;
	if (!((uint8_t)Int_Mask_S68K & 0x4))
		return;

	sub68k_interrupt(2, -1);
}

/**
 * memory_ctrl_write_low(): Memory mode, low byte. (0xA12003)
 * Bits 7-6: Program RAM bank. Bit 1: DMNA.
 * (The word write uses the full word here, but only bits 7-6 and 1 matter.)
 */
static void memory_ctrl_write_low(uint32_t data)
{
	const uint32_t bank = (data & 0xC0) << 11;
	const uint32_t dmna = (data >> 1) & 1;

	if (!((uint8_t)Ram_Word_State & 0x2))
	{
		// Mode 2M.
		Bank_M68K = (int)bank;
		if (dmna)
		{
			// asm: mov byte [Ram_Word_State], 1 (low byte only)
			Ram_Word_State = set_low_byte_uint(Ram_Word_State, 1);
			MS68K_Set_Word_Ram();
		}
		M68K_Set_Prg_Ram();
		return;
	}

	// Mode 1M.
	if (!dmna)
	{
		// DMNA bit = 1
		// asm: or word [Memory_Control_Status + 2], 0x0202
		uint8_t *mcs = mem_ctrl_status_bytes();
		mcs[2] |= 0x02;
		mcs[3] |= 0x02;
	}

	Bank_M68K = (int)bank;
	M68K_Set_Prg_Ram();
}


/******** SegaCD extended Write Byte ********/

static void M68K_Write_Byte_Bios_CD(uint32_t address, uint8_t data)
{
	if (address < 0x20000 || address > 0x3FFFF)
		return;

	if ((uint8_t)S68K_State == 1)	// BUS available ?
		return;

	Ram_Prg.u8[prg_ram_bank_offset(address) ^ 1] = data;
}

static void M68K_Write_Byte_WRam(uint32_t address, uint8_t data)
{
	// NOTE: For address > 0x23FFFF, the asm's ".bad" path doesn't pop ecx,
	// which unbalances the stack. Here, it's simply ignored.
	if (address > 0x23FFFF)
		return;

	switch (Ram_Word_State & 3)
	{
		case 0:
		case 1:
		default:
			// Word RAM: 2M mode.
			Ram_Word_2M.u8[(address - 0x200000) ^ 1] = data;
			break;

		case 2:
			// Word RAM: 1M mode, bank 0.
			if (address <= 0x21FFFF)
			{
				Ram_Word_1M.u8[(address - 0x200000) ^ 1] = data;
				break;
			}

			// Cell arranged.
			// NOTE: Same byte offset quirk as M68K_Read_Byte_WRam. (no ^ 1)
			Ram_Word_1M.u8[(cell_conv(address) * 2) + (address & 1)] = data;
			break;

		case 3:
			// Word RAM: 1M mode, bank 1.
			if (address <= 0x21FFFF)
			{
				Ram_Word_1M.u8[((address - 0x200000) ^ 1) + 0x20000] = data;
				break;
			}

			// Cell arranged. (See NOTE above.)
			Ram_Word_1M.u8[(cell_conv(address) * 2) + (address & 1) + 0x20000] = data;
			break;
	}
}

static void M68K_Write_Byte_BRAM(uint32_t address, uint8_t data)
{
	// TODO: Restrict accessible area based on BRAM size.
	// asm: cmp word [BRAM_Ex_State], 0x101
	if ((BRAM_Ex_State & 0xFFFF) != 0x101)
		return;

	// BRAM is written on odd (?) addresses only.
	// Thus, for 512 KB, a mask of 0xFFFFF (1 MB) is needed.
	Ram_Backup_Ex[(address & 0xFFFFF) >> 1] = data;
}

static void M68K_Write_Byte_BRAM_W(uint32_t address, uint8_t data)
{
	if (address != 0x7FFFFF)
		return;

	// asm: mov [BRAM_Ex_State], al (low byte only)
	BRAM_Ex_State = set_low_byte_int(BRAM_Ex_State, data);
}

/**
 * cd_reg_write_byte(): Write a byte to the SegaCD registers. (0xA12000 - 0xA1202F)
 * @param reg Register offset. (address & 0x3F)
 */
static void cd_reg_write_byte(unsigned int reg, uint8_t data)
{
	switch (reg)
	{
		case 0x00:	// S68K_Ctrl_H
			s68k_ctrl_write_high(data);
			return;

		case 0x01:	// S68K_Ctrl_L
			s68k_ctrl_write_low(data);
			return;

		case 0x02:	// Memory_Ctrl_H
			// asm: mov [S68K_Mem_WP], al (low byte only)
			S68K_Mem_WP = set_low_byte_uint(S68K_Mem_WP, data);
			return;

		case 0x03:	// Memory_Ctrl_L
			memory_ctrl_write_low(data);
			return;

		case 0x06:	// HINT_Vector_H
			Rom_Data.u8[0x73] = data;
			return;

		case 0x07:	// HINT_Vector_L
			Rom_Data.u8[0x72] = data;
			return;

		case 0x0E:	// Com_Flag_H
			// asm: mov [COMM.Flag + 1], al
			COMM.Flag = (COMM.Flag & ~0xFF00U) | ((unsigned int)data << 8);
			return;

		case 0x0F:	// Com_Flag_L
			// asm: rol al, 1; mov byte [COMM.Flag + 1], al
			data = (uint8_t)((data << 1) | (data >> 7));
			COMM.Flag = (COMM.Flag & ~0xFF00U) | ((unsigned int)data << 8);
			return;

		case 0x04:	// CDC_Mode_H
		case 0x05:	// CDC_Mode_L
		case 0x08:	// CDC_Host_Data_H
		case 0x09:	// CDC_Host_Data_L
		case 0x0A:	// Unknow
		case 0x0B:	// Unknow
		case 0x0C:	// Stop_Watch_H
		case 0x0D:	// Stop_Watch_L
			return;

		default:
			break;
	}

	if (reg < 0x20)
	{
		// Com_D0 - Com_D7
		comm_write_byte(COMM.Command, reg - 0x10, data);
	}

	// Com_S0 - Com_S7: read-only.
}

static void M68K_Write_Byte_Misc_CD(uint32_t address, uint8_t data)
{
	if (address <= 0xA0FFFF)
	{
		// Z80 memory.
		if (Z80_State & (Z80_STATE_BUSREQ | Z80_STATE_RESET))
			return;

		Z80_WriteB_Table[(address & 0x7000) >> 12](address & 0x7FFF, data);
		return;
	}

	if (address == 0xA11100)
	{
		// Z80 bus request.
		// TODO: Should this be ah, Z80_STATE_ENABLED ?
		z80_busreq_write(data & 1);
		return;
	}

	if (address == 0xA11200)
	{
		// Z80 reset.
		z80_reset_write(data & 1);
		return;
	}

	if (address <= 0xA1000D)
	{
		// I/O ports.
		switch (address & 0x0E)
		{
			case 0x02:	// Pad_1
				WR_Controller_1(data);
				break;
			case 0x04:	// Pad_2
				WR_Controller_2(data);
				break;
			case 0x08:	// CT_Pad_1 (asm: mov [Controller_1_COM], al)
				Controller_1_COM = set_low_byte_uint(Controller_1_COM, data);
				break;
			case 0x0A:	// CT_Pad_2 (asm: mov [Controller_2_COM], al)
				Controller_2_COM = set_low_byte_uint(Controller_2_COM, data);
				break;
			default:	// bad
				break;
		}
		return;
	}

	// SegaCD registers.
	if (address < 0xA12000 || address > 0xA1202F)
	{
		M68K_Write_Byte_Bad(address, data);
		return;
	}

	cd_reg_write_byte(address & 0x3F, data);
}


/******** SegaCD extended Write Word ********/

static void M68K_Write_Word_Bios_CD(uint32_t address, uint16_t data)
{
	if (address < 0x20000 || address > 0x3FFFF)
		return;

	if ((uint8_t)S68K_State == 1)	// BUS available ?
		return;

	Ram_Prg.u16[prg_ram_bank_offset(address) >> 1] = data;
}

static void M68K_Write_Word_WRam(uint32_t address, uint16_t data)
{
	if (address > 0x23FFFF)
		return;

	switch (Ram_Word_State & 3)
	{
		case 0:
		case 1:
		default:
			// Word RAM: 2M mode.
			Ram_Word_2M.u16[(address - 0x200000) >> 1] = data;
			break;

		case 2:
			// Word RAM: 1M mode, bank 0.
			if (address <= 0x21FFFF)
			{
				Ram_Word_1M.u16[(address - 0x200000) >> 1] = data;
				break;
			}

			// Cell arranged.
			Ram_Word_1M.u16[cell_conv(address)] = data;
			break;

		case 3:
			// Word RAM: 1M mode, bank 1.
			if (address <= 0x21FFFF)
			{
				Ram_Word_1M.u16[((address - 0x200000) + 0x20000) >> 1] = data;
				break;
			}

			// Cell arranged.
			Ram_Word_1M.u16[cell_conv(address) + (0x20000 >> 1)] = data;
			break;
	}
}

static void M68K_Write_Word_BRAM(uint32_t address, uint16_t data)
{
	// TODO: Restrict accessible area based on BRAM size.
	// asm: cmp word [BRAM_Ex_State], 0x101
	if ((BRAM_Ex_State & 0xFFFF) != 0x101)
		return;

	// BRAM is written on odd (?) addresses only.
	// Thus, for 512 KB, a mask of 0xFFFFF (1 MB) is needed.
	// (Only the low byte is written.)
	Ram_Backup_Ex[(address & 0xFFFFF) >> 1] = (uint8_t)data;
}

static void M68K_Write_Word_BRAM_W(uint32_t address, uint16_t data)
{
	if (address != 0x7FFFFE)
		return;

	// asm: mov [BRAM_Ex_State], al (low byte only)
	BRAM_Ex_State = set_low_byte_int(BRAM_Ex_State, (uint8_t)data);
}

/**
 * cd_reg_write_word(): Write a word to the SegaCD registers. (0xA12000 - 0xA1202E)
 * @param reg Register offset. (address & 0x3E)
 */
static void cd_reg_write_word(unsigned int reg, uint16_t data)
{
	switch (reg)
	{
		case 0x00:	// S68K_Ctrl
			// Low byte: SRES / SBRQ. High byte: IFL2.
			// NOTE: The asm restores the data after sub68k_reset() with
			// "pop ax" after "push eax", which unbalances the stack.
			// The intended behavior (data preserved) is used here.
			s68k_ctrl_write_low((uint8_t)(data & 0xFF));
			s68k_ctrl_write_high((uint8_t)(data >> 8));
			return;

		case 0x02:	// Memory_Ctrl
			// asm: mov [S68K_Mem_WP], ah (low byte only)
			S68K_Mem_WP = set_low_byte_uint(S68K_Mem_WP, (uint8_t)(data >> 8));
			memory_ctrl_write_low(data);
			return;

		case 0x06:	// HINT_Vector
			Rom_Data.u16[0x72 >> 1] = data;
			return;

		case 0x0E:	// Com_Flag
			// asm: mov [COMM.Flag + 1], ah
			COMM.Flag = (COMM.Flag & ~0xFF00U) | (data & 0xFF00U);
			return;

		case 0x04:	// CDC_Mode
		case 0x08:	// CDC_Host_Data
		case 0x0A:	// Unknow
		case 0x0C:	// Stop_Watch
			return;

		default:
			break;
	}

	if (reg < 0x20)
	{
		// Com_D0 - Com_D7
		COMM.Command[(reg - 0x10) >> 1] = data;
	}

	// Com_S0 - Com_S7: read-only.
}

static void M68K_Write_Word_Misc_CD(uint32_t address, uint16_t data)
{
	if (address <= 0xA0FFFF)
	{
		// Z80 memory.
		if (Z80_State & (Z80_STATE_BUSREQ | Z80_STATE_RESET))
			return;

		// NOTE: The asm sets dl = dh = al (low byte of the word) before
		// calling the FASTCALL byte write function, so the *low* byte is
		// what gets written.
		Z80_WriteB_Table[(address & 0x7000) >> 12](address & 0x7FFF, (uint8_t)(data & 0xFF));
		return;
	}

	if (address == 0xA11100)
	{
		// Z80 bus request.
		// TODO: Should this be al, Z80_STATE_ENABLED ?
		z80_busreq_write((data >> 8) & 1);
		return;
	}

	if (address == 0xA11200)
	{
		// Z80 reset.
		z80_reset_write((data >> 8) & 1);
		return;
	}

	if (address <= 0xA1000D)
	{
		// I/O ports.
		switch (address & 0x0E)
		{
			case 0x02:	// Pad_1
				WR_Controller_1((unsigned char)data);
				break;
			case 0x04:	// Pad_2
				WR_Controller_2((unsigned char)data);
				break;
			case 0x08:	// CT_Pad_1 (asm: mov [Controller_1_COM], ax)
				Controller_1_COM = set_low_word_uint(Controller_1_COM, data);
				break;
			case 0x0A:	// CT_Pad_2 (asm: mov [Controller_2_COM], ax)
				Controller_2_COM = set_low_word_uint(Controller_2_COM, data);
				break;
			default:	// bad
				break;
		}
		return;
	}

	// SegaCD registers.
	if (address < 0xA12000 || address > 0xA1202F)
		return;

	cd_reg_write_word(address & 0x3E, data);
}
