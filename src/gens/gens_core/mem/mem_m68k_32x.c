/***************************************************************************
 * Gens: Main 68000 memory management. (32X)                               *
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
 * C port of mem_m68k_32x.asm.
 *
 * Memory layout notes: (same conventions as mem_m68k.c / mem_m68k_cd.c)
 * - Rom_Data, _32X_Genesis_Rom and _32X_VDP_Ram store 68000 memory as
 *   host-endian 16-bit words. A 68000 byte at address A is at u8[A ^ 1];
 *   a 68000 word at (even) A is u16[A >> 1].
 * - _32X_Comm is a plain big-endian byte array. (68000 byte order.)
 * - Partial-register loads/stores of 32-bit variables in the asm
 *   ("mov [var + 1], al", "movzx eax, word [var + 2]", ...) use
 *   little-endian byte numbering, as on x86.
 * - All handlers take a 24-bit address. (address & 0xFFFFFF)
 */

#include <stdint.h>
#include <string.h>

#include "mem_m68k.h"
#include "mem_m68k_handlers.h"
#include "mem_m68k_32x.h"
#include "mem_sh2.h"

#include "libgsft/gsft_fastcall.h"

// CPU cores.
#include "gens_core/cpu/68k/star_68k.h"
#include "gens_core/cpu/68k/cpu_68k.h"
#include "gens_core/cpu/sh2/sh2.h"
#include "gens_core/cpu/sh2/cpu_sh2.h"
#include "mdZ80/mdZ80.h"

// I/O, sound, 32X VDP.
#include "gens_core/io/io.h"
#include "gens_core/sound/ym2612.hpp"
#include "gens_core/sound/pwm.h"
#include "gens_core/vdp/vdp_32x.h"

#define CYCLE_FOR_TAKE_Z80_BUS_GENESIS 16

/**
 * Z80 bus tables. (Defined in mem_z80.c; not declared in mem_z80.h.)
 * These typedefs must match the ones in mem_z80.c.
 */
typedef uint8_t FASTCALL (*z80_ReadB_fn) (uint32_t address);
typedef void    FASTCALL (*z80_WriteB_fn)(uint32_t address, uint8_t data);
extern const z80_ReadB_fn  Z80_ReadB_Table[16];
extern const z80_WriteB_fn Z80_WriteB_Table[16];


/** Data. **/

// 32X MC68000 firmware.
unsigned char _32X_Genesis_Rom[1024] __attribute__((aligned(64)));

int Bank_SH2;

// "MARS" ID, as stored in the asm. (byte reads index it directly;
// word reads load a little-endian word from it.)
// The extra byte stands in for whatever followed the table in the asm.
static const uint8_t _32X_ID_Tab[5] = {'A', 'M', 'S', 'R', 0};


/**
 * Helpers for the asm's partial-register stores into 32-bit variables.
 * (e.g. "mov [var], al" / "setnz [var]" only modify the low byte.)
 */
static inline int set_low_byte_int(int var, uint8_t val)
{
	return (int)(((uint32_t)var & 0xFFFFFF00U) | val);
}
static inline unsigned int set_byte_uint(unsigned int var, unsigned int byte_num, uint8_t val)
{
	const unsigned int shift = byte_num * 8;
	return ((var & ~(0xFFU << shift)) | ((unsigned int)val << shift));
}
static inline unsigned int set_low_word_uint(unsigned int var, uint16_t val)
{
	return ((var & 0xFFFF0000U) | val);
}
static inline uint8_t get_byte_uint(unsigned int var, unsigned int byte_num)
{
	return (uint8_t)(var >> (byte_num * 8));
}

/**
 * genesis_rom_word(): Read a host-endian word from _32X_Genesis_Rom.
 * @param offset Byte offset. (even)
 */
static inline uint16_t genesis_rom_word(uint32_t offset)
{
	uint16_t w;
	memcpy(&w, &_32X_Genesis_Rom[offset & 0x3FE], sizeof(w));
	return w;
}


/** Forward declarations. **/

static uint8_t M68K_Read_Byte_Bios_32X(uint32_t address);
static uint8_t M68K_Read_Byte_BiosR_32X(uint32_t address);
static uint8_t M68K_Read_Byte_Misc_32X(uint32_t address);

static uint16_t M68K_Read_Word_Bios_32X(uint32_t address);
static uint16_t M68K_Read_Word_BiosR_32X(uint32_t address);
static uint16_t M68K_Read_Word_Misc_32X(uint32_t address);

static void M68K_Write_Byte_Misc_32X(uint32_t address, uint8_t data);
static void M68K_Write_Word_Misc_32X(uint32_t address, uint16_t data);

// Global in the asm.
uint8_t M68K_Read_Byte_32X_FB0(uint32_t address);
uint8_t M68K_Read_Byte_32X_FB1(uint32_t address);
uint16_t M68K_Read_Word_32X_FB0(uint32_t address);
uint16_t M68K_Read_Word_32X_FB1(uint32_t address);
void M68K_Write_Byte_32X_FB0(uint32_t address, uint8_t data);
void M68K_Write_Byte_32X_FB1(uint32_t address, uint8_t data);
void M68K_Write_Word_32X_FB0(uint32_t address, uint16_t data);
void M68K_Write_Word_32X_FB1(uint32_t address, uint16_t data);


/** 32X Default Jump Tables. **/

M68K_Read_Byte_fn _32X_M68K_Read_Byte_Table[0x20] =
{
	M68K_Read_Byte_Rom0,		// 0x000000 - 0x07FFFF
	M68K_Read_Byte_Rom1,		// 0x080000 - 0x0FFFFF
	M68K_Read_Byte_Rom2,		// 0x100000 - 0x17FFFF
	M68K_Read_Byte_Rom3,		// 0x180000 - 0x1FFFFF
	M68K_Read_Byte_Rom4,		// 0x200000 - 0x27FFFF
	M68K_Read_Byte_Rom5,		// 0x280000 - 0x2FFFFF
	M68K_Read_Byte_Rom6,		// 0x300000 - 0x37FFFF
	M68K_Read_Byte_Rom7,		// 0x380000 - 0x3FFFFF
	M68K_Read_Byte_Bios_32X,	// 0x400000 - 0x47FFFF
	M68K_Read_Byte_BiosR_32X,	// 0x480000 - 0x4FFFFF
	M68K_Read_Byte_Bad,		// 0x500000 - 0x57FFFF
	M68K_Read_Byte_Bad,		// 0x580000 - 0x5FFFFF
	M68K_Read_Byte_Bad,		// 0x600000 - 0x67FFFF
	M68K_Read_Byte_Bad,		// 0x680000 - 0x6FFFFF
	M68K_Read_Byte_Bad,		// 0x700000 - 0x77FFFF
	M68K_Read_Byte_32X_FB0,		// 0x780000 - 0x7FFFFF
	M68K_Read_Byte_32X_FB1,		// 0x800000 - 0x87FFFF
	M68K_Read_Byte_Rom0,		// 0x880000 - 0x8FFFFF
	M68K_Read_Byte_Rom1,		// 0x900000 - 0x97FFFF
	M68K_Read_Byte_Rom2,		// 0x980000 - 0x9FFFFF
	M68K_Read_Byte_Misc_32X,	// 0xA00000 - 0xA7FFFF
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

M68K_Read_Word_fn _32X_M68K_Read_Word_Table[0x20] =
{
	M68K_Read_Word_Rom0,		// 0x000000 - 0x07FFFF
	M68K_Read_Word_Rom1,		// 0x080000 - 0x0FFFFF
	M68K_Read_Word_Rom2,		// 0x100000 - 0x17FFFF
	M68K_Read_Word_Rom3,		// 0x180000 - 0x1FFFFF
	M68K_Read_Word_Rom4,		// 0x200000 - 0x27FFFF
	M68K_Read_Word_Rom5,		// 0x280000 - 0x2FFFFF
	M68K_Read_Word_Rom6,		// 0x300000 - 0x37FFFF
	M68K_Read_Word_Rom7,		// 0x380000 - 0x3FFFFF
	M68K_Read_Word_Bios_32X,	// 0x400000 - 0x47FFFF
	M68K_Read_Word_BiosR_32X,	// 0x480000 - 0x4FFFFF
	M68K_Read_Word_Bad,		// 0x500000 - 0x57FFFF
	M68K_Read_Word_Bad,		// 0x580000 - 0x5FFFFF
	M68K_Read_Word_Bad,		// 0x600000 - 0x67FFFF
	M68K_Read_Word_Bad,		// 0x680000 - 0x6FFFFF
	M68K_Read_Word_Bad,		// 0x700000 - 0x77FFFF
	M68K_Read_Word_32X_FB0,		// 0x780000 - 0x7FFFFF
	M68K_Read_Word_32X_FB1,		// 0x800000 - 0x87FFFF
	M68K_Read_Word_Rom0,		// 0x880000 - 0x8FFFFF
	M68K_Read_Word_Rom1,		// 0x900000 - 0x97FFFF
	M68K_Read_Word_Rom2,		// 0x980000 - 0x9FFFFF
	M68K_Read_Word_Misc_32X,	// 0xA00000 - 0xA7FFFF
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

M68K_Write_Byte_fn _32X_M68K_Write_Byte_Table[0x10] =
{
	M68K_Write_Byte_Bad,		// 0x000000 - 0x0FFFFF
	M68K_Write_Byte_Bad,		// 0x100000 - 0x1FFFFF
	M68K_Write_Byte_SRAM,		// 0x200000 - 0x2FFFFF
	M68K_Write_Byte_Bad,		// 0x300000 - 0x3FFFFF
	M68K_Write_Byte_Bad,		// 0x400000 - 0x4FFFFF
	M68K_Write_Byte_Bad,		// 0x500000 - 0x5FFFFF
	M68K_Write_Byte_Bad,		// 0x600000 - 0x6FFFFF
	M68K_Write_Byte_32X_FB0,	// 0x700000 - 0x7FFFFF
	M68K_Write_Byte_32X_FB1,	// 0x800000 - 0x8FFFFF
	M68K_Write_Byte_Bad,		// 0x900000 - 0x9FFFFF
	M68K_Write_Byte_Misc_32X,	// 0xA00000 - 0xAFFFFF
	M68K_Write_Byte_Bad,		// 0xB00000 - 0xBFFFFF
	M68K_Write_Byte_VDP,		// 0xC00000 - 0xCFFFFF
	M68K_Write_Byte_Bad,		// 0xD00000 - 0xDFFFFF
	M68K_Write_Byte_Ram,		// 0xE00000 - 0xEFFFFF
	M68K_Write_Byte_Ram,		// 0xF00000 - 0xFFFFFF
};

M68K_Write_Word_fn _32X_M68K_Write_Word_Table[0x10] =
{
	M68K_Write_Word_Bad,		// 0x000000 - 0x0FFFFF
	M68K_Write_Word_Bad,		// 0x100000 - 0x1FFFFF
	M68K_Write_Word_SRAM,		// 0x200000 - 0x2FFFFF
	M68K_Write_Word_Bad,		// 0x300000 - 0x3FFFFF
	M68K_Write_Word_Bad,		// 0x400000 - 0x4FFFFF
	M68K_Write_Word_Bad,		// 0x500000 - 0x5FFFFF
	M68K_Write_Word_Bad,		// 0x600000 - 0x6FFFFF
	M68K_Write_Word_32X_FB0,	// 0x700000 - 0x7FFFFF
	M68K_Write_Word_32X_FB1,	// 0x800000 - 0x8FFFFF
	M68K_Write_Word_Bad,		// 0x900000 - 0x9FFFFF
	M68K_Write_Word_Misc_32X,	// 0xA00000 - 0xAFFFFF
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


/******** 32X extended Read Byte ********/

static uint8_t M68K_Read_Byte_Bios_32X(uint32_t address)
{
	return _32X_Genesis_Rom[(address & 0x3FF) ^ 1];
}

static uint8_t M68K_Read_Byte_BiosR_32X(uint32_t address)
{
	// NOTE: This handler is also mapped at 0x000000 by M68K_32X_Mode().
	if (address < 0x400)
		return _32X_Genesis_Rom[address ^ 1];

	return Rom_Data.u8[address ^ 1];
}

/**
 * _32X_reg_read_byte(): Read a byte from the 32X adapter registers.
 * (0xA15100 - 0xA1517F)
 * @param reg Register offset. (address & 0x3F)
 */
static uint8_t _32X_reg_read_byte(unsigned int reg)
{
	switch (reg)
	{
		case 0x00:	// ACR_H
			return _32X_FM;

		case 0x01:	// ACR_L
			return (uint8_t)(_32X_ADEN | _32X_RES | 0x80);

		case 0x03:	// Int
			return 0;

		case 0x05:	// Bank
			return (uint8_t)Bank_SH2;

		case 0x07:	// DREQ_C
			return (uint8_t)(_32X_RV | get_byte_uint(_32X_DREQ_ST, 0) |
					 (get_byte_uint(_32X_DREQ_ST, 1) & 0x80));

		case 0x30:	// PWM_Cont_H
			return get_byte_uint(PWM_Mode, 1);

		case 0x31:	// PWM_Cont_L
			return get_byte_uint(PWM_Mode, 0);

		case 0x32:	// PWM_Cycle_H
			return get_byte_uint(PWM_Cycle_Tmp, 1);

		case 0x33:	// PWM_Cycle_L
			return get_byte_uint(PWM_Cycle_Tmp, 0);

		case 0x34:	// PWM_Pulse_L
		case 0x38:	// PWM_Pulse_L (sic; the asm's Pulse_C isn't used here)
			return _32X_PWM_Full(PWM_RP_L, PWM_WP_L);

		case 0x36:	// PWM_Pulse_R
			return _32X_PWM_Full(PWM_RP_R, PWM_WP_R);

		default:
			break;
	}

	if (reg >= 0x20 && reg < 0x30)
	{
		// Communication registers.
		return _32X_Comm[reg - 0x20];
	}

	// bad
	return 0;
}

/**
 * _32X_vdp_reg_read_byte(): Read a byte from the 32X VDP registers.
 * (0xA15180 - 0xA151FF)
 * @param reg Register offset. (address & 0xF)
 */
static uint8_t _32X_vdp_reg_read_byte(unsigned int reg)
{
	uint8_t state;

	switch (reg)
	{
		case 0x0:	// VDP_Mode_H
			return get_byte_uint(_32X_VDP.Mode, 1);
		case 0x1:	// VDP_Mode_L
			return get_byte_uint(_32X_VDP.Mode, 0);
		case 0x3:	// VDP_Shift
			return get_byte_uint(_32X_VDP.Mode, 2);
		case 0x5:	// VDP_AF_Len_L
			return get_byte_uint(_32X_VDP.AF_Len, 0);
		case 0x6:	// VDP_AF_St_H
			return get_byte_uint(_32X_VDP.AF_St, 1);
		case 0x7:	// VDP_AF_St_L
			return get_byte_uint(_32X_VDP.AF_St, 0);
		case 0x8:	// VDP_AF_Data_H
			return get_byte_uint(_32X_VDP.AF_Data, 1);
		case 0x9:	// VDP_AF_Data_L
			return get_byte_uint(_32X_VDP.AF_Data, 0);
		case 0xA:	// VDP_State_H
			return get_byte_uint(_32X_VDP.State, 1);

		case 0xB:	// VDP_State_L
			// Toggle the PEN bit on every read.
			state = (uint8_t)(get_byte_uint(_32X_VDP.State, 0) ^ 2);
			_32X_VDP.State = set_byte_uint(_32X_VDP.State, 0, state);
			return state;

		default:	// bad
			return 0;
	}
}

static uint8_t M68K_Read_Byte_Misc_32X(uint32_t address)
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
		    CYCLE_FOR_TAKE_Z80_BUS_GENESIS)
		{
			// Bus taken.
			return 0x80;
		}

		return (uint8_t)((uint8_t)Last_BUS_REQ_St | 0x80);
	}

	if (address >= 0xA15100)
	{
		// 32X registers.
		if (address < 0xA15180)
			return _32X_reg_read_byte(address & 0x3F);

		// 32X VDP registers.
		if (_32X_FM)
			return 0;
		if (address >= 0xA15200)
			return 0;
		return _32X_vdp_reg_read_byte(address & 0xF);
	}

	if (address >= 0xA130EC)
	{
		// 32X ID. ("MARS")
		return _32X_ID_Tab[address & 3];
	}

	if (address > 0xA1000F)
		return 0;

	// I/O ports.
	switch (address & 0x0E)
	{
		case 0x00:	// MD_Spec
			return md_version_register();
		case 0x02:	// Pad_1
			return RD_Controller_1();
		case 0x04:	// Pad_2
			return RD_Controller_2();
		case 0x06:	// Ser
			return 0xFF;
		case 0x08:	// CT_Pad_1
			return (uint8_t)Controller_1_COM;
		case 0x0A:	// CT_Pad_2
			return (uint8_t)Controller_2_COM;
		case 0x0C:	// CT_Ser
		default:	// bad
			return 0;
	}
}

uint8_t M68K_Read_Byte_32X_FB0(uint32_t address)
{
	return _32X_VDP_Ram.u8[(address & 0x1FFFF) ^ 1];
}

uint8_t M68K_Read_Byte_32X_FB1(uint32_t address)
{
	return _32X_VDP_Ram.u8[((address & 0x1FFFF) ^ 1) + 0x20000];
}


/******** 32X extended Read Word ********/

static uint16_t M68K_Read_Word_Bios_32X(uint32_t address)
{
	// NOTE: The asm masks with 0xFE, so only the first 256 bytes are used.
	return genesis_rom_word(address & 0xFE);
}

static uint16_t M68K_Read_Word_BiosR_32X(uint32_t address)
{
	// NOTE: This handler is also mapped at 0x000000 by M68K_32X_Mode().
	// The word path switches to the ROM at 0x100, not 0x400.
	if (address < 0x100)
		return genesis_rom_word(address & ~1U);

	return Rom_Data.u16[address >> 1];
}

/**
 * _32X_reg_read_word(): Read a word from the 32X adapter registers.
 * (0xA15100 - 0xA1517E)
 * @param reg Register offset. (address & 0x3E)
 */
static uint16_t _32X_reg_read_word(unsigned int reg)
{
	switch (reg)
	{
		case 0x00:	// ACR
			return (uint16_t)(((uint16_t)_32X_FM << 8) |
					  (uint8_t)(_32X_ADEN | _32X_RES) | 0x80);

		case 0x02:	// INT
			return 0;

		case 0x04:	// Bank
			return (uint8_t)Bank_SH2;

		case 0x06:	// DREQ_C
			return (uint8_t)(_32X_RV | get_byte_uint(_32X_DREQ_ST, 0) |
					 (get_byte_uint(_32X_DREQ_ST, 1) & 0x80));

		case 0x08:	// DREQ_Src_H
			return (uint16_t)(_32X_DREQ_SRC >> 16);
		case 0x0A:	// DREQ_Src_L
			return (uint16_t)_32X_DREQ_SRC;
		case 0x0C:	// DREQ_Dest_H
			return (uint16_t)(_32X_DREQ_DST >> 16);
		case 0x0E:	// DREQ_Dest_L
			return (uint16_t)_32X_DREQ_DST;
		case 0x10:	// DREQ_Len
			return (uint16_t)_32X_DREQ_LEN;

		case 0x12:	// FIFO
			// NOTE: The asm returns without setting eax here,
			// so the result was whatever eax held. Return 0.
			return 0;

		case 0x30:	// PWM_Cont
			return (uint16_t)PWM_Mode;
		case 0x32:	// PWM_Cycle
			return (uint16_t)PWM_Cycle_Tmp;

		// PWM pulse: low byte is the low byte of the write pointer,
		// high byte is the "full" table entry. (mov eax, [WP]; mov ah, [...])
		case 0x34:	// PWM_Pulse_L
		case 0x38:	// PWM_Pulse_C
			return (uint16_t)((_32X_PWM_Full(PWM_RP_L, PWM_WP_L) << 8) |
					  (PWM_WP_L & 0xFF));
		case 0x36:	// PWM_Pulse_R
			return (uint16_t)((_32X_PWM_Full(PWM_RP_R, PWM_WP_R) << 8) |
					  (PWM_WP_R & 0xFF));

		default:
			break;
	}

	if (reg >= 0x20 && reg < 0x30)
	{
		// Communication registers.
		return (uint16_t)((_32X_Comm[reg - 0x20] << 8) | _32X_Comm[reg - 0x20 + 1]);
	}

	// bad
	return 0;
}

/**
 * _32X_vdp_reg_read_word(): Read a word from the 32X VDP registers.
 * (0xA15180 - 0xA151FE)
 * @param reg Register offset. (address & 0xE)
 */
static uint16_t _32X_vdp_reg_read_word(unsigned int reg)
{
	uint16_t state;

	switch (reg)
	{
		case 0x0:	// VDP_Mode
			return (uint16_t)_32X_VDP.Mode;
		case 0x2:	// VDP_Shift
			return get_byte_uint(_32X_VDP.Mode, 2);
		case 0x4:	// VDP_AF_Len
			return get_byte_uint(_32X_VDP.AF_Len, 0);
		case 0x6:	// VDP_AF_St
			return (uint16_t)_32X_VDP.AF_St;
		case 0x8:	// VDP_AF_Data
			return (uint16_t)_32X_VDP.AF_Data;

		case 0xA:	// VDP_State
			// Toggle the PEN bit on every read.
			state = (uint16_t)(_32X_VDP.State ^ 2);
			_32X_VDP.State = set_low_word_uint(_32X_VDP.State, state);
			return state;

		default:	// bad
			return 0;
	}
}

static uint16_t M68K_Read_Word_Misc_32X(uint32_t address)
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
		    CYCLE_FOR_TAKE_Z80_BUS_GENESIS)
		{
			// Bus taken.
			al = fake_fetch_next();
			return (uint16_t)(0x8000 | al);
		}

		// asm: mov ah, [Last_BUS_REQ_St]; add ah, 0x80
		ah = (uint8_t)((uint8_t)Last_BUS_REQ_St + 0x80);
		al = fake_fetch_next();
		return (uint16_t)((ah << 8) | al);
	}

	if (address >= 0xA15100)
	{
		// 32X registers.
		if (address < 0xA15180)
			return _32X_reg_read_word(address & 0x3E);

		// 32X VDP registers.
		if (_32X_FM)
			return 0;

		if (address >= 0xA15200)
		{
			// 32X CRAM.
			if (address >= 0xA15400)
				return 0;
			return _32X_VDP_CRam[(address - 0xA15200) >> 1];
		}

		return _32X_vdp_reg_read_word(address & 0xE);
	}

	if (address >= 0xA130EC)
	{
		// 32X ID. ("MARS")
		// asm: movzx eax, word [_32X_ID_Tab + (address & 3)] (little-endian)
		const unsigned int i = (address & 3);
		return (uint16_t)(_32X_ID_Tab[i] | (_32X_ID_Tab[i + 1] << 8));
	}

	if (address > 0xA1000F)
		return 0;

	// I/O ports.
	switch (address & 0x0E)
	{
		case 0x00:	// MD_Spec
			return md_version_register();
		case 0x02:	// Pad_1
			return RD_Controller_1();
		case 0x04:	// Pad_2
			return RD_Controller_2();
		case 0x06:	// Ser
			return 0xFF00;
		case 0x08:	// CT_Pad_1 (asm: mov eax, [Controller_1_COM])
			return (uint16_t)Controller_1_COM;
		case 0x0A:	// CT_Pad_2 (asm: mov eax, [Controller_2_COM])
			return (uint16_t)Controller_2_COM;
		case 0x0C:	// CT_Ser
		default:	// bad
			return 0;
	}
}

uint16_t M68K_Read_Word_32X_FB0(uint32_t address)
{
	return _32X_VDP_Ram.u16[(address & 0x1FFFE) >> 1];
}

uint16_t M68K_Read_Word_32X_FB1(uint32_t address)
{
	return _32X_VDP_Ram.u16[((address & 0x1FFFE) + 0x20000) >> 1];
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
 * sram_ctrl_write(): SRAM control register. (0xA130F0 - 0xA130F1)
 * @param data Low byte of the written data.
 */
static void sram_ctrl_write(uint8_t data)
{
	// asm: setnz [SRAM_ON]; setz [SRAM_Write] (low byte only)
	SRAM_ON = set_low_byte_int(SRAM_ON, ((data & 1) ? 1 : 0));
	SRAM_Write = set_low_byte_int(SRAM_Write, ((data & 2) ? 0 : 1));
}

/**
 * acr_low_write(): Adapter control register, low byte. (0xA15101)
 * Bit 1: RES (SH2 reset), Bit 0: ADEN (adapter enable).
 */
static void acr_low_write(uint8_t data)
{
	const uint8_t old_res = _32X_RES;
	const uint8_t new_res = (uint8_t)(data & 2);
	const uint8_t aden = (uint8_t)(data & 1);

	_32X_RES = new_res;

	// asm: cmp ax, 2 (ah = old RES, al = new RES)
	if (old_res == 0 && new_res == 2)
	{
		SH2_Reset(&M_SH2, 1);
		SH2_Reset(&S_SH2, 1);
	}

	if (_32X_ADEN ^ aden)
	{
		_32X_ADEN = aden;
		M68K_32X_Mode();
	}
}

/**
 * int_ctrl_write(): Interrupt control register. (0xA15103)
 * Bit 0: INTM (master SH2 CMD interrupt), Bit 1: INTS (slave SH2).
 */
static void int_ctrl_write(uint8_t data)
{
	const uint8_t sint = (uint8_t)(data & 2);
	const uint8_t mint = (uint8_t)((data << 1) & 2);

	if (_32X_MINT & mint)
		SH2_Interrupt(&M_SH2, 8);
	if (_32X_SINT & sint)
		SH2_Interrupt(&S_SH2, 8);
}

/**
 * bank_write(): 32X ROM bank register. (0xA15105)
 */
static void bank_write(uint8_t data)
{
	// asm: and al, 3; mov [Bank_SH2], al (low byte only)
	Bank_SH2 = set_low_byte_int(Bank_SH2, (uint8_t)(data & 3));
	M68K_Set_32X_Rom_Bank();
}

/**
 * dreq_ctrl_write(): DREQ control register. (0xA15107)
 * Bit 2: 68S (68000 to SH2 DREQ), Bit 0: RV (ROM to VRAM DMA).
 */
static void dreq_ctrl_write(uint8_t data)
{
	const uint8_t rv = (uint8_t)(data & 1);
	const uint8_t dreq = (uint8_t)(data & 4);
	const uint8_t old_st = get_byte_uint(_32X_DREQ_ST, 0);

	if (_32X_RV ^ rv)
	{
		_32X_RV = rv;
		M68K_32X_Mode();
	}

	// asm: cmp bx, 0x0004 (bh = old DREQ_ST, bl = data & 4)
	if (old_st == 0 && dreq == 4)
	{
		_32X_DREQ_ST = set_byte_uint(_32X_DREQ_ST, 1, 0x40);
		// asm: byte stores. (only the low byte is cleared)
		_32X_FIFO_Block = set_byte_uint(_32X_FIFO_Block, 0, 0);
		_32X_FIFO_Read = set_byte_uint(_32X_FIFO_Read, 0, 0);
		_32X_FIFO_Write = set_byte_uint(_32X_FIFO_Write, 0, 0);
	}

	_32X_DREQ_ST = set_byte_uint(_32X_DREQ_ST, 0, dreq);
}

/**
 * pwm_fifo_push_l() / pwm_fifo_push_r(): Push a value onto a PWM FIFO,
 * unless it's full.
 */
static void pwm_fifo_push_l(uint16_t data)
{
	const unsigned int wp = PWM_WP_L & (PWM_BUF_SIZE - 1);
	if (_32X_PWM_Full(PWM_RP_L, wp) & 0x80)
		return;
	PWM_FIFO_L[wp] = data;
	PWM_WP_L = (wp + 1) & (PWM_BUF_SIZE - 1);
}

static void pwm_fifo_push_r(uint16_t data)
{
	const unsigned int wp = PWM_WP_R & (PWM_BUF_SIZE - 1);
	if (_32X_PWM_Full(PWM_RP_R, wp) & 0x80)
		return;
	PWM_FIFO_R[wp] = data;
	PWM_WP_R = (wp + 1) & (PWM_BUF_SIZE - 1);
}

/**
 * pwm_fifo_push_mono(): Push a value onto both PWM FIFOs.
 * Uses the left channel's pointers for both. (as in the asm)
 */
static void pwm_fifo_push_mono(uint16_t data)
{
	const unsigned int wp = PWM_WP_L & (PWM_BUF_SIZE - 1);
	if (_32X_PWM_Full(PWM_RP_L, wp) & 0x80)
		return;
	PWM_FIFO_L[wp] = data;
	PWM_FIFO_R[wp] = data;
	PWM_WP_L = (wp + 1) & (PWM_BUF_SIZE - 1);
	PWM_WP_R = PWM_WP_L;
}


/******** 32X extended Write Byte ********/

/**
 * _32X_reg_write_byte(): Write a byte to the 32X adapter registers.
 * (0xA15100 - 0xA1517F)
 * @param reg Register offset. (address & 0x3F)
 */
static void _32X_reg_write_byte(unsigned int reg, uint8_t data)
{
	switch (reg)
	{
		case 0x00:	// ACR_H
			_32X_Write_FM(data);
			return;

		case 0x01:	// ACR_L
			acr_low_write(data);
			return;

		case 0x03:	// Int
			int_ctrl_write(data);
			return;

		case 0x05:	// Bank
			bank_write(data);
			return;

		case 0x07:	// DREQ_C
			dreq_ctrl_write(data);
			return;

		case 0x31:	// PWM_Cont_L
			PWM_Mode = set_byte_uint(PWM_Mode, 0, data);
			return;

		case 0x32:	// PWM_Cycle_H
			PWM_Cycle_Tmp = set_byte_uint(PWM_Cycle_Tmp, 1, data);
			PWM_Set_Cycle(((unsigned int)data << 8) | get_byte_uint(PWM_Cycle_Tmp, 0));
			return;

		case 0x33:	// PWM_Cycle_L
			PWM_Cycle_Tmp = set_byte_uint(PWM_Cycle_Tmp, 0, data);
			PWM_Set_Cycle(((unsigned int)get_byte_uint(PWM_Cycle_Tmp, 1) << 8) | data);
			return;

		case 0x34:	// PWM_Pulse_L_H
		case 0x38:	// PWM_Pulse_C_H (uses the left temp register)
			PWM_FIFO_L_Tmp = set_byte_uint(PWM_FIFO_L_Tmp, 1, data);
			return;

		case 0x35:	// PWM_Pulse_L_L
			pwm_fifo_push_l((uint16_t)((get_byte_uint(PWM_FIFO_L_Tmp, 1) << 8) | data));
			return;

		case 0x36:	// PWM_Pulse_R_H
			PWM_FIFO_R_Tmp = set_byte_uint(PWM_FIFO_R_Tmp, 1, data);
			return;

		case 0x37:	// PWM_Pulse_R_L
			pwm_fifo_push_r((uint16_t)((get_byte_uint(PWM_FIFO_R_Tmp, 1) << 8) | data));
			return;

		case 0x39:	// PWM_Pulse_C_L
			pwm_fifo_push_mono((uint16_t)((get_byte_uint(PWM_FIFO_L_Tmp, 1) << 8) | data));
			return;

		default:
			break;
	}

	if (reg >= 0x20 && reg < 0x30)
	{
		// Communication registers.
		_32X_Comm[reg - 0x20] = data;
	}

	// bad
}

/**
 * _32X_vdp_reg_write_byte(): Write a byte to the 32X VDP registers.
 * (0xA15180 - 0xA151FF)
 * @param reg Register offset. (address & 0xF)
 */
static void _32X_vdp_reg_write_byte(unsigned int reg, uint8_t data)
{
	switch (reg)
	{
		case 0x1:	// VDP_Mode
			_32X_VDP.Mode = set_byte_uint(_32X_VDP.Mode, 0, data);
			break;
		case 0x3:	// VDP_Shift
			_32X_VDP.Mode = set_byte_uint(_32X_VDP.Mode, 2, data);
			break;
		case 0x5:	// VDP_AF_Len
			_32X_VDP.AF_Len = set_byte_uint(_32X_VDP.AF_Len, 0, data);
			break;
		case 0xB:	// VDP_State
			_32X_Write_FB_Control(data);
			break;
		default:	// bad
			break;
	}
}

static void M68K_Write_Byte_Misc_32X(uint32_t address, uint8_t data)
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

	if (address >= 0xA15100)
	{
		// 32X registers.
		if (address < 0xA15180)
		{
			_32X_reg_write_byte(address & 0x3F, data);
			return;
		}

		// 32X VDP registers. (Byte writes to CRAM are ignored.)
		if (_32X_FM)
			return;
		if (address >= 0xA15200)
			return;
		_32X_vdp_reg_write_byte(address & 0xF, data);
		return;
	}

	if (address >= 0xA130F0)
	{
		// Genesis bank registers.
		if (address < 0xA130F2)
		{
			// SRAM control.
			sram_ctrl_write(data);
			return;
		}

		if (address > 0xA130FF)
			return;

		// SSF2 bankswitching.
		{
			const unsigned int idx = (address & 0xF) >> 1;
			const unsigned int bank = (data & 0x1F);
			M68K_Read_Byte_Table[idx] = Genesis_M68K_Read_Byte_Table[bank];
			M68K_Read_Word_Table[idx] = Genesis_M68K_Read_Word_Table[bank];
		}
		return;
	}

	if (address > 0xA1000F)
		return;

	// I/O ports.
	switch (address & 0x0E)
	{
		case 0x02:	// Pad_1
			WR_Controller_1(data);
			break;
		case 0x04:	// Pad_2
			WR_Controller_2(data);
			break;
		case 0x08:	// CT_Pad_1 (byte store)
			Controller_1_COM = (Controller_1_COM & 0xFFFFFF00U) | data;
			break;
		case 0x0A:	// CT_Pad_2 (byte store)
			Controller_2_COM = (Controller_2_COM & 0xFFFFFF00U) | data;
			break;
		default:	// bad
			break;
	}
}

void M68K_Write_Byte_32X_FB0(uint32_t address, uint8_t data)
{
	// Zero bytes are not written. (overwrite mode)
	if (data == 0)
		return;
	_32X_VDP_Ram.u8[(address & 0x1FFFF) ^ 1] = data;
}

void M68K_Write_Byte_32X_FB1(uint32_t address, uint8_t data)
{
	// Zero bytes are not written. (overwrite mode)
	if (data == 0)
		return;
	_32X_VDP_Ram.u8[((address & 0x1FFFF) ^ 1) + 0x20000] = data;
}


/******** 32X extended Write Word ********/

/**
 * fifo_write(): Write a word to the 68000 -> SH2 FIFO. (0xA15112)
 * In the asm, _32X_FIFO_A and _32X_FIFO_B are adjacent, and the data is
 * stored at byte offset (_32X_FIFO_Block + _32X_FIFO_Write * 2) from
 * _32X_FIFO_A. (_32X_FIFO_Block is 0 or 8, selecting FIFO A or B.)
 */
static void fifo_write(uint16_t data)
{
	const unsigned int block = _32X_FIFO_Block;
	unsigned int write = _32X_FIFO_Write;
	unsigned int off;

	if ((_32X_DREQ_ST & 0x8004) != 0x0004)
		return;

	off = block + (write * 2);
	if (off < 8)
		_32X_FIFO_A[off >> 1] = data;
	else if (off < 16)
		_32X_FIFO_B[(off - 8) >> 1] = data;
	// (else: the asm would write past _32X_FIFO_B.)

	write++;
	if (write < 4)
	{
		_32X_FIFO_Write = write;
		return;
	}

	// FIFO block is full.
	if (get_byte_uint(_32X_DREQ_ST, 1) & 0x40)
	{
		// Switch to the other block.
		_32X_DREQ_ST = set_byte_uint(_32X_DREQ_ST, 1, 0);
		_32X_FIFO_Write = set_byte_uint(_32X_FIFO_Write, 0, 0);
		_32X_FIFO_Read = set_byte_uint(_32X_FIFO_Read, 0, 0);
		_32X_FIFO_Block = block ^ (4 * 2);
	}
	else
	{
		// Both blocks are full.
		// NOTE: _32X_FIFO_Write is not updated here. (as in the asm)
		_32X_DREQ_ST = set_byte_uint(_32X_DREQ_ST, 1, 0x80);
	}

	SH2_DMA0_Request(&M_SH2, 1);
}

/**
 * _32X_reg_write_word(): Write a word to the 32X adapter registers.
 * (0xA15100 - 0xA1517E)
 * @param reg Register offset. (address & 0x3E)
 */
static void _32X_reg_write_word(unsigned int reg, uint16_t data)
{
	const uint8_t lo = (uint8_t)(data & 0xFF);
	const uint8_t hi = (uint8_t)(data >> 8);

	switch (reg)
	{
		case 0x00:	// ACR
			_32X_Write_FM(hi);
			acr_low_write(lo);
			return;

		case 0x02:	// INT
			int_ctrl_write(lo);
			return;

		case 0x04:	// Bank
			bank_write(lo);
			return;

		case 0x06:	// DREQ_C
			dreq_ctrl_write(lo);
			return;

		case 0x08:	// DREQ_Src_H (byte store)
			_32X_DREQ_SRC = set_byte_uint(_32X_DREQ_SRC, 2, lo);
			return;
		case 0x0A:	// DREQ_Src_L
			_32X_DREQ_SRC = set_low_word_uint(_32X_DREQ_SRC, (uint16_t)(data & ~1));
			return;
		case 0x0C:	// DREQ_Dest_H (byte store)
			_32X_DREQ_DST = set_byte_uint(_32X_DREQ_DST, 2, lo);
			return;
		case 0x0E:	// DREQ_Dest_L
			_32X_DREQ_DST = set_low_word_uint(_32X_DREQ_DST, data);
			return;
		case 0x10:	// DREQ_Len
			_32X_DREQ_LEN = set_low_word_uint(_32X_DREQ_LEN, (uint16_t)(data & ~3));
			return;

		case 0x12:	// FIFO
			fifo_write(data);
			return;

		case 0x30:	// PWM_Cont (byte store)
			PWM_Mode = set_byte_uint(PWM_Mode, 0, (uint8_t)(lo & 0x0F));
			return;
		case 0x32:	// PWM_Cycle
			PWM_Set_Cycle(data);
			return;
		case 0x34:	// PWM_Pulse_L
			pwm_fifo_push_l(data);
			return;
		case 0x36:	// PWM_Pulse_R
			pwm_fifo_push_r(data);
			return;
		case 0x38:	// PWM_Pulse_C
			pwm_fifo_push_mono(data);
			return;

		default:
			break;
	}

	if (reg >= 0x20 && reg < 0x30)
	{
		// Communication registers.
		_32X_Comm[reg - 0x20 + 0] = hi;
		_32X_Comm[reg - 0x20 + 1] = lo;
	}

	// bad
}

/**
 * vdp_autofill(): 32X VDP auto fill. (write to 0xA15188)
 * Ported literally from the asm, including its fill length.
 */
static void vdp_autofill(uint16_t data)
{
	// Fill the frame buffer that isn't being displayed.
	const uint32_t fb_base = ((_32X_VDP.State & 1) ^ 1) << 17;
	uint32_t st = _32X_VDP.AF_St;	// asm: full dword; only bl is incremented.
	uint32_t count;
	unsigned int carry;

	_32X_VDP.AF_Data = set_low_word_uint(_32X_VDP.AF_Data, data);

	// asm: ecx = AF_Len + 1; shr ecx, 1
	count = _32X_VDP.AF_Len + 1;
	carry = (count & 1);
	count >>= 1;

	if (count == 0)
	{
		// Spec_Fill: a single word.
		_32X_VDP_Ram_Write_Word(fb_base + (st * 2), data);
		st = (st & ~0xFFU) | ((st + 1) & 0xFF);
		_32X_VDP.AF_St = st;
		return;
	}

	if (carry)
	{
		_32X_VDP_Ram_Write_Word(fb_base + (st * 2), data);
		st = (st & ~0xFFU) | ((st + 1) & 0xFF);
	}

	// asm: mov [edi + ebx*2], eax (two words); add bl, 2; dec ecx; jns
	for (;;)
	{
		_32X_VDP_Ram_Write_Word(fb_base + (st * 2), data);
		_32X_VDP_Ram_Write_Word(fb_base + (st * 2) + 2, data);
		st = (st & ~0xFFU) | ((st + 2) & 0xFF);
		count--;
		if ((int32_t)count < 0)
			break;
	}

	_32X_VDP.AF_St = st;
}

/**
 * _32X_vdp_reg_write_word(): Write a word to the 32X VDP registers.
 * (0xA15180 - 0xA151FE)
 * @param reg Register offset. (address & 0xE)
 */
static void _32X_vdp_reg_write_word(unsigned int reg, uint16_t data)
{
	const uint8_t lo = (uint8_t)(data & 0xFF);

	switch (reg)
	{
		case 0x0:	// VDP_Mode (byte store)
			_32X_VDP.Mode = set_byte_uint(_32X_VDP.Mode, 0, lo);
			break;
		case 0x2:	// VDP_Shift (byte store)
			_32X_VDP.Mode = set_byte_uint(_32X_VDP.Mode, 2, lo);
			break;
		case 0x4:	// VDP_AF_Len (byte store)
			_32X_VDP.AF_Len = set_byte_uint(_32X_VDP.AF_Len, 0, lo);
			break;
		case 0x6:	// VDP_AF_St
			_32X_VDP.AF_St = set_low_word_uint(_32X_VDP.AF_St, data);
			break;
		case 0x8:	// VDP_AF_Data
			vdp_autofill(data);
			break;
		case 0xA:	// VDP_State
			_32X_Write_FB_Control(lo);
			break;
		default:	// bad
			break;
	}
}

static void M68K_Write_Word_Misc_32X(uint32_t address, uint16_t data)
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

	if (address >= 0xA15100)
	{
		// 32X registers.
		if (address < 0xA15180)
		{
			_32X_reg_write_word(address & 0x3E, data);
			return;
		}

		// 32X VDP registers.
		if (_32X_FM)
			return;

		if (address >= 0xA15200)
		{
			// 32X CRAM.
			if (address >= 0xA15400)
				return;
			_32X_Write_CRam((address - 0xA15200) >> 1, data);
			return;
		}

		_32X_vdp_reg_write_word(address & 0xE, data);
		return;
	}

	if (address >= 0xA130F0)
	{
		// Genesis bank registers.
		if (address < 0xA130F2)
		{
			// SRAM control.
			sram_ctrl_write((uint8_t)(data & 0xFF));
			return;
		}

		// SSF2 bankswitching is disabled for word writes in the asm.
		return;
	}

	if (address > 0xA1000F)
		return;

	// I/O ports.
	switch (address & 0x0E)
	{
		case 0x02:	// Pad_1
			WR_Controller_1((uint8_t)(data & 0xFF));
			break;
		case 0x04:	// Pad_2
			WR_Controller_2((uint8_t)(data & 0xFF));
			break;
		case 0x08:	// CT_Pad_1 (word store)
			Controller_1_COM = (Controller_1_COM & 0xFFFF0000U) | data;
			break;
		case 0x0A:	// CT_Pad_2 (word store)
			Controller_2_COM = (Controller_2_COM & 0xFFFF0000U) | data;
			break;
		default:	// bad
			break;
	}
}

/**
 * fb_overwrite_word(): Frame buffer overwrite: only non-zero bytes are written.
 * asm: [VDP_Ram + off + 0] = al (if nonzero); [VDP_Ram + off + 1] = ah (if nonzero)
 * On a little-endian host, byte +0 is the low byte of the host word.
 */
static inline void fb_overwrite_word(uint32_t byte_offset, uint16_t data)
{
	uint16_t w = _32X_VDP_Ram.u16[byte_offset >> 1];
	if (data & 0x00FF)
		w = (uint16_t)((w & 0xFF00) | (data & 0x00FF));
	if (data & 0xFF00)
		w = (uint16_t)((w & 0x00FF) | (data & 0xFF00));
	_32X_VDP_Ram.u16[byte_offset >> 1] = w;
}

void M68K_Write_Word_32X_FB0(uint32_t address, uint16_t data)
{
	const uint32_t a = (address & 0x3FFFE);

	if (!(a & 0x20000))
	{
		_32X_VDP_Ram.u16[a >> 1] = data;
		return;
	}

	// Overwrite image.
	fb_overwrite_word(a - 0x20000, data);
}

void M68K_Write_Word_32X_FB1(uint32_t address, uint16_t data)
{
	const uint32_t a = (address & 0x3FFFE);

	if (!(a & 0x20000))
	{
		_32X_VDP_Ram.u16[(a + 0x20000) >> 1] = data;
		return;
	}

	// Overwrite image.
	fb_overwrite_word(a, data);
}
