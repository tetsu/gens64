/***************************************************************************
 * Gens: SH2 memory management.                                            *
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
 * C port of mem_sh2.asm. (32X memory map as seen by the SH2s)
 *
 * Memory layout notes (same as the asm):
 * - _32X_Rom, _32X_Ram, _32X_MSH2_Rom, _32X_SSH2_Rom and _32X_Comm are
 *   stored as raw big-endian bytes: SH2 byte A is at u8[A].
 * - _32X_VDP_Ram (frame buffers) and _32X_VDP_CRam are stored as host-endian
 *   16-bit words: SH2 byte A is at u8[A ^ 1], word A at u16[A >> 1], and a
 *   long at A is (word[A] << 16) | word[A + 2].
 * - Several asm variables are accessed with partial (byte/word) loads and
 *   stores at fixed offsets. Those accesses are done here with shifts and
 *   masks (little-endian byte numbering, as in the asm).
 * - The asm keeps the current SH2 context in ebp; the C core uses SH2_Cur.
 */

#include <stdint.h>

#include "mem_sh2.h"
#include "gens_core/cpu/sh2/sh2.h"
#include "gens_core/cpu/sh2/cpu_sh2.h"
#include "gens_core/vdp/vdp_32x.h"
#include "gens_core/sound/pwm.h"

// MD bpp. (Defined in emulator/g_main.cpp; g_main.hpp is a C++ header.)
extern uint8_t bppMD;


/** Global variables. **/

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++-compat"

__typeof__(_32X_Rom)      _32X_Rom      __attribute__((aligned(64)));
__typeof__(_32X_Ram)      _32X_Ram;
__typeof__(_32X_MSH2_Rom) _32X_MSH2_Rom;
__typeof__(_32X_SSH2_Rom) _32X_SSH2_Rom;

#pragma GCC diagnostic pop

unsigned char _MSH2_Reg[0x40] __attribute__((aligned(64)));
unsigned char _SSH2_Reg[0x40];
unsigned char _SH2_VDP_Reg[0x10];
unsigned char _32X_Comm[0x10];
unsigned char _32X_ADEN;
unsigned char _32X_RES;
unsigned char _32X_FM;
unsigned char _32X_RV;

unsigned short _32X_FIFO_A[4] __attribute__((aligned(64)));
unsigned short _32X_FIFO_B[4];
unsigned int _32X_FIFO_Block;
unsigned int _32X_FIFO_Read;
unsigned int _32X_FIFO_Write;
unsigned int _32X_DREQ_ST;
unsigned int _32X_DREQ_SRC;
unsigned int _32X_DREQ_DST;
unsigned int _32X_DREQ_LEN;

unsigned char _32X_MINT __attribute__((aligned(64)));
unsigned char _32X_SINT;
unsigned char _32X_HIC;
unsigned int CPL_MSH2;
unsigned int CPL_SSH2;
int Cycles_MSH2;
int Cycles_SSH2;


/** Helpers. **/

/** Byte n (0 = least significant) of a 32-bit value. **/
static inline uint8_t get_b(uint32_t v, unsigned int n)
{
	return (uint8_t)(v >> (n * 8));
}

/** Replace byte n (0 = least significant) of a 32-bit variable. **/
static inline void set_b(unsigned int *p, unsigned int n, uint8_t b)
{
	*p = (*p & ~(0xFFU << (n * 8))) | ((uint32_t)b << (n * 8));
}

/** Replace the low 16 bits of a 32-bit variable. **/
static inline void set_w(unsigned int *p, uint16_t w)
{
	*p = (*p & 0xFFFF0000U) | w;
}

/** Subtract cycles from the current SH2's I/O cycle counter. **/
static inline void cycle_io(uint32_t cycles)
{
	SH2_Cur->Cycle_IO -= cycles;
}

/** Big-endian accesses to a raw byte buffer. **/
static inline uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Host-endian 16-bit word at a byte offset of _32X_VDP_Ram. **/
static inline uint16_t *vdp_ram_w(uint32_t byte_ofs)
{
	return &_32X_VDP_Ram.u16[(byte_ofs & (sizeof(_32X_VDP_Ram) - 1)) >> 1];
}

/** Long from _32X_VDP_Ram (rol 16 of the host dword). **/
static inline uint32_t vdp_ram_l(uint32_t byte_ofs)
{
	return ((uint32_t)*vdp_ram_w(byte_ofs) << 16) | *vdp_ram_w(byte_ofs + 2);
}

/** Byte of _32X_VDP_Ram at a host byte offset. **/
static inline uint8_t *vdp_ram_b(uint32_t byte_ofs)
{
	return &_32X_VDP_Ram.u8[byte_ofs & (sizeof(_32X_VDP_Ram) - 1)];
}

/** FIFO A and FIFO B are contiguous in the asm: byte offset 0-7 = A, 8-15 = B. **/
static inline uint16_t fifo_word(uint32_t byte_ofs)
{
	byte_ofs &= 0xF;
	if (byte_ofs < 8)
		return _32X_FIFO_A[byte_ofs >> 1];
	return _32X_FIFO_B[(byte_ofs - 8) >> 1];
}


/*****************************************************************
 * Helpers shared with the 68000 side (mem_m68k_32x.c).          *
 *****************************************************************/

/**
 * _32X_PWM_Full(): PWM_FULL_TAB entry for (read pointer, write pointer).
 * (The pointers come from savestates too, so the index is masked.)
 */
uint8_t _32X_PWM_Full(unsigned int rp, unsigned int wp)
{
	return PWM_FULL_TAB[((rp * PWM_BUF_SIZE) + wp) & ((PWM_BUF_SIZE * PWM_BUF_SIZE) - 1)];
}

/** _32X_Write_FM(): Frame buffer access mode (FM, bit 7). **/
void _32X_Write_FM(uint8_t data)
{
	const uint8_t fm = (uint8_t)(data & 0x80);
	const uint8_t changed = (uint8_t)(_32X_FM ^ fm);
	_32X_FM = fm;
	if (changed)
		_32X_Set_FB();
}

/**
 * _32X_Write_FB_Control(): 32X VDP frame buffer control (FS).
 * The swap is delayed until VBlank unless the display is blank.
 */
void _32X_Write_FB_Control(uint8_t data)
{
	const uint8_t mode = get_b(_32X_VDP.Mode, 0);
	const uint8_t state_h = get_b(_32X_VDP.State, 1);

	set_b(&_32X_VDP.State, 2, data);
	if ((mode & 3) && !(state_h & 0x80))
	{
		// Not in VBlank: the frame buffer swap is delayed.
		return;
	}

	set_b(&_32X_VDP.State, 0, data);
	_32X_Set_FB();
}

/** _32X_Write_CRam(): Update one CRam entry and its adjusted (host color) value. **/
void _32X_Write_CRam(unsigned int idx, uint16_t data)
{
	idx &= 0xFF;
	_32X_VDP_CRam[idx] = data;
	if (bppMD == 32)
		_32X_VDP_CRam_Adjusted.u32[idx] = _32X_Palette.u32[data];
	else
		_32X_VDP_CRam_Adjusted.u16[idx] = _32X_Palette.u16[data];
}

/**
 * _32X_VDP_Ram_Write_Word(): Write a word to _32X_VDP_Ram.
 * Auto fill can address one word past the end of the frame buffers
 * (the asm then writes past _32X_VDP_Ram); that write is dropped.
 */
void _32X_VDP_Ram_Write_Word(uint32_t byte_ofs, uint16_t data)
{
	if (byte_ofs < sizeof(_32X_VDP_Ram))
		_32X_VDP_Ram.u16[byte_ofs >> 1] = data;
}


/*********************************
 * 32X system registers: read.   *
 *********************************/

/** Byte read of the PWM pulse registers: the FIFO "full" state. **/
static uint8_t pwm_pulse_l_rb(void)
{
	return _32X_PWM_Full(PWM_RP_L, PWM_WP_L);
}

static uint8_t pwm_pulse_r_rb(void)
{
	return _32X_PWM_Full(PWM_RP_R, PWM_WP_R);
}

/**
 * sh2_32x_reg_rb(): Byte read from the 32X system registers.
 * @param adr Address.
 * @param slave 0 for MSH2, 1 for SSH2.
 */
static uint8_t sh2_32x_reg_rb(uint32_t adr, int slave)
{
	cycle_io(10);
	adr &= 0x3F;

	switch (adr)
	{
		case 0x00:	// IntC_H
			return (uint8_t)((uint8_t)(_32X_ADEN + _32X_ADEN) | _32X_FM);
		case 0x01:	// IntC_L
			return (slave ? _32X_SINT : _32X_MINT);
		case 0x05:	// HCnt
			return _32X_HIC;
		case 0x06:	// DREQ_H
			return get_b(_32X_DREQ_ST, 1);
		case 0x07:	// DREQ_L
			return (uint8_t)(_32X_RV | get_b(_32X_DREQ_ST, 0));

		case 0x30:	// PWM_Cont_H
			return get_b(PWM_Mode, 1);
		case 0x31:	// PWM_Cont_L
			return get_b(PWM_Mode, 0);
		case 0x32:	// PWM_Cycle_H
			return get_b(PWM_Cycle_Tmp, 1);
		case 0x33:	// PWM_Cycle_L
			return get_b(PWM_Cycle_Tmp, 0);
		case 0x34:	// PWM_Pulse_L
			return pwm_pulse_l_rb();
		case 0x36:	// PWM_Pulse_R
			return pwm_pulse_r_rb();
		case 0x38:	// PWM_Pulse_C (the asm table points to Pulse_L)
			return pwm_pulse_l_rb();

		default:
			if (adr >= 0x20 && adr < 0x30)
				return _32X_Comm[adr - 0x20];
			return 0;
	}
}


/**
 * sh2_rw_fifo(): Word read of the 32X FIFO register.
 */
static uint16_t sh2_rw_fifo(void)
{
	uint32_t st, rd, block, len;
	uint16_t val;

	st = _32X_DREQ_ST & 0x4004;
	rd = _32X_FIFO_Read;
	block = _32X_FIFO_Block ^ (4 * 2);
	if (st != 0x0004)
		return 0;

	val = fifo_word(block + (rd * 2));

	rd++;
	if (rd < 4)
	{
		_32X_FIFO_Read = rd;
		return val;
	}

	// FIFO A empty.
	len = (uint16_t)(_32X_DREQ_LEN - 4);
	set_w(&_32X_DREQ_LEN, (uint16_t)len);
	if (len == 0)
	{
		// FIFO read end.
		set_w(&_32X_DREQ_ST, 0);
		SH2_DMA0_Request(&M_SH2, 0);	// we use M_SH2 instead of EBP (like real 32X).
		return val;
	}

	if (get_b(_32X_DREQ_ST, 1) & 0x80)
	{
		_32X_FIFO_Block = block;
		set_b(&_32X_DREQ_ST, 1, 0);
		set_b(&_32X_FIFO_Write, 0, 0);
		set_b(&_32X_FIFO_Read, 0, 0);
		return val;
	}

	// FIFO B empty.
	set_b(&_32X_DREQ_ST, 1, 0x40);
	SH2_DMA0_Request(&M_SH2, 0);	// we use M_SH2 instead of EBP (like real 32X).
	return val;
}


/**
 * sh2_32x_reg_rw(): Word read from the 32X system registers.
 * @param adr Address.
 * @param slave 0 for MSH2, 1 for SSH2.
 * @param al_in Low byte of eax when the asm handler was called.
 *              (The PWM pulse handlers only set ah.)
 */
static uint16_t sh2_32x_reg_rw(uint32_t adr, int slave, uint8_t al_in)
{
	// The asm uses different cycle counts for MSH2 and SSH2.
	cycle_io(slave ? 14 : 16);
	adr &= 0x3E;

	switch (adr >> 1)
	{
		case 0:		// IntC
		{
			uint8_t hi = (uint8_t)(_32X_FM | (uint8_t)(_32X_ADEN + _32X_ADEN));
			uint8_t lo = (slave ? _32X_SINT : _32X_MINT);
			return (uint16_t)((hi << 8) | lo);
		}
		case 2:		// HCnt
			return _32X_HIC;
		case 3:		// DREQ
			return (uint16_t)((get_b(_32X_DREQ_ST, 1) << 8) |
					  (uint8_t)(_32X_RV | get_b(_32X_DREQ_ST, 0)));
		case 4:		// DREQ_Src_H
			return (uint16_t)(_32X_DREQ_SRC >> 16);
		case 5:		// DREQ_Src_L
			return (uint16_t)_32X_DREQ_SRC;
		case 6:		// DREQ_Dst_H
			return (uint16_t)(_32X_DREQ_DST >> 16);
		case 7:		// DREQ_Dst_L
			return (uint16_t)_32X_DREQ_DST;
		case 8:		// DREQ_Len
			return (uint16_t)_32X_DREQ_LEN;
		case 9:		// FIFO
			return sh2_rw_fifo();

		case 16: case 17: case 18: case 19:
		case 20: case 21: case 22: case 23:
			// Communication registers.
			return be16(&_32X_Comm[adr - 0x20]);

		case 24:	// PWM_Cont
			return (uint16_t)PWM_Mode;
		case 25:	// PWM_Cycle
			return (uint16_t)PWM_Cycle_Tmp;
		case 26:	// PWM_Pulse_L
		case 28:	// PWM_Pulse_C
			return (uint16_t)((_32X_PWM_Full(PWM_RP_L, PWM_WP_L) << 8) | al_in);
		case 27:	// PWM_Pulse_R
			return (uint16_t)((_32X_PWM_Full(PWM_RP_R, PWM_WP_R) << 8) | al_in);

		default:
			return 0;
	}
}


/*********************************
 * 32X VDP registers: read.      *
 *********************************/

static uint8_t sh2_vdp_reg_rb(uint32_t adr)
{
	uint8_t v;

	cycle_io(3);
	switch (adr & 0xF)
	{
		case 0x0:	return get_b(_32X_VDP.Mode, 1);
		case 0x1:	return get_b(_32X_VDP.Mode, 0);
		case 0x3:	return get_b(_32X_VDP.Mode, 2);		// Shift
		case 0x5:	return get_b(_32X_VDP.AF_Len, 0);
		case 0x6:	return get_b(_32X_VDP.AF_St, 1);
		case 0x7:	return get_b(_32X_VDP.AF_St, 0);
		case 0x8:	return get_b(_32X_VDP.AF_Data, 1);
		case 0x9:	return get_b(_32X_VDP.AF_Data, 0);
		case 0xA:	return get_b(_32X_VDP.State, 1);
		case 0xB:
			v = get_b(_32X_VDP.State, 0) ^ 2;
			set_b(&_32X_VDP.State, 0, v);
			return v;
		default:
			return 0;
	}
}

static uint16_t sh2_vdp_reg_rw(uint32_t adr)
{
	uint16_t v;

	cycle_io(5);
	switch ((adr & 0xE) >> 1)
	{
		case 0:	return (uint16_t)_32X_VDP.Mode;
		case 1:	return get_b(_32X_VDP.Mode, 2);		// Shift
		case 2:	return get_b(_32X_VDP.AF_Len, 0);
		case 3:	return (uint16_t)_32X_VDP.AF_St;
		case 4:	return (uint16_t)_32X_VDP.AF_Data;
		case 5:
			v = (uint16_t)_32X_VDP.State ^ 2;
			set_w(&_32X_VDP.State, v);
			return v;
		default:
			return 0;
	}
}


/*********************************
 * 32X system registers: write.  *
 *********************************/

/** PWM_DMA: if the FIFO is now full, deassert DREQ1 on both SH2s. **/
static void pwm_dma(unsigned int rp, unsigned int wp)
{
	if (_32X_PWM_Full(rp, wp) & 0x80)
	{
		SH2_DMA1_Request(&S_SH2, 0);
		SH2_DMA1_Request(&M_SH2, 0);
	}
}

static void pwm_pulse_l_w(uint16_t data)
{
	unsigned int rp = PWM_RP_L;
	unsigned int wp = PWM_WP_L;

	if (_32X_PWM_Full(rp, wp) & 0x80)
		return;

	PWM_FIFO_L[wp & 7] = data;
	wp = (wp + 1) & (PWM_BUF_SIZE - 1);
	PWM_WP_L = wp;
	pwm_dma(rp, wp);
}

static void pwm_pulse_r_w(uint16_t data)
{
	unsigned int rp = PWM_RP_R;
	unsigned int wp = PWM_WP_R;

	if (_32X_PWM_Full(rp, wp) & 0x80)
		return;

	PWM_FIFO_R[wp & 7] = data;
	wp = (wp + 1) & (PWM_BUF_SIZE - 1);
	PWM_WP_R = wp;
	pwm_dma(rp, wp);
}

static void pwm_pulse_c_w(uint16_t data)
{
	// Mono: uses the left channel's pointers for both FIFOs.
	unsigned int rp = PWM_RP_L;
	unsigned int wp = PWM_WP_L;

	if (_32X_PWM_Full(rp, wp) & 0x80)
		return;

	PWM_FIFO_L[wp & 7] = data;
	PWM_FIFO_R[wp & 7] = data;
	wp = (wp + 1) & (PWM_BUF_SIZE - 1);
	PWM_WP_L = wp;
	PWM_WP_R = wp;
	pwm_dma(rp, wp);
}


/**
 * sh2_32x_reg_wb(): Byte write to the 32X system registers.
 * @param adr Address.
 * @param data Data.
 * @param slave 0 for MSH2, 1 for SSH2.
 */
static void sh2_32x_reg_wb(uint32_t adr, uint8_t data, int slave)
{
	adr &= 0x3F;
	if (slave)
		_SSH2_Reg[adr] = data;
	else
		_MSH2_Reg[adr] = data;
	cycle_io(10);

	switch (adr)
	{
		case 0x00:	// IntC_H
			_32X_Write_FM(data);
			break;
		case 0x01:	// IntC_L
			if (slave)
				_32X_SINT = data;
			else
				_32X_MINT = data;
			break;
		case 0x05:	// HCnt
			_32X_HIC = data;
			break;

		case 0x30:	// PWM_Cont_H
			set_b(&PWM_Mode, 1, data);
			PWM_Set_Int(data);
			break;
		case 0x31:	// PWM_Cont_L
			set_b(&PWM_Mode, 0, data);
			break;
		case 0x32:	// PWM_Cycle_H
			set_b(&PWM_Cycle_Tmp, 1, data);
			PWM_Set_Cycle(((unsigned int)data << 8) | get_b(PWM_Cycle_Tmp, 0));
			break;
		case 0x33:	// PWM_Cycle_L
			set_b(&PWM_Cycle_Tmp, 0, data);
			PWM_Set_Cycle(((unsigned int)get_b(PWM_Cycle_Tmp, 1) << 8) | data);
			break;
		case 0x34:	// PWM_Pulse_L_H
			set_b(&PWM_FIFO_L_Tmp, 1, data);
			break;
		case 0x35:	// PWM_Pulse_L_L
			pwm_pulse_l_w((uint16_t)((get_b(PWM_FIFO_L_Tmp, 1) << 8) | data));
			break;
		case 0x36:	// PWM_Pulse_R_H
			set_b(&PWM_FIFO_R_Tmp, 1, data);
			break;
		case 0x37:	// PWM_Pulse_R_L
			pwm_pulse_r_w((uint16_t)((get_b(PWM_FIFO_R_Tmp, 1) << 8) | data));
			break;
		case 0x38:	// PWM_Pulse_C_H (uses the left channel's temp register)
			set_b(&PWM_FIFO_L_Tmp, 1, data);
			break;
		case 0x39:	// PWM_Pulse_C_L
			pwm_pulse_c_w((uint16_t)((get_b(PWM_FIFO_L_Tmp, 1) << 8) | data));
			break;

		default:
			if (adr >= 0x20 && adr < 0x30)
				_32X_Comm[adr - 0x20] = data;
			break;
	}
}


/**
 * sh2_32x_reg_ww(): Word write to the 32X system registers.
 * @param adr Address.
 * @param data Data.
 * @param slave 0 for MSH2, 1 for SSH2.
 */
static void sh2_32x_reg_ww(uint32_t adr, uint16_t data, int slave)
{
	const uint8_t dh = (uint8_t)(data >> 8);
	const uint8_t dl = (uint8_t)data;

	adr &= 0x3E;
	if (slave)
	{
		_SSH2_Reg[adr + 0] = dh;
		_SSH2_Reg[adr + 1] = dl;
	}
	else
	{
		_MSH2_Reg[adr + 0] = dh;
		_MSH2_Reg[adr + 1] = dl;
	}
	// The asm uses different cycle counts for MSH2 and SSH2.
	cycle_io(slave ? 14 : 16);

	switch (adr >> 1)
	{
		case 0:		// IntC
			if (slave)
				_32X_SINT = dl;
			else
				_32X_MINT = dl;
			_32X_Write_FM(dh);
			break;
		case 2:		// HCnt (the asm uses the byte handler: low byte only)
			_32X_HIC = dl;
			break;

		case 16: case 17: case 18: case 19:
		case 20: case 21: case 22: case 23:
			// Communication registers.
			_32X_Comm[adr - 0x20 + 0] = dh;
			_32X_Comm[adr - 0x20 + 1] = dl;
			break;

		case 24:	// PWM_Cont
			set_b(&PWM_Mode, 0, dl);
			set_b(&PWM_Mode, 1, dh);
			PWM_Set_Int(dh);
			break;
		case 25:	// PWM_Cycle (PWM_Cycle_Tmp is not updated, as in the asm)
			PWM_Set_Cycle(data);
			break;
		case 26:	// PWM_Pulse_L
			pwm_pulse_l_w(data);
			break;
		case 27:	// PWM_Pulse_R
			pwm_pulse_r_w(data);
			break;
		case 28:	// PWM_Pulse_C
			pwm_pulse_c_w(data);
			break;

		default:
			break;
	}
}


/*********************************
 * 32X VDP registers: write.     *
 *********************************/

/**
 * sh2_vdp_ww_af_data(): Auto fill.
 * Writes AF_Len + 1 words of AF_Data to the frame buffer that isn't being
 * displayed, starting at AF_St. Only the low byte of the address advances
 * (with wraparound), except that a 32-bit store at low byte 0xFF writes its
 * second word to the next 256-word line, as in the asm. (A write past the
 * end of the frame buffers is dropped.)
 */
static void sh2_vdp_ww_af_data(uint16_t data)
{
	uint32_t base, st, count;
	uint8_t lo;

	set_w(&_32X_VDP.AF_Data, data);

	base = ((_32X_VDP.State & 1) ^ 1) << 17;
	st = _32X_VDP.AF_St;
	count = get_b(_32X_VDP.AF_Len, 0) + 1;
	lo = (uint8_t)st;

#define AF_PUT(word_idx) _32X_VDP_Ram_Write_Word(base + ((uint32_t)(word_idx) * 2), data)

	if ((count >> 1) == 0)
	{
		// AF_Len == 0: one word.
		AF_PUT((st & ~0xFFU) | lo);
		lo++;
	}
	else
	{
		uint32_t loops = count >> 1;
		if (count & 1)
		{
			// Odd number of words: one word first.
			AF_PUT((st & ~0xFFU) | lo);
			lo++;
		}

		do
		{
			uint32_t idx = (st & ~0xFFU) | lo;
			AF_PUT(idx);
			AF_PUT(idx + 1);
			lo += 2;
		} while (--loops);
	}

#undef AF_PUT

	_32X_VDP.AF_St = (st & ~0xFFU) | lo;
}

static void sh2_vdp_reg_wb(uint32_t adr, uint8_t data)
{
	adr &= 0xF;
	_SH2_VDP_Reg[adr] = data;
	cycle_io(3);

	switch (adr)
	{
		case 0x1:	set_b(&_32X_VDP.Mode, 0, data);		break;
		case 0x3:	set_b(&_32X_VDP.Mode, 2, data);		break;	// Shift
		case 0x5:	set_b(&_32X_VDP.AF_Len, 0, data);	break;
		case 0xB:	_32X_Write_FB_Control(data);			break;
		default:	break;
	}
}

static void sh2_vdp_reg_ww(uint32_t adr, uint16_t data)
{
	const uint8_t dl = (uint8_t)data;

	adr &= 0xE;
	_SH2_VDP_Reg[adr + 0] = (uint8_t)(data >> 8);
	_SH2_VDP_Reg[adr + 1] = dl;
	cycle_io(5);

	// Several entries use the byte handlers (low byte only).
	switch (adr >> 1)
	{
		case 0:	set_b(&_32X_VDP.Mode, 0, dl);		break;
		case 1:	set_b(&_32X_VDP.Mode, 2, dl);		break;	// Shift
		case 2:	set_b(&_32X_VDP.AF_Len, 0, dl);		break;
		case 3:	set_w(&_32X_VDP.AF_St, data);		break;
		case 4:	sh2_vdp_ww_af_data(data);		break;
		case 5:	_32X_Write_FB_Control(dl);			break;
		default:	break;
	}
}


/*********************************
 * Region 0x00: BIOS / registers *
 *********************************/

/** "VDP" area: 0x4100-0x41FF registers, 0x4200-0x43FF palette. **/

uint8_t FASTCALL MSH2_Read_Byte_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return _32X_MSH2_Rom.u8[adr & 0x07FF];
	if (adr & 0xFFBF00)
	{
		if (!_32X_FM || (adr & 0xFFBE00))
			return 0;
		return sh2_vdp_reg_rb(adr);
	}
	return sh2_32x_reg_rb(adr, 0);
}

uint8_t FASTCALL SSH2_Read_Byte_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return _32X_SSH2_Rom.u8[adr & 0x03FF];
	if (adr & 0xFFBF00)
	{
		if (!_32X_FM || (adr & 0xFFBE00))
			return 0;
		return sh2_vdp_reg_rb(adr);
	}
	return sh2_32x_reg_rb(adr, 1);
}

/** Word read of the VDP area (registers or palette). **/
static uint16_t sh2_read_word_vdp(uint32_t adr)
{
	if (!_32X_FM)
		return 0;
	if (adr & 0xFFBE00)
	{
		// Palette.
		if (adr & 0xFFBC01)
			return 0;
		return _32X_VDP_CRam[(adr & 0x1FE) >> 1];
	}
	return sh2_vdp_reg_rw(adr);
}

uint16_t FASTCALL MSH2_Read_Word_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return be16(&_32X_MSH2_Rom.u8[adr & 0x07FE]);
	if (adr & 0xFFBF00)
		return sh2_read_word_vdp(adr);
	// The interpreter calls handlers with eax = adr >> 24.
	return sh2_32x_reg_rw(adr, 0, (uint8_t)(adr >> 24));
}

uint16_t FASTCALL SSH2_Read_Word_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return be16(&_32X_SSH2_Rom.u8[adr & 0x03FE]);
	if (adr & 0xFFBF00)
		return sh2_read_word_vdp(adr);
	// The interpreter calls handlers with eax = adr >> 24.
	return sh2_32x_reg_rw(adr, 1, (uint8_t)(adr >> 24));
}

/** Long read of the VDP area (registers or palette). **/
static uint32_t sh2_read_long_vdp(uint32_t adr)
{
	uint16_t lo, hi;

	if (!_32X_FM)
		return 0;
	if (adr & 0xFFBE00)
	{
		// Palette.
		uint32_t idx;
		if (adr & 0xFFBC03)
			return 0;
		idx = (adr & 0x1FC) >> 1;
		return ((uint32_t)_32X_VDP_CRam[idx] << 16) | _32X_VDP_CRam[idx + 1];
	}

	lo = sh2_vdp_reg_rw(adr + 2);
	hi = sh2_vdp_reg_rw(adr);
	return ((uint32_t)hi << 16) | lo;
}

/** Long read of the 32X system registers: two word reads. **/
static uint32_t sh2_read_long_32x_reg(uint32_t adr, int slave)
{
	// "should replace that by a real LONG read function !"
	// The second asm call sees eax = result of the first call.
	uint16_t lo = sh2_32x_reg_rw(adr + 2, slave, (uint8_t)(adr >> 24));
	uint16_t hi = sh2_32x_reg_rw(adr, slave, (uint8_t)lo);
	return ((uint32_t)hi << 16) | lo;
}

uint32_t FASTCALL MSH2_Read_Long_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return be32(&_32X_MSH2_Rom.u8[adr & 0x07FC]);
	if (adr & 0xFFBF00)
		return sh2_read_long_vdp(adr);
	return sh2_read_long_32x_reg(adr, 0);
}

uint32_t FASTCALL SSH2_Read_Long_00(uint32_t adr)
{
	if (!(adr & 0xFFC000))
		return be32(&_32X_SSH2_Rom.u8[adr & 0x03FC]);
	if (adr & 0xFFBF00)
		return sh2_read_long_vdp(adr);
	return sh2_read_long_32x_reg(adr, 1);
}


void FASTCALL MSH2_Write_Byte_00(uint32_t adr, uint8_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		if (!_32X_FM || (adr & 0xFFBE00))
			return;
		sh2_vdp_reg_wb(adr, data);
		return;
	}
	sh2_32x_reg_wb(adr, data, 0);
}

void FASTCALL SSH2_Write_Byte_00(uint32_t adr, uint8_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		if (!_32X_FM || (adr & 0xFFBE00))
			return;
		sh2_vdp_reg_wb(adr, data);
		return;
	}
	sh2_32x_reg_wb(adr, data, 1);
}

/** Word write to the VDP area (registers or palette). **/
static void sh2_write_word_vdp(uint32_t adr, uint16_t data)
{
	if (!_32X_FM)
		return;
	if (adr & 0xFFBE00)
	{
		// Palette.
		if (adr & 0xFFBC01)
			return;
		_32X_Write_CRam((adr & 0x1FE) >> 1, data);
		return;
	}
	sh2_vdp_reg_ww(adr, data);
}

void FASTCALL MSH2_Write_Word_00(uint32_t adr, uint16_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		sh2_write_word_vdp(adr, data);
		return;
	}
	sh2_32x_reg_ww(adr, data, 0);
}

void FASTCALL SSH2_Write_Word_00(uint32_t adr, uint16_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		sh2_write_word_vdp(adr, data);
		return;
	}
	sh2_32x_reg_ww(adr, data, 1);
}

/** Long write to the VDP area (registers or palette). **/
static void sh2_write_long_vdp(uint32_t adr, uint32_t data)
{
	if (!_32X_FM)
		return;
	if (adr & 0xFFBE00)
	{
		// Palette.
		uint32_t idx;
		if (adr & 0xFFBC03)
			return;
		idx = (adr & 0x1FC) >> 1;
		_32X_Write_CRam(idx + 0, (uint16_t)(data >> 16));
		_32X_Write_CRam(idx + 1, (uint16_t)data);
		return;
	}

	sh2_vdp_reg_ww(adr + 2, (uint16_t)data);
	sh2_vdp_reg_ww(adr, (uint16_t)(data >> 16));
}

void FASTCALL MSH2_Write_Long_00(uint32_t adr, uint32_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		sh2_write_long_vdp(adr, data);
		return;
	}
	sh2_32x_reg_ww(adr + 2, (uint16_t)data, 0);
	sh2_32x_reg_ww(adr, (uint16_t)(data >> 16), 0);
}

void FASTCALL SSH2_Write_Long_00(uint32_t adr, uint32_t data)
{
	if (!(adr & 0xFFC000))
		return;
	if (adr & 0xFFBF00)
	{
		sh2_write_long_vdp(adr, data);
		return;
	}
	sh2_32x_reg_ww(adr + 2, (uint16_t)data, 1);
	sh2_32x_reg_ww(adr, (uint16_t)(data >> 16), 1);
}


/*********************************
 * ROM (cartridge)               *
 *********************************/

uint8_t FASTCALL SH2_Read_Byte_Rom(uint32_t adr)
{
	cycle_io(6);
	return _32X_Rom.u8[adr & 0x3FFFFF];
}

uint16_t FASTCALL SH2_Read_Word_Rom(uint32_t adr)
{
	cycle_io(8);
	return be16(&_32X_Rom.u8[adr & 0x3FFFFE]);
}

uint32_t FASTCALL SH2_Read_Long_Rom(uint32_t adr)
{
	cycle_io(8);
	return be32(&_32X_Rom.u8[adr & 0x3FFFFC]);
}


/*********************************
 * Frame buffers                 *
 *********************************/

uint8_t FASTCALL SH2_Read_Byte_FB0(uint32_t adr)
{
	cycle_io(4);
	return *vdp_ram_b((adr & 0x1FFFF) ^ 1);
}

uint8_t FASTCALL SH2_Read_Byte_FB1(uint32_t adr)
{
	cycle_io(4);
	return *vdp_ram_b(((adr & 0x1FFFF) ^ 1) + 0x20000);
}

uint16_t FASTCALL SH2_Read_Word_FB0(uint32_t adr)
{
	cycle_io(5);
	return *vdp_ram_w(adr & 0x1FFFE);
}

uint16_t FASTCALL SH2_Read_Word_FB1(uint32_t adr)
{
	cycle_io(5);
	return *vdp_ram_w((adr & 0x1FFFE) + 0x20000);
}

uint32_t FASTCALL SH2_Read_Long_FB0(uint32_t adr)
{
	cycle_io(5);
	return vdp_ram_l(adr & 0x1FFFC);
}

uint32_t FASTCALL SH2_Read_Long_FB1(uint32_t adr)
{
	cycle_io(5);
	return vdp_ram_l((adr & 0x1FFFC) + 0x20000);
}


/**
 * Byte writes: 0x00 is transparent. There is no separate overwrite area
 * for byte writes (the address is masked with 0x1FFFF).
 */
void FASTCALL SH2_Write_Byte_FB0(uint32_t adr, uint8_t data)
{
	if (data)
		*vdp_ram_b((adr & 0x1FFFF) ^ 1) = data;
}

void FASTCALL SH2_Write_Byte_FB1(uint32_t adr, uint8_t data)
{
	if (data)
		*vdp_ram_b(((adr & 0x1FFFF) ^ 1) + 0x20000) = data;
}

/** Overwrite image: each nonzero host byte is written. **/
static inline void fb_overwrite_b(uint32_t host_ofs, uint8_t b)
{
	if (b)
		*vdp_ram_b(host_ofs) = b;
}

static void fb_write_word(uint32_t fb_base, uint32_t adr, uint16_t data)
{
	adr &= 0x3FFFE;
	if (!(adr & 0x20000))
	{
		*vdp_ram_w(fb_base + adr) = data;
		return;
	}

	// Overwrite image.
	fb_overwrite_b(fb_base + adr - 0x20000 + 0, (uint8_t)data);
	fb_overwrite_b(fb_base + adr - 0x20000 + 1, (uint8_t)(data >> 8));
}

void FASTCALL SH2_Write_Word_FB0(uint32_t adr, uint16_t data)
{
	fb_write_word(0, adr, data);
}

void FASTCALL SH2_Write_Word_FB1(uint32_t adr, uint16_t data)
{
	fb_write_word(0x20000, adr, data);
}

static void fb_write_long(uint32_t fb_base, uint32_t adr, uint32_t data)
{
	const uint16_t hi = (uint16_t)(data >> 16);
	const uint16_t lo = (uint16_t)data;

	adr &= 0x3FFFC;
	if (!(adr & 0x20000))
	{
		*vdp_ram_w(fb_base + adr + 0) = hi;
		*vdp_ram_w(fb_base + adr + 2) = lo;
		return;
	}

	// Overwrite image.
	fb_overwrite_b(fb_base + adr - 0x20000 + 0, (uint8_t)hi);
	fb_overwrite_b(fb_base + adr - 0x20000 + 1, (uint8_t)(hi >> 8));
	fb_overwrite_b(fb_base + adr - 0x20000 + 2, (uint8_t)lo);
	fb_overwrite_b(fb_base + adr - 0x20000 + 3, (uint8_t)(lo >> 8));
}

void FASTCALL SH2_Write_Long_FB0(uint32_t adr, uint32_t data)
{
	fb_write_long(0, adr, data);
}

void FASTCALL SH2_Write_Long_FB1(uint32_t adr, uint32_t data)
{
	fb_write_long(0x20000, adr, data);
}


/*********************************
 * SDRAM                         *
 *********************************/

uint8_t FASTCALL SH2_Read_Byte_Ram(uint32_t adr)
{
	return _32X_Ram.u8[adr & 0x3FFFF];
}

uint16_t FASTCALL SH2_Read_Word_Ram(uint32_t adr)
{
	return be16(&_32X_Ram.u8[adr & 0x3FFFE]);
}

uint32_t FASTCALL SH2_Read_Long_Ram(uint32_t adr)
{
	return be32(&_32X_Ram.u8[adr & 0x3FFFC]);
}

void FASTCALL SH2_Write_Byte_Ram(uint32_t adr, uint8_t data)
{
	_32X_Ram.u8[adr & 0x3FFFF] = data;
}

void FASTCALL SH2_Write_Word_Ram(uint32_t adr, uint16_t data)
{
	adr &= 0x3FFFE;
	_32X_Ram.u8[adr + 0] = (uint8_t)(data >> 8);
	_32X_Ram.u8[adr + 1] = (uint8_t)data;
}

void FASTCALL SH2_Write_Long_Ram(uint32_t adr, uint32_t data)
{
	adr &= 0x3FFFC;
	_32X_Ram.u8[adr + 0] = (uint8_t)(data >> 24);
	_32X_Ram.u8[adr + 1] = (uint8_t)(data >> 16);
	_32X_Ram.u8[adr + 2] = (uint8_t)(data >> 8);
	_32X_Ram.u8[adr + 3] = (uint8_t)data;
}
