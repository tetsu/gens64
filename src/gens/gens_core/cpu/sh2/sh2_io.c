/***************************************************************************
 * Gens: SH2 emulator - on-chip I/O registers and cache area. (C version)  *
 *                                                                         *
 * SH2 emulator 1.60 (ASM source part)                                     *
 * Copyright 2002 Stéphane Dallongeville                                   *
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
 * C port of sh2_io.asm:
 * - SH2_Read_* / SH2_Write_*: access an SH2's address space from outside.
 * - SH2_*_C0: cache data array (0xC0xxxxxx), stored big-endian.
 * - SH2_*_FF: on-chip peripherals (0xFFFFFE00-0xFFFFFFFF): FRT, WDT,
 *   interrupt controller, DIVU, DMAC, BSC.
 *
 * The asm handlers use ebp as "the current SH2 context"; here that is
 * SH2_Cur (see sh2.h). Byte/word stores into 32-bit context fields are
 * done exactly like the asm's partial stores (x86 little-endian byte
 * numbering: byte 0 = bits 0-7), so untouched bytes are preserved.
 */

#include "sh2.h"

#include <stdint.h>

#ifndef GENS_CORE_C
#error sh2_io.c is only used by the C core (GENS_CORE_C).
#endif

/** Partial-store helpers (x86 byte numbering). **/

static inline uint8_t get_byte(uint32_t v, unsigned int n)
{
	return (uint8_t)(v >> (n * 8));
}

static inline void set_byte(uint32_t *p, unsigned int n, uint8_t v)
{
	*p = (*p & ~(0xFFU << (n * 8))) | ((uint32_t)v << (n * 8));
}

static inline void set_word(uint32_t *p, unsigned int n, uint16_t v)
{
	*p = (*p & ~(0xFFFFU << (n * 8))) | ((uint32_t)v << (n * 8));
}


/*****************************************************************
 * External access: SH2_Read_* / SH2_Write_* (SH2_CONTEXT *, adr) *
 *****************************************************************/

uint8_t SH2_Read_Byte(SH2_CONTEXT *SH2, uint32_t adr)
{
	SH2_CONTEXT *prev = SH2_Cur;
	uint8_t ret;
	SH2_Cur = SH2;
	ret = SH2->Read_Byte[adr >> 24](adr);
	SH2_Cur = prev;
	return ret;
}

uint16_t SH2_Read_Word(SH2_CONTEXT *SH2, uint32_t adr)
{
	SH2_CONTEXT *prev = SH2_Cur;
	uint16_t ret;
	SH2_Cur = SH2;
	ret = SH2->Read_Word[adr >> 24](adr);
	SH2_Cur = prev;
	return ret;
}

uint32_t SH2_Read_Long(SH2_CONTEXT *SH2, uint32_t adr)
{
	SH2_CONTEXT *prev = SH2_Cur;
	uint32_t ret;
	SH2_Cur = SH2;
	ret = SH2->Read_Long[adr >> 24](adr);
	SH2_Cur = prev;
	return ret;
}

void SH2_Write_Byte(SH2_CONTEXT *SH2, uint32_t adr, uint8_t data)
{
	SH2_CONTEXT *prev = SH2_Cur;
	SH2_Cur = SH2;
	SH2->Write_Byte[adr >> 24](adr, data);
	SH2_Cur = prev;
}

void SH2_Write_Word(SH2_CONTEXT *SH2, uint32_t adr, uint16_t data)
{
	SH2_CONTEXT *prev = SH2_Cur;
	SH2_Cur = SH2;
	SH2->Write_Word[adr >> 24](adr, data);
	SH2_Cur = prev;
}

void SH2_Write_Long(SH2_CONTEXT *SH2, uint32_t adr, uint32_t data)
{
	SH2_CONTEXT *prev = SH2_Cur;
	SH2_Cur = SH2;
	SH2->Write_Long[adr >> 24](adr, data);
	SH2_Cur = prev;
}


/*********************************************
 * Cache data array (0xC0000000-0xC0000FFF). *
 * Stored big-endian, like the asm.          *
 *********************************************/

uint8_t FASTCALL SH2_Read_Byte_C0(uint32_t adr)
{
	return SH2_Cur->Cache[adr & 0xFFF];
}

uint16_t FASTCALL SH2_Read_Word_C0(uint32_t adr)
{
	const uint8_t *p = &SH2_Cur->Cache[adr & 0xFFE];
	return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t FASTCALL SH2_Read_Long_C0(uint32_t adr)
{
	const uint8_t *p = &SH2_Cur->Cache[adr & 0xFFC];
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void FASTCALL SH2_Write_Byte_C0(uint32_t adr, uint8_t data)
{
	SH2_Cur->Cache[adr & 0xFFF] = data;
}

void FASTCALL SH2_Write_Word_C0(uint32_t adr, uint16_t data)
{
	uint8_t *p = &SH2_Cur->Cache[adr & 0xFFE];
	p[0] = (uint8_t)(data >> 8);
	p[1] = (uint8_t)data;
}

void FASTCALL SH2_Write_Long_C0(uint32_t adr, uint32_t data)
{
	uint8_t *p = &SH2_Cur->Cache[adr & 0xFFC];
	p[0] = (uint8_t)(data >> 24);
	p[1] = (uint8_t)(data >> 16);
	p[2] = (uint8_t)(data >> 8);
	p[3] = (uint8_t)data;
}


/****************************************************************
 * On-chip I/O registers (0xFFFFFE00-0xFFFFFFFF).               *
 *                                                              *
 * IO_Reg[] keeps a big-endian copy of everything written; reads *
 * of registers without a handler return that copy.             *
 *                                                              *
 * Accesses with (adr & 0xFFFE00) == 0 are ignored. For reads,  *
 * the asm then returns eax unchanged, which is (adr >> 24) =   *
 * 0xFF since these handlers are only mapped at 0xFF.           *
 ****************************************************************/

uint8_t FASTCALL SH2_Read_Byte_FF(uint32_t adr)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;

	if (!(adr & 0xFFFE00))
		return 0xFF;

	reg = adr & 0x1FF;
	switch (reg)
	{
		case 0x010:	// E10: TIER
			return (uint8_t)(sh2->FRTTIER | 0x01);
		case 0x011:	// E11: FTCSR
			return sh2->FRTCSR;
		case 0x012:	// E12: FRC H
			return get_byte(sh2->FRTCNT, 2);
		case 0x013:	// E13: FRC L
			return get_byte(sh2->FRTCNT, 1);
		case 0x014:	// E14: OCRA/B H
			if (sh2->FRTTOCR & 0x10)
				return get_byte(sh2->FRTOCRB, 2);
			return get_byte(sh2->FRTOCRA, 2);
		case 0x015:	// E15: OCRA/B L
			if (sh2->FRTTOCR & 0x10)
				return get_byte(sh2->FRTOCRB, 1);
			return get_byte(sh2->FRTOCRA, 1);
		case 0x016:	// E16: TCR
			return sh2->FRTTCR;
		case 0x017:	// E17: TOCR
			return (uint8_t)(sh2->FRTTOCR | 0xE0);
		case 0x018:	// E18: ICR H
			return get_byte(sh2->FRTICR, 2);
		case 0x019:	// E19: ICR L
			return get_byte(sh2->FRTICR, 1);
		case 0x080:	// E80: WDTCSR
			return sh2->WDTSR;
		case 0x081:	// E81: WDTCNT
			return get_byte(sh2->WDTCNT, 2);
		case 0x083:	// E83: WDTRST
			return sh2->WDTRST;
		default:
			return sh2->IO_Reg[reg];
	}
}


uint16_t FASTCALL SH2_Read_Word_FF(uint32_t adr)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;

	if (!(adr & 0xFFFE00))
		return 0x00FF;

	reg = adr & 0x1FE;
	switch (reg)
	{
		case 0x108:	return (uint16_t)(sh2->DVCR >> 16);	// F08: DVCR H
		case 0x10A:	return (uint16_t)sh2->DVCR;		// F0A: DVCR L
		case 0x10C:	return (uint16_t)(sh2->VCRDIV >> 16);	// F0C: VCRDIV H
		case 0x10E:	return (uint16_t)sh2->VCRDIV;		// F0E: VCRDIV L
		case 0x140:	return (uint16_t)(sh2->BARA >> 16);	// F40: BARA H
		case 0x142:	return (uint16_t)sh2->BARA;		// F42: BARA L
		case 0x144:	return (uint16_t)(sh2->BAMRA >> 16);	// F44: BAMRA H
		case 0x146:	return (uint16_t)sh2->BAMRA;		// F46: BAMRA L
		case 0x1A0:	return (uint16_t)(sh2->VCRDMA0 >> 16);	// FA0: VCRDMA0 H
		case 0x1A2:	return (uint16_t)sh2->VCRDMA0;		// FA2: VCRDMA0 L
		case 0x1A4:	return (uint16_t)(sh2->VCRDMA1 >> 16);	// FA4: VCRDMA1 H
		case 0x1A6:	return (uint16_t)sh2->VCRDMA1;		// FA6: VCRDMA1 L
		case 0x1E0:	return (uint16_t)sh2->BCR1;		// FE0: BCR1
		default:
			return (uint16_t)((sh2->IO_Reg[reg] << 8) | sh2->IO_Reg[reg + 1]);
	}
}


uint32_t FASTCALL SH2_Read_Long_FF(uint32_t adr)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;
	const uint8_t *p;

	if (!(adr & 0xFFFE00))
		return 0x000000FF;

	reg = adr & 0x1FC;
	switch (reg)
	{
		case 0x100:	return sh2->DVSR;	// F00
		case 0x104:	return sh2->DVDNTL;	// F04: DVDNT
		case 0x108:	return sh2->DVCR;	// F08
		case 0x10C:	return sh2->VCRDIV;	// F0C
		case 0x110:	return sh2->DVDNTH;	// F10
		case 0x114:	return sh2->DVDNTL;	// F14
		case 0x118:	return sh2->DVDNTH;	// F18 (mirror)
		case 0x11C:	return sh2->DVDNTL;	// F1C (mirror)
		case 0x140:	return sh2->BARA;	// F40
		case 0x144:	return sh2->BAMRA;	// F44
		case 0x180:	return sh2->SAR0;	// F80
		case 0x184:	return sh2->DAR0;	// F84
		case 0x188:	return sh2->TCR0;	// F88
		case 0x18C:	return sh2->CHCR0;	// F8C
		case 0x190:	return sh2->SAR1;	// F90
		case 0x194:	return sh2->DAR1;	// F94
		case 0x198:	return sh2->TCR1;	// F98
		case 0x19C:	return sh2->CHCR1;	// F9C
		case 0x1A0:	return sh2->VCRDMA0;	// FA0
		case 0x1A4:	return sh2->VCRDMA1;	// FA4
		case 0x1B0:	return sh2->DMAOR;	// FB0
		case 0x1E0:	return sh2->BCR1;	// FE0
		default:
			p = &sh2->IO_Reg[reg];
			return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	}
}


void FASTCALL SH2_Write_Byte_FF(uint32_t adr, uint8_t data)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;

	if (!(adr & 0xFFFE00))
		return;

	reg = adr & 0x1FF;
	sh2->IO_Reg[reg] = data;

	switch (reg)
	{
		case 0x010:	// E10: TIER
			sh2->FRTTIER = (uint8_t)(data & 0x8E);
			break;
		case 0x011:	// E11: FTCSR (flags can only be cleared; bit 0 is written)
			sh2->FRTCSR = (uint8_t)((sh2->FRTCSR & data) | (data & 1));
			break;
		case 0x012:	// E12: FRC H
			set_byte(&sh2->FRTCNT, 2, data);
			break;
		case 0x013:	// E13: FRC L
			set_byte(&sh2->FRTCNT, 1, data);
			break;
		case 0x014:	// E14: OCRA/B H
			if (sh2->FRTTOCR & 0x10)
				set_byte(&sh2->FRTOCRB, 2, data);
			else
				set_byte(&sh2->FRTOCRA, 2, data);
			break;
		case 0x015:	// E15: OCRA/B L
			if (sh2->FRTTOCR & 0x10)
				set_byte(&sh2->FRTOCRB, 1, data);
			else
				set_byte(&sh2->FRTOCRA, 1, data);
			break;
		case 0x016:	// E16: TCR
			sh2->FRTTCR = data;
			set_byte(&sh2->FRT_Sft, 0, sh2->FRT_Tab[data & 3]);
			break;
		case 0x017:	// E17: TOCR
			sh2->FRTTOCR = data;
			break;
		case 0x0E2:	// EE2: IPRA H
			set_byte(&sh2->IPDMA, 0, (uint8_t)(data & 0xF));
			set_byte(&sh2->IPDIV, 0, (uint8_t)(data >> 4));
			break;
		case 0x0E3:	// EE3: IPRA L
			set_byte(&sh2->IPWDT, 0, (uint8_t)(data >> 4));
			break;
		case 0x0E4:	// EE4: VCRWDT
			set_byte(&sh2->VCRWDT, 0, (uint8_t)(data & 0x7F));
			break;
		default:
			break;
	}
}


void FASTCALL SH2_Write_Word_FF(uint32_t adr, uint16_t data)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;
	const uint8_t hi = (uint8_t)(data >> 8);
	const uint8_t lo = (uint8_t)data;

	if (!(adr & 0xFFFE00))
		return;

	reg = adr & 0x1FE;
	sh2->IO_Reg[reg + 0] = hi;
	sh2->IO_Reg[reg + 1] = lo;

	switch (reg)
	{
		case 0x080:	// E80: WDTCSR (0xA5xx) / WDTCNT (other)
			if (hi == 0xA5)
			{
				sh2->WDTSR = lo;
				sh2->WDT_Sft = sh2->WDT_Tab[lo & 7];
			}
			else
			{
				sh2->WDTCNT = (uint32_t)lo << 16;
			}
			break;
		case 0x082:	// E82: WDTRST
			if (hi == 0x5A)
				sh2->WDTRST = (uint8_t)((sh2->WDTRST & 0x80) | (lo & 0x60));
			else if (!(lo & 0x80))
				sh2->WDTRST &= (uint8_t)~0x80;
			break;
		case 0x0E2:	// EE2: IPRA
			set_byte(&sh2->IPDIV, 0, (uint8_t)(hi >> 4));
			set_byte(&sh2->IPDMA, 0, (uint8_t)(hi & 0xF));
			set_byte(&sh2->IPWDT, 0, (uint8_t)(lo >> 4));
			break;
		case 0x0E4:	// EE4: VCRWDT
			set_byte(&sh2->VCRWDT, 0, (uint8_t)(hi & 0x7F));
			break;
		case 0x108:	set_word(&sh2->DVCR, 2, data);		break;	// F08
		case 0x10A:	set_word(&sh2->DVCR, 0, data);		break;	// F0A
		case 0x10C:	set_word(&sh2->VCRDIV, 2, data);	break;	// F0C
		case 0x10E:	set_word(&sh2->VCRDIV, 0, data);	break;	// F0E
		case 0x140:	set_word(&sh2->BARA, 2, data);		break;	// F40
		case 0x142:	set_word(&sh2->BARA, 0, data);		break;	// F42
		case 0x144:	set_word(&sh2->BAMRA, 2, data);		break;	// F44
		case 0x146:	set_word(&sh2->BAMRA, 0, data);		break;	// F46
		case 0x1A0:	set_word(&sh2->VCRDMA0, 2, data);	break;	// FA0
		case 0x1A2:	set_word(&sh2->VCRDMA0, 0, data);	break;	// FA2
		case 0x1A4:	set_word(&sh2->VCRDMA1, 2, data);	break;	// FA4
		case 0x1A6:	set_word(&sh2->VCRDMA1, 0, data);	break;	// FA6
		case 0x1E0:	// FE0: BCR1
			set_word(&sh2->BCR1, 0, (uint16_t)(data & 0x1FF7));
			break;
		default:
			break;
	}
}


/*************************
 * Division unit (DIVU). *
 *************************/

/** DVDNT (F04): 32 / 32 signed division. **/
static void divu_dvdnt(SH2_CONTEXT *sh2, uint32_t data)
{
	const int32_t dividend = (int32_t)data;
	const int32_t divisor = (int32_t)sh2->DVSR;

	sh2->DVDNTL = data;
	if (divisor == 0)
	{
		// The asm sign-extends the dividend (cdq) into DVDNTH.
		sh2->DVDNTH = (dividend < 0 ? 0xFFFFFFFFU : 0);
		sh2->DVCR |= 1;
		return;
	}

	// 0x80000000 / -1 would fault in the asm (idiv #DE).
	// Use 64-bit math; the quotient wraps to 0x80000000, remainder 0.
	sh2->DVDNTH = (uint32_t)(int32_t)((int64_t)dividend % (int64_t)divisor);
	sh2->DVDNTL = (uint32_t)(int64_t)((int64_t)dividend / (int64_t)divisor);
}


/** DVDNTL (F14/F1C): 64 / 32 signed division (DVDNTH:DVDNTL / DVSR). **/
static void divu_dvdntl(SH2_CONTEXT *sh2, uint32_t data)
{
	const uint32_t dvsr = sh2->DVSR;
	const uint32_t dvdnth = sh2->DVDNTH;
	uint32_t chk_hi, chk_div;
	int overflow = 0;
	int64_t dividend, quot, rem;

	sh2->DVDNTL = data;
	if (dvsr == 0)
	{
		sh2->DVCR |= 1;
		return;
	}

	// Overflow check, exactly as the asm does it.
	chk_hi = (dvdnth << 1) | (data >> 31);		// shld edx, eax, 1
	chk_div = dvsr;
	if ((int32_t)chk_div < 0)
		chk_div = 0U - chk_div;		// neg (0x80000000 stays)
	if ((int32_t)chk_hi < 0)
	{
		if (chk_hi != 0xFFFFFFFFU)
		{
			chk_hi = 0U - chk_hi;
			if (chk_hi >= chk_div)
				overflow = 1;
		}
	}
	else if (chk_hi >= chk_div)
	{
		overflow = 1;
	}

	if (!overflow)
	{
		dividend = (int64_t)(((uint64_t)dvdnth << 32) | data);
		if (dividend == INT64_MIN && (int32_t)dvsr == -1)
		{
			// INT64_MIN / -1 is undefined in C (and faults in the asm).
			quot = INT64_MAX;
			rem = 0;
		}
		else
		{
			quot = dividend / (int32_t)dvsr;
			rem = dividend % (int32_t)dvsr;
		}
		if (quot >= INT32_MIN && quot <= INT32_MAX)
		{
			sh2->DVDNTH = (uint32_t)(int32_t)rem;
			sh2->DVDNTL = (uint32_t)(int32_t)quot;
			return;
		}
		// The asm's check misses this case and idiv would fault (#DE).
		// Treat it as an overflow instead.
	}

	// Overflow: saturate according to the sign of (DVDNTH ^ DVSR).
	sh2->DVDNTL = ((int32_t)(dvdnth ^ dvsr) < 0 ? 0x80000000U : 0x7FFFFFFFU);
	sh2->DVDNTH = 0;
	sh2->DVCR |= 1;
}


/****************************
 * DMA controller (DMAC).   *
 ****************************/

/**
 * dma_channel_t: Pointers to one DMA channel's registers.
 */
typedef struct
{
	uint32_t *SAR;
	uint32_t *DAR;
	uint32_t *TCR;
	uint32_t *CHCR;
	uint32_t *VCRDMA;
	uint8_t *DREQ;
} dma_channel_t;

static void dma_get_channel(SH2_CONTEXT *sh2, int ch, dma_channel_t *dc)
{
	if (ch == 0)
	{
		dc->SAR = &sh2->SAR0;
		dc->DAR = &sh2->DAR0;
		dc->TCR = &sh2->TCR0;
		dc->CHCR = &sh2->CHCR0;
		dc->VCRDMA = &sh2->VCRDMA0;
		dc->DREQ = &sh2->DREQ0;
	}
	else
	{
		dc->SAR = &sh2->SAR1;
		dc->DAR = &sh2->DAR1;
		dc->TCR = &sh2->TCR1;
		dc->CHCR = &sh2->CHCR1;
		dc->VCRDMA = &sh2->VCRDMA1;
		dc->DREQ = &sh2->DREQ1;
	}
}


/**
 * dma_run(): Run a DMA transfer.
 * @param chcr CHCR value (transfer size and address modes).
 * @param count Transfer count (TCR).
 * @param dreq If nonzero: external request mode (loop while DREQ is set);
 *             otherwise: auto request mode (loop until count reaches 0).
 */
static void dma_run(SH2_CONTEXT *sh2, const dma_channel_t *dc, uint32_t chcr,
		    uint32_t count, int dreq)
{
	static const uint32_t size_tab[4] = {1, 2, 4, 8 + 4};
	const unsigned int ts = (chcr >> 10) & 3;	// transfer size
	const unsigned int sm = (chcr >> 12) & 3;	// source mode
	const unsigned int dm = (chcr >> 14) & 3;	// destination mode
	uint32_t unit = size_tab[ts];
	uint32_t src = *dc->SAR;
	uint32_t dst = *dc->DAR;
	uint8_t al;

	// Read/write functions are selected once, from the start addresses.
	SH2_RB *rb = sh2->Read_Byte[src >> 24];
	SH2_RW *rw = sh2->Read_Word[src >> 24];
	SH2_RL *rl = sh2->Read_Long[src >> 24];
	SH2_WB *wb = sh2->Write_Byte[dst >> 24];
	SH2_WW *ww = sh2->Write_Word[dst >> 24];
	SH2_WL *wl = sh2->Write_Long[dst >> 24];

	if (unit & 8)
		count *= 4;	// 16-byte units: 4 longwords each
	unit &= 7;

	do
	{
		// Decrement modes adjust the address before the transfer.
		if (dm == 2)
			dst -= unit;
		if (sm == 2)
			src -= unit;

		switch (ts)
		{
			case 0:
				wb(dst, rb(src));
				break;
			case 1:
				ww(dst, rw(src));
				break;
			default:
				wl(dst, rl(src));
				break;
		}

		// Increment modes adjust the address after the transfer.
		if (dm == 1)
			dst += unit;
		if (sm == 1)
			src += unit;

		count--;
		if (dreq)
		{
			if (!*dc->DREQ)
				break;
		}
		else if (count == 0)
			break;
	} while (1);

	if (dreq)
	{
		// External request: the transfer stops when DREQ drops.
		// TE is set if the count reached (or went below) zero.
		al = (uint8_t)(*dc->CHCR & 0xFF);
		*dc->TCR = count;
		*dc->SAR = src;
		*dc->DAR = dst;
		if ((int32_t)count <= 0)
			al |= 0x02;
		set_byte(dc->CHCR, 0, al);
	}
	else
	{
		// Auto request: transfer complete.
		al = (uint8_t)((*dc->CHCR & 0xFF) | 0x02);
		*dc->TCR = 0;
		*dc->SAR = src;
		if (al & 0x04)
		{
			// Interrupt enable: vector from VCRDMA, priority from IPRA.
			uint32_t level_vector = ((uint32_t)get_byte(*dc->VCRDMA, 0) << 8) |
						get_byte(sh2->IPDMA, 0);
			SH2_Interrupt_Internal(sh2, level_vector);
		}
		*dc->DAR = dst;
		set_byte(dc->CHCR, 0, al);
	}
}


/**
 * dma_write_chcr(): Write CHCR0/CHCR1 and start a transfer if possible.
 * Also used after DMAOR writes and by SH2_DMA0/1_Request (via
 * SH2_Write_Long_FF).
 */
static void dma_write_chcr(SH2_CONTEXT *sh2, int ch, uint32_t data)
{
	dma_channel_t dc;
	uint8_t dmaor;
	uint32_t count;

	dma_get_channel(sh2, ch, &dc);

	// TE (bit 1) can't be set by writing 1; it keeps its current value.
	if (data & 2)
		data = (data & ~2U) | (*dc.CHCR & 2);

	dmaor = (uint8_t)(sh2->DMAOR & 0xFF);
	*dc.CHCR = data;

	if (data & 0x200)
	{
		// Auto request mode: DE=1, TE=0, DME=1, NMIF=0, AE=0.
		if ((data & 3) != 1 || (dmaor & 7) != 1)
			return;
		count = *dc.TCR;
		*dc.TCR = 0;	// prevent recursion with a DMA request during DMA
		if (count == 0)
			return;
		dma_run(sh2, &dc, data, count, 0);
	}
	else
	{
		// External request mode: DE=1, DME=1, NMIF=0, AE=0, and DREQ set.
		if ((data & 1) != 1 || (dmaor & 7) != 1)
			return;
		if (!*dc.DREQ)
			return;
		count = *dc.TCR;
		*dc.TCR = 0;	// prevent recursion with a DMA request during DMA
		if (count == 0)
			return;
		dma_run(sh2, &dc, data, count, 1);
	}
}


void FASTCALL SH2_Write_Long_FF(uint32_t adr, uint32_t data)
{
	SH2_CONTEXT *sh2 = SH2_Cur;
	unsigned int reg;
	uint8_t *p;

	if (!(adr & 0xFFFE00))
		return;

	reg = adr & 0x1FC;
	p = &sh2->IO_Reg[reg];
	p[0] = (uint8_t)(data >> 24);
	p[1] = (uint8_t)(data >> 16);
	p[2] = (uint8_t)(data >> 8);
	p[3] = (uint8_t)data;

	switch (reg)
	{
		case 0x100:	sh2->DVSR = data;		break;	// F00
		case 0x104:	divu_dvdnt(sh2, data);		break;	// F04: DVDNT
		case 0x108:	sh2->DVCR = data;		break;	// F08
		case 0x10C:	sh2->VCRDIV = data;		break;	// F0C
		case 0x110:	sh2->DVDNTH = data;		break;	// F10
		case 0x114:	divu_dvdntl(sh2, data);		break;	// F14
		case 0x118:	sh2->DVDNTH = data;		break;	// F18 (mirror)
		case 0x11C:	divu_dvdntl(sh2, data);		break;	// F1C (mirror)
		case 0x140:	sh2->BARA = data;		break;	// F40
		case 0x144:	sh2->BAMRA = data;		break;	// F44
		case 0x180:	sh2->SAR0 = data;		break;	// F80
		case 0x184:	sh2->DAR0 = data;		break;	// F84
		case 0x188:	sh2->TCR0 = data & 0xFFFFFF;	break;	// F88
		case 0x18C:	dma_write_chcr(sh2, 0, data);	break;	// F8C
		case 0x190:	sh2->SAR1 = data;		break;	// F90
		case 0x194:	sh2->DAR1 = data;		break;	// F94
		case 0x198:	sh2->TCR1 = data & 0xFFFFFF;	break;	// F98
		case 0x19C:	dma_write_chcr(sh2, 1, data);	break;	// F9C
		case 0x1A0:	sh2->VCRDMA0 = data;		break;	// FA0
		case 0x1A4:	sh2->VCRDMA1 = data;		break;	// FA4
		case 0x1B0:	// FB0: DMAOR
		{
			const uint8_t d = (uint8_t)data;
			const uint8_t old = (uint8_t)(sh2->DMAOR & 0xFF);
			set_byte(&sh2->DMAOR, 0, (uint8_t)((old & d & 0x6) | (d & 0x9)));

			// Re-evaluate both channels with their current CHCR values.
			dma_write_chcr(sh2, 0, sh2->CHCR0);
			dma_write_chcr(sh2, 1, sh2->CHCR1);
			break;
		}
		case 0x1E0:	sh2->BCR1 = data & 0x1FF7;	break;	// FE0
		default:
			break;
	}
}
