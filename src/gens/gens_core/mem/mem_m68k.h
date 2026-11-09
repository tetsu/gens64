/***************************************************************************
 * Gens: Main 68000 memory management functions.                           *
 *                                                                         *
 * Copyright (c) 1999-2002 by Stéphane Dallongeville                       *
 * Copyright (c) 2003-2004 by Stéphane Akhoun                              *
 * Copyright (c) 2008 by David Korth                                       *
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

#ifndef GENS_MEM_M68K_H
#define GENS_MEM_M68K_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern union
{
	uint8_t  u8[64*1024];
	uint16_t u16[(64*1024)>>1];
	uint32_t u32[(64*1024)>>2];
} Ram_68k;
extern union
{
	uint8_t  u8[6*1024*1024];
	uint16_t u16[(6*1024*1024)>>1];
	uint32_t u32[(6*1024*1024)>>2];
} Rom_Data;
extern unsigned char SRAM[64 * 1024];
extern unsigned char Ram_Backup_Ex[512 * 1024];	// Sega CD
extern unsigned char Genesis_Rom[2 * 1024];

/**
 * Main 68000 memory handler function types.
 * NOTE: In the x86 asm core, the tables contain pointers to asm handlers
 * that use a custom register-based calling convention; C code must only
 * copy table entries, never call them. In the C core, the handlers are
 * ordinary C functions that take a 24-bit address.
 */
typedef uint8_t  (*M68K_Read_Byte_fn)(uint32_t address);
typedef uint16_t (*M68K_Read_Word_fn)(uint32_t address);
typedef void     (*M68K_Write_Byte_fn)(uint32_t address, uint8_t data);
typedef void     (*M68K_Write_Word_fn)(uint32_t address, uint16_t data);

/** Read tables: indexed by (address >> 19) & 0x1F (512 KB pages). **/
extern M68K_Read_Byte_fn M68K_Read_Byte_Table[0x20];
extern M68K_Read_Word_fn M68K_Read_Word_Table[0x20];
/** Write tables: indexed by (address >> 20) & 0x0F (1 MB pages). **/
extern M68K_Write_Byte_fn M68K_Write_Byte_Table[0x10];
extern M68K_Write_Word_fn M68K_Write_Word_Table[0x10];

extern unsigned int Rom_Size;

extern int SRAM_Start;
extern int SRAM_End;
extern int SRAM_ON;
extern int SRAM_Write;
extern int SRAM_Custom;
extern int SRAM_Enabled;

extern int Z80_M68K_Cycle_Tab[512];

/** Z80 state. **/
#define Z80_STATE_ENABLED	(1 << 0)
#define Z80_STATE_BUSREQ	(1 << 1)
#define Z80_STATE_RESET		(1 << 2)

extern unsigned int Z80_State;
extern int Last_BUS_REQ_Cnt;
extern int Last_BUS_REQ_St;
extern int Bank_M68K;
extern int Fake_Fetch;

extern int CPL_M68K;
extern int CPL_Z80;
extern int Cycles_M68K;
extern int Cycles_Z80;

extern int Game_Mode;
extern int CPU_Mode;
extern int Gen_Mode;

void Init_Memory_M68K(int System_ID);
unsigned char M68K_RB(unsigned int Adr);
unsigned short M68K_RW(unsigned int Adr);
void M68K_WB(unsigned int Adr, unsigned char Data);
void M68K_WW(unsigned int Adr, unsigned short Data);

#ifdef __cplusplus
}
#endif

#endif /* GENS_MEM_M68K_H */
