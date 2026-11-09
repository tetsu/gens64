/***************************************************************************
 * Gens: Main 68000 memory handlers. (Shared with SegaCD / 32X maps.)      *
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

#ifndef GENS_MEM_M68K_HANDLERS_H
#define GENS_MEM_M68K_HANDLERS_H

#include <stdint.h>
#include "mem_m68k.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Genesis memory map tables. (Used for SSF2 bankswitching.)
 */
extern const M68K_Read_Byte_fn Genesis_M68K_Read_Byte_Table[0x20];
extern const M68K_Read_Word_fn Genesis_M68K_Read_Word_Table[0x20];
extern const M68K_Write_Byte_fn Genesis_M68K_Write_Byte_Table[0x10];
extern const M68K_Write_Word_fn Genesis_M68K_Write_Word_Table[0x10];

/**
 * Main 68000 memory handlers.
 * All handlers take a 24-bit address. (address & 0xFFFFFF)
 * Write handlers receive data already masked to 8 or 16 bits.
 */

/** Read Byte **/
uint8_t M68K_Read_Byte_Bad(uint32_t address);
uint8_t M68K_Read_Byte_Rom0(uint32_t address);
uint8_t M68K_Read_Byte_Rom1(uint32_t address);
uint8_t M68K_Read_Byte_Rom2(uint32_t address);
uint8_t M68K_Read_Byte_Rom3(uint32_t address);
uint8_t M68K_Read_Byte_Rom4(uint32_t address);
uint8_t M68K_Read_Byte_Rom5(uint32_t address);
uint8_t M68K_Read_Byte_Rom6(uint32_t address);
uint8_t M68K_Read_Byte_Rom7(uint32_t address);
uint8_t M68K_Read_Byte_Ram(uint32_t address);
uint8_t M68K_Read_Byte_VDP(uint32_t address);

/** Read Word **/
uint16_t M68K_Read_Word_Bad(uint32_t address);
uint16_t M68K_Read_Word_Rom0(uint32_t address);
uint16_t M68K_Read_Word_Rom1(uint32_t address);
uint16_t M68K_Read_Word_Rom2(uint32_t address);
uint16_t M68K_Read_Word_Rom3(uint32_t address);
uint16_t M68K_Read_Word_Rom4(uint32_t address);
uint16_t M68K_Read_Word_Rom5(uint32_t address);
uint16_t M68K_Read_Word_Rom6(uint32_t address);
uint16_t M68K_Read_Word_Rom7(uint32_t address);
uint16_t M68K_Read_Word_Ram(uint32_t address);
uint16_t M68K_Read_Word_VDP(uint32_t address);

/** Write Byte **/
void M68K_Write_Byte_Bad(uint32_t address, uint8_t data);
void M68K_Write_Byte_SRAM(uint32_t address, uint8_t data);
void M68K_Write_Byte_Ram(uint32_t address, uint8_t data);
void M68K_Write_Byte_VDP(uint32_t address, uint8_t data);

/** Write Word **/
void M68K_Write_Word_Bad(uint32_t address, uint16_t data);
void M68K_Write_Word_SRAM(uint32_t address, uint16_t data);
void M68K_Write_Word_Ram(uint32_t address, uint16_t data);
void M68K_Write_Word_VDP(uint32_t address, uint16_t data);

#ifdef __cplusplus
}
#endif

#endif /* GENS_MEM_M68K_HANDLERS_H */
