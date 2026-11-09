/***************************************************************************
 * Gens: Main 68000 memory management.                                     *
 *                                                                         *
 * Copyright (c) 1999-2002 by Stéphane Dallongeville                       *
 * Copyright (c) 2003-2004 by Stéphane Akhoun                              *
 * Copyright (c) 2008-2009 by David Korth                                  *
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
 * C port of mem_m68k.asm.
 *
 * Memory layout notes:
 * - Rom_Data and Ram_68k store 68000 memory as host-endian 16-bit words.
 *   A 68000 byte at address A is at u8[A ^ 1]; a 68000 word at (even) A
 *   is u16[A >> 1].
 * - SRAM is NOT byte-swapped. (68000 byte A is at SRAM[A - SRAM_Start].)
 */

#include <stdint.h>

#include "mem_m68k.h"
#include "mem_m68k_handlers.h"
#include "mem_m68k_cd.h"
#include "mem_m68k_32x.h"

#include "libgsft/gsft_fastcall.h"

// CPU cores.
#include "gens_core/cpu/68k/star_68k.h"
#include "mdZ80/mdZ80.h"

// I/O, VDP, sound.
#include "gens_core/io/io.h"
#include "gens_core/vdp/vdp_io.h"
#include "gens_core/sound/ym2612.hpp"
#include "gens_core/sound/psg.h"

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

__typeof__(Ram_68k) Ram_68k __attribute__((aligned(64)));
__typeof__(Rom_Data) Rom_Data __attribute__((aligned(64)));

// Genesis firmware.
unsigned char Genesis_Rom[2 * 1024];

unsigned int Rom_Size;

// Save RAM.
unsigned char SRAM[64 * 1024];
int SRAM_Start;
int SRAM_End;
int SRAM_ON;
int SRAM_Write;
int SRAM_Custom;
int SRAM_Enabled;

int Z80_M68K_Cycle_Tab[512] __attribute__((aligned(64)));

// Z80 state.
unsigned int Z80_State;
int Last_BUS_REQ_Cnt;
int Last_BUS_REQ_St;
int Bank_M68K;
int Fake_Fetch;

int Game_Mode;
int CPU_Mode;
int Gen_Mode;	// NOTE: This is unused! (except in GSXv7)

int CPL_M68K;
int CPL_Z80;
int Cycles_M68K;
int Cycles_Z80;


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


/** Forward declarations for non-exported handlers. **/

static uint8_t M68K_Read_Byte_Rom8(uint32_t address);
static uint8_t M68K_Read_Byte_Rom9(uint32_t address);
static uint8_t M68K_Read_Byte_RomA(uint32_t address);
static uint8_t M68K_Read_Byte_RomB(uint32_t address);
static uint8_t M68K_Read_Byte_RomC(uint32_t address);
static uint8_t M68K_Read_Byte_RomD(uint32_t address);
static uint8_t M68K_Read_Byte_RomE(uint32_t address);
static uint8_t M68K_Read_Byte_RomF(uint32_t address);
static uint8_t M68K_Read_Byte_Misc(uint32_t address);

static uint16_t M68K_Read_Word_Rom8(uint32_t address);
static uint16_t M68K_Read_Word_Rom9(uint32_t address);
static uint16_t M68K_Read_Word_RomA(uint32_t address);
static uint16_t M68K_Read_Word_RomB(uint32_t address);
static uint16_t M68K_Read_Word_RomC(uint32_t address);
static uint16_t M68K_Read_Word_RomD(uint32_t address);
static uint16_t M68K_Read_Word_RomE(uint32_t address);
static uint16_t M68K_Read_Word_RomF(uint32_t address);
static uint16_t M68K_Read_Word_Misc(uint32_t address);

static void M68K_Write_Byte_Misc(uint32_t address, uint8_t data);
static void M68K_Write_Word_Misc(uint32_t address, uint16_t data);


/** Current Main 68000 Jump Tables. **/

#define REP4(x)		x, x, x, x
#define REP8(x)		REP4(x), REP4(x)
#define REP16(x)	REP8(x), REP8(x)
#define REP32(x)	REP16(x), REP16(x)

M68K_Read_Byte_fn M68K_Read_Byte_Table[0x20] = { REP32(M68K_Read_Byte_Bad) };
M68K_Read_Word_fn M68K_Read_Word_Table[0x20] = { REP32(M68K_Read_Word_Bad) };
M68K_Write_Byte_fn M68K_Write_Byte_Table[0x10] = { REP16(M68K_Write_Byte_Bad) };
M68K_Write_Word_fn M68K_Write_Word_Table[0x10] = { REP16(M68K_Write_Word_Bad) };


/** Genesis Main 68000 Jump Tables. **/

const M68K_Read_Byte_fn Genesis_M68K_Read_Byte_Table[0x20] =
{
	M68K_Read_Byte_Rom0,		// 0x000000 - 0x07FFFF
	M68K_Read_Byte_Rom1,		// 0x080000 - 0x0FFFFF
	M68K_Read_Byte_Rom2,		// 0x100000 - 0x17FFFF
	M68K_Read_Byte_Rom3,		// 0x180000 - 0x1FFFFF
	M68K_Read_Byte_Rom4,		// 0x200000 - 0x27FFFF
	M68K_Read_Byte_Rom5,		// 0x280000 - 0x2FFFFF
	M68K_Read_Byte_Rom6,		// 0x300000 - 0x37FFFF
	M68K_Read_Byte_Rom7,		// 0x380000 - 0x3FFFFF
	M68K_Read_Byte_Rom8,		// 0x400000 - 0x47FFFF
	M68K_Read_Byte_Rom9,		// 0x480000 - 0x4FFFFF
	M68K_Read_Byte_RomA,		// 0x500000 - 0x57FFFF
	M68K_Read_Byte_RomB,		// 0x580000 - 0x5FFFFF
	M68K_Read_Byte_RomC,		// 0x600000 - 0x67FFFF
	M68K_Read_Byte_RomD,		// 0x680000 - 0x6FFFFF
	M68K_Read_Byte_RomE,		// 0x700000 - 0x77FFFF
	M68K_Read_Byte_RomF,		// 0x780000 - 0x7FFFFF
	M68K_Read_Byte_Bad,		// 0x800000 - 0x87FFFF
	M68K_Read_Byte_Bad,		// 0x880000 - 0x8FFFFF
	M68K_Read_Byte_Bad,		// 0x900000 - 0x97FFFF
	M68K_Read_Byte_Bad,		// 0x980000 - 0x9FFFFF
	M68K_Read_Byte_Misc,		// 0xA00000 - 0xA7FFFF
	M68K_Read_Byte_Bad,		// 0xA80000 - 0xAFFFFF
	M68K_Read_Byte_Bad,		// 0xB00000 - 0xB7FFFF
	M68K_Read_Byte_Bad,		// 0xB80000 - 0xBFFFFF
	M68K_Read_Byte_VDP,		// 0xC00000 - 0xC7FFFF
	M68K_Read_Byte_VDP,		// 0xC80000 - 0xCFFFFF
	M68K_Read_Byte_VDP,		// 0xD00000 - 0xD7FFFF
	M68K_Read_Byte_VDP,		// 0xD80000 - 0xDFFFFF
	M68K_Read_Byte_Ram,		// 0xE00000 - 0xE7FFFF
	M68K_Read_Byte_Ram,		// 0xE80000 - 0xEFFFFF
	M68K_Read_Byte_Ram,		// 0xF00000 - 0xF7FFFF
	M68K_Read_Byte_Ram,		// 0xF80000 - 0xFFFFFF
};

const M68K_Read_Word_fn Genesis_M68K_Read_Word_Table[0x20] =
{
	M68K_Read_Word_Rom0,		// 0x000000 - 0x07FFFF
	M68K_Read_Word_Rom1,		// 0x080000 - 0x0FFFFF
	M68K_Read_Word_Rom2,		// 0x100000 - 0x17FFFF
	M68K_Read_Word_Rom3,		// 0x180000 - 0x1FFFFF
	M68K_Read_Word_Rom4,		// 0x200000 - 0x27FFFF
	M68K_Read_Word_Rom5,		// 0x280000 - 0x2FFFFF
	M68K_Read_Word_Rom6,		// 0x300000 - 0x37FFFF
	M68K_Read_Word_Rom7,		// 0x380000 - 0x3FFFFF
	M68K_Read_Word_Rom8,		// 0x400000 - 0x47FFFF
	M68K_Read_Word_Rom9,		// 0x480000 - 0x4FFFFF
	M68K_Read_Word_RomA,		// 0x500000 - 0x57FFFF
	M68K_Read_Word_RomB,		// 0x580000 - 0x5FFFFF
	M68K_Read_Word_RomC,		// 0x600000 - 0x67FFFF
	M68K_Read_Word_RomD,		// 0x680000 - 0x6FFFFF
	M68K_Read_Word_RomE,		// 0x700000 - 0x77FFFF
	M68K_Read_Word_RomF,		// 0x780000 - 0x7FFFFF
	M68K_Read_Word_Bad,		// 0x800000 - 0x87FFFF
	M68K_Read_Word_Bad,		// 0x880000 - 0x8FFFFF
	M68K_Read_Word_Bad,		// 0x900000 - 0x97FFFF
	M68K_Read_Word_Bad,		// 0x980000 - 0x9FFFFF
	M68K_Read_Word_Misc,		// 0xA00000 - 0xA7FFFF
	M68K_Read_Word_Bad,		// 0xA80000 - 0xAFFFFF
	M68K_Read_Word_Bad,		// 0xB00000 - 0xB7FFFF
	M68K_Read_Word_Bad,		// 0xB80000 - 0xBFFFFF
	M68K_Read_Word_VDP,		// 0xC00000 - 0xC7FFFF
	M68K_Read_Word_VDP,		// 0xC80000 - 0xCFFFFF
	M68K_Read_Word_VDP,		// 0xD00000 - 0xD7FFFF
	M68K_Read_Word_VDP,		// 0xD80000 - 0xDFFFFF
	M68K_Read_Word_Ram,		// 0xE00000 - 0xE7FFFF
	M68K_Read_Word_Ram,		// 0xE80000 - 0xEFFFFF
	M68K_Read_Word_Ram,		// 0xF00000 - 0xF7FFFF
	M68K_Read_Word_Ram,		// 0xF80000 - 0xFFFFFF
};

const M68K_Write_Byte_fn Genesis_M68K_Write_Byte_Table[0x10] =
{
	M68K_Write_Byte_SRAM,		// 0x000000 - 0x0FFFFF
	M68K_Write_Byte_SRAM,		// 0x100000 - 0x1FFFFF
	M68K_Write_Byte_SRAM,		// 0x200000 - 0x2FFFFF
	M68K_Write_Byte_SRAM,		// 0x300000 - 0x3FFFFF
	M68K_Write_Byte_SRAM,		// 0x400000 - 0x4FFFFF
	M68K_Write_Byte_SRAM,		// 0x500000 - 0x5FFFFF
	M68K_Write_Byte_SRAM,		// 0x600000 - 0x6FFFFF
	M68K_Write_Byte_SRAM,		// 0x700000 - 0x7FFFFF
	M68K_Write_Byte_SRAM,		// 0x800000 - 0x8FFFFF
	M68K_Write_Byte_SRAM,		// 0x900000 - 0x9FFFFF
	M68K_Write_Byte_Misc,		// 0xA00000 - 0xAFFFFF
	M68K_Write_Byte_Bad,		// 0xB00000 - 0xBFFFFF
	M68K_Write_Byte_VDP,		// 0xC00000 - 0xCFFFFF
	M68K_Write_Byte_VDP,		// 0xD00000 - 0xDFFFFF
	M68K_Write_Byte_Ram,		// 0xE00000 - 0xEFFFFF
	M68K_Write_Byte_Ram,		// 0xF00000 - 0xFFFFFF
};

const M68K_Write_Word_fn Genesis_M68K_Write_Word_Table[0x10] =
{
	M68K_Write_Word_SRAM,		// 0x000000 - 0x0FFFFF
	M68K_Write_Word_SRAM,		// 0x100000 - 0x1FFFFF
	M68K_Write_Word_SRAM,		// 0x200000 - 0x2FFFFF
	M68K_Write_Word_SRAM,		// 0x300000 - 0x3FFFFF
	M68K_Write_Word_SRAM,		// 0x400000 - 0x4FFFFF
	M68K_Write_Word_SRAM,		// 0x500000 - 0x5FFFFF
	M68K_Write_Word_SRAM,		// 0x600000 - 0x6FFFFF
	M68K_Write_Word_SRAM,		// 0x700000 - 0x7FFFFF
	M68K_Write_Word_SRAM,		// 0x800000 - 0x8FFFFF
	M68K_Write_Word_SRAM,		// 0x900000 - 0x9FFFFF
	M68K_Write_Word_Misc,		// 0xA00000 - 0xAFFFFF
	M68K_Write_Word_Bad,		// 0xB00000 - 0xBFFFFF
	M68K_Write_Word_VDP,		// 0xC00000 - 0xCFFFFF
	M68K_Write_Word_VDP,		// 0xD00000 - 0xDFFFFF
	M68K_Write_Word_Ram,		// 0xE00000 - 0xEFFFFF
	M68K_Write_Word_Ram,		// 0xF00000 - 0xFFFFFF
};


/**
 * Init_Memory_M68K(): Initialize the Main 68000 memory map.
 * @param System_ID 0 == Genesis; 1 == 32X; >1 == SegaCD.
 * (The asm compares only the low byte of System_ID, unsigned.)
 */
void Init_Memory_M68K(int System_ID)
{
	const uint8_t sys = (uint8_t)System_ID;
	int i;

	if (sys > 1)
	{
		// SegaCD.
		for (i = 0; i < 0x20; i++)
		{
			M68K_Read_Byte_Table[i] = SegaCD_M68K_Read_Byte_Table[i];
			M68K_Read_Word_Table[i] = SegaCD_M68K_Read_Word_Table[i];
		}
		for (i = 0; i < 0x10; i++)
		{
			M68K_Write_Byte_Table[i] = SegaCD_M68K_Write_Byte_Table[i];
			M68K_Write_Word_Table[i] = SegaCD_M68K_Write_Word_Table[i];
		}
	}
	else if (sys == 1)
	{
		// 32X.
		for (i = 0; i < 0x20; i++)
		{
			M68K_Read_Byte_Table[i] = _32X_M68K_Read_Byte_Table[i];
			M68K_Read_Word_Table[i] = _32X_M68K_Read_Word_Table[i];
		}
		for (i = 0; i < 0x10; i++)
		{
			M68K_Write_Byte_Table[i] = _32X_M68K_Write_Byte_Table[i];
			M68K_Write_Word_Table[i] = _32X_M68K_Write_Word_Table[i];
		}

		// Special cases.
		// asm: [M68K_Read_Byte_Table + 8*8 - 4] = [_32X_M68K_Read_Byte_Table + 6*8]
		M68K_Read_Byte_Table[15] = _32X_M68K_Read_Byte_Table[12];
		M68K_Read_Word_Table[15] = _32X_M68K_Read_Word_Table[12];
		// asm: [M68K_Write_Byte_Table + 8*4 + 4] = [_32X_M68K_Write_Byte_Table + 6*4]
		M68K_Write_Byte_Table[9] = _32X_M68K_Write_Byte_Table[6];
		M68K_Write_Word_Table[9] = _32X_M68K_Write_Word_Table[6];
	}
	else
	{
		// Genesis.
		for (i = 0; i < 0x20; i++)
		{
			M68K_Read_Byte_Table[i] = Genesis_M68K_Read_Byte_Table[i];
			M68K_Read_Word_Table[i] = Genesis_M68K_Read_Word_Table[i];
		}
		for (i = 0; i < 0x10; i++)
		{
			M68K_Write_Byte_Table[i] = Genesis_M68K_Write_Byte_Table[i];
			M68K_Write_Word_Table[i] = Genesis_M68K_Write_Word_Table[i];
		}
	}
}


/** Dispatchers. **/

unsigned char M68K_RB(unsigned int Adr)
{
	return M68K_Read_Byte_Table[(Adr >> 19) & 0x1F](Adr & 0xFFFFFF);
}

unsigned short M68K_RW(unsigned int Adr)
{
	return M68K_Read_Word_Table[(Adr >> 19) & 0x1F](Adr & 0xFFFFFF);
}

void M68K_WB(unsigned int Adr, unsigned char Data)
{
	M68K_Write_Byte_Table[(Adr >> 20) & 0x0F](Adr & 0xFFFFFF, Data);
}

void M68K_WW(unsigned int Adr, unsigned short Data)
{
	M68K_Write_Word_Table[(Adr >> 20) & 0x0F](Adr & 0xFFFFFF, Data);
}


/** Shared helpers. **/

/**
 * md_version_register(): Genesis version register.
 * Format: [MODE VMOD DISK RSV VER3 VER2 VER1 VER0]
 * MODE: Region. (0 == East; 1 == West)
 * VMOD: Video mode. (0 == NTSC; 1 == PAL)
 * DISK: Floppy disk drive. (0 == connected; 1 == not connected.)
 * RSV: Reserved. (I think this is used for SegaCD.)
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
 * sram_read_window(): Check if an address is in the active SRAM window.
 * (Shared condition for M68K_Read_*_Rom4.)
 * Comparisons are unsigned, as in the asm. (jb / ja)
 */
static inline int sram_read_window(uint32_t address)
{
	return ((SRAM_ON & 1) &&
		address >= (uint32_t)SRAM_Start &&
		address <= (uint32_t)SRAM_End &&
		(SRAM_Enabled & 1));
}


/******** Read Byte Proc ********/

uint8_t M68K_Read_Byte_Bad(uint32_t address)
{
	(void)address;
	return 0;
}

#define M68K_READ_BYTE_ROM(n, base) \
uint8_t M68K_Read_Byte_Rom##n(uint32_t address) \
{ \
	return Rom_Data.u8[((address & 0x7FFFF) | (base)) ^ 1]; \
}

M68K_READ_BYTE_ROM(0, 0x000000)
M68K_READ_BYTE_ROM(1, 0x080000)
M68K_READ_BYTE_ROM(2, 0x100000)
M68K_READ_BYTE_ROM(3, 0x180000)

uint8_t M68K_Read_Byte_Rom4(uint32_t address)
{
	if (sram_read_window(address))
	{
		if (SRAM_Custom & 1)
			return 0;

		return SRAM[address - (uint32_t)SRAM_Start];	// no byte swapped
	}

	return Rom_Data.u8[((address & 0x7FFFF) | 0x200000) ^ 1];
}

M68K_READ_BYTE_ROM(5, 0x280000)
M68K_READ_BYTE_ROM(6, 0x300000)
M68K_READ_BYTE_ROM(7, 0x380000)

static M68K_READ_BYTE_ROM(8, 0x400000)
static M68K_READ_BYTE_ROM(9, 0x480000)
static M68K_READ_BYTE_ROM(A, 0x500000)
static M68K_READ_BYTE_ROM(B, 0x580000)

static uint8_t M68K_Read_Byte_RomC(uint32_t address) { (void)address; return 0; }
static uint8_t M68K_Read_Byte_RomD(uint32_t address) { (void)address; return 0; }
static uint8_t M68K_Read_Byte_RomE(uint32_t address) { (void)address; return 0; }
static uint8_t M68K_Read_Byte_RomF(uint32_t address) { (void)address; return 0; }

uint8_t M68K_Read_Byte_Ram(uint32_t address)
{
	return Ram_68k.u8[(address & 0xFFFF) ^ 1];
}

static uint8_t M68K_Read_Byte_Misc(uint32_t address)
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
			return 0x81;

		// Z80 off. (unsigned compare: ja)
		if ((uint32_t)(main68k_readOdometer() - (uint32_t)Last_BUS_REQ_Cnt) >
		    CYCLE_FOR_TAKE_Z80_BUS_GENESIS)
		{
			// Bus taken.
			return 0x80;
		}

		return ((uint8_t)Last_BUS_REQ_St | 0x80);
	}

	if (address > 0xA1000D)
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
		case 0x0E:	// bad
		default:
			return 0;
	}
}

uint8_t M68K_Read_Byte_VDP(uint32_t address)
{
	// Valid address: ((address & 0xE700E0) == 0xC00000)
	// Information from vdppin.txt, (c) 2008 Charles MacDonald.
	if (address & 0x700E0)
		return 0;
	address &= 0x1F;	// only check low 5 bits.

	if (address < 0x04)
		return 0;

	if (address < 0x08)
	{
		// VDP status.
		uint16_t status = VDP_Read_Status();
		if (!(address & 1))
		{
			// on lit que le poids fort (read the high byte)
			return (uint8_t)(status >> 8);
		}
		return (uint8_t)status;
	}

	if (address > 0x09)
		return 0;

	// VDP counter.
	if (address & 1)
		return VDP_Read_H_Counter();
	return VDP_Read_V_Counter();
}


/******** Read Word Proc ********/

uint16_t M68K_Read_Word_Bad(uint32_t address)
{
	(void)address;
	return 0;
}

#define M68K_READ_WORD_ROM(n, base) \
uint16_t M68K_Read_Word_Rom##n(uint32_t address) \
{ \
	return Rom_Data.u16[((address & 0x7FFFF) + (base)) >> 1]; \
}

M68K_READ_WORD_ROM(0, 0x000000)
M68K_READ_WORD_ROM(1, 0x080000)
M68K_READ_WORD_ROM(2, 0x100000)
M68K_READ_WORD_ROM(3, 0x180000)

uint16_t M68K_Read_Word_Rom4(uint32_t address)
{
	if (sram_read_window(address))
	{
		uint32_t offset;
		uint16_t data;

		if (SRAM_Custom & 1)
			return 0;

		// no byte swapped (big-endian in SRAM[]; asm: movzx word + rol ax, 8)
		offset = address - (uint32_t)SRAM_Start;
		data = (uint16_t)(SRAM[offset] << 8);
		if (offset + 1 < sizeof(SRAM))
			data |= SRAM[offset + 1];
		return data;
	}

	return Rom_Data.u16[((address & 0x7FFFF) + 0x200000) >> 1];
}

M68K_READ_WORD_ROM(5, 0x280000)
M68K_READ_WORD_ROM(6, 0x300000)
M68K_READ_WORD_ROM(7, 0x380000)

static M68K_READ_WORD_ROM(8, 0x400000)
static M68K_READ_WORD_ROM(9, 0x480000)
static M68K_READ_WORD_ROM(A, 0x500000)
static M68K_READ_WORD_ROM(B, 0x580000)

static uint16_t M68K_Read_Word_RomC(uint32_t address) { (void)address; return 0; }
static uint16_t M68K_Read_Word_RomD(uint32_t address) { (void)address; return 0; }
static uint16_t M68K_Read_Word_RomE(uint32_t address) { (void)address; return 0; }
static uint16_t M68K_Read_Word_RomF(uint32_t address) { (void)address; return 0; }

uint16_t M68K_Read_Word_Ram(uint32_t address)
{
	return Ram_68k.u16[(address & 0xFFFF) >> 1];
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

static uint16_t M68K_Read_Word_Misc(uint32_t address)
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

		ah = (uint8_t)((uint8_t)Last_BUS_REQ_St + 0x80);
		al = fake_fetch_next();
		return (uint16_t)((ah << 8) | al);
	}

	if (address > 0xA1000D)
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
		case 0x0E:	// bad
		default:
			return 0;
	}
}

uint16_t M68K_Read_Word_VDP(uint32_t address)
{
	uint8_t v_counter;

	// Valid address: ((address & 0xE700E0) == 0xC00000)
	// Information from vdppin.txt, (c) 2008 Charles MacDonald.
	if (address & 0x700E0)
		return 0;
	address &= 0x1F;	// only check low 5 bits.

	// TODO: $C00008 == HV counter.
	if (address <= 0x03)
		return VDP_Read_Data();

	if (address <= 0x07)
		return VDP_Read_Status();

	if (address > 0x09)
		return 0;

	// HV counter: V counter in the high byte, H counter in the low byte.
	// (V counter is read first, as in the asm.)
	v_counter = VDP_Read_V_Counter();
	return (uint16_t)((v_counter << 8) | VDP_Read_H_Counter());
}


/******** Write Byte Proc ********/

void M68K_Write_Byte_Bad(uint32_t address, uint8_t data)
{
	(void)address;
	(void)data;
}

void M68K_Write_Byte_SRAM(uint32_t address, uint8_t data)
{
	if (!(SRAM_ON & 1))
		return;
	if (!(SRAM_Write & 1))
		return;
	if (address < (uint32_t)SRAM_Start)
		return;
	if (address > (uint32_t)SRAM_End)
		return;

	// If SRAM is disabled by the user, don't do anything.
	if (!(SRAM_Enabled & 1))
		return;

	SRAM[address - (uint32_t)SRAM_Start] = data;
}

void M68K_Write_Byte_Ram(uint32_t address, uint8_t data)
{
	Ram_68k.u8[(address & 0xFFFF) ^ 1] = data;
}

/**
 * z80_busreq_write(): Handle a write to the Z80 bus request register. (0xA11100)
 * Shared by M68K_Write_Byte_Misc and M68K_Write_Word_Misc.
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

static void M68K_Write_Byte_Misc(uint32_t address, uint8_t data)
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

	// no_ctrl_io
	if (address < 0xA130F1)
		return;

	if (address == 0xA130F1)
	{
		// SRAM control. (asm: setnz / setz byte stores)
		SRAM_ON = set_low_byte_int(SRAM_ON, ((data & 1) ? 1 : 0));
		SRAM_Write = set_low_byte_int(SRAM_Write, ((data & 2) ? 0 : 1));
		return;
	}

	if (address > 0xA130FF)
		return;

	// Super Street Fighter II (SSF2) bankswitching system.
	{
		const unsigned int bank = data & 0x1F;
		const unsigned int idx = (address & 0xF) >> 1;
		M68K_Read_Byte_Table[idx] = Genesis_M68K_Read_Byte_Table[bank];
		M68K_Read_Word_Table[idx] = Genesis_M68K_Read_Word_Table[bank];
	}
}

void M68K_Write_Byte_VDP(uint32_t address, uint8_t data)
{
	// Valid address: ((address & 0xE700E0) == 0xC00000)
	// Information from vdppin.txt, (c) 2008 Charles MacDonald.
	if (address & 0x700E0)
		return;
	address &= 0x1F;	// only check low 5 bits.

	if (address <= 0x03)
	{
		VDP_Write_Data_Byte(data);
		return;
	}

	if (address != 0x11)
		return;

	PSG_Write(data);
}


/******** Write Word Proc ********/

void M68K_Write_Word_Bad(uint32_t address, uint16_t data)
{
	(void)address;
	(void)data;
}

void M68K_Write_Word_SRAM(uint32_t address, uint16_t data)
{
	uint32_t offset;

	if (!(SRAM_ON & 1))
		return;
	if (!(SRAM_Write & 1))
		return;
	if (address < (uint32_t)SRAM_Start)
		return;
	if (address > (uint32_t)SRAM_End)
		return;

	// If SRAM is disabled by the user, don't do anything.
	if (!(SRAM_Enabled & 1))
		return;

	// asm: rol ax, 8; mov [SRAM + offset], ax (big-endian in SRAM[])
	offset = address - (uint32_t)SRAM_Start;
	SRAM[offset] = (uint8_t)(data >> 8);
	if (offset + 1 < sizeof(SRAM))
		SRAM[offset + 1] = (uint8_t)(data & 0xFF);
}

void M68K_Write_Word_Ram(uint32_t address, uint16_t data)
{
	Ram_68k.u16[(address & 0xFFFF) >> 1] = data;
}

static void M68K_Write_Word_Misc(uint32_t address, uint16_t data)
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

	// no_ctrl_io
	if (address < 0xA130F0)
		return;

	if (address == 0xA130F0)
	{
		// SRAM control. (asm: setnz / setz byte stores)
		SRAM_ON = set_low_byte_int(SRAM_ON, ((data & 1) ? 1 : 0));
		SRAM_Write = set_low_byte_int(SRAM_Write, ((data & 2) ? 0 : 1));
		return;
	}

	if (address > 0xA130FF)
		return;

	// Super Street Fighter II (SSF2) bankswitching system.
	// NOTE: The asm uses the *high* byte of the word. (mov al, ah)
	{
		const unsigned int bank = (data >> 8) & 0x1F;
		const unsigned int idx = (address & 0xF) >> 1;
		M68K_Read_Byte_Table[idx] = Genesis_M68K_Read_Byte_Table[bank];
		M68K_Read_Word_Table[idx] = Genesis_M68K_Read_Word_Table[bank];
	}
}

void M68K_Write_Word_VDP(uint32_t address, uint16_t data)
{
	// Valid address: ((address & 0xE700E0) == 0xC00000)
	// Information from vdppin.txt, (c) 2008 Charles MacDonald.
	if (address & 0x700E0)
		return;
	address &= 0x1F;	// only check low 5 bits.

	if (address <= 0x03)
	{
		VDP_Write_Data_Word(data);
		return;
	}

	if (address > 0x07)
		return;

	VDP_Write_Ctrl(data);
}
