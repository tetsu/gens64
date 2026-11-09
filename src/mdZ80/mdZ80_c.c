/***************************************************************************
 * mdZ80: Gens Z80 Emulator                                                *
 * mdZ80_c.c: Portable C implementation of the mdZ80 execution functions.  *
 *                                                                         *
 * Replaces mdZ80_x86.asm (32-bit x86 only) so Gens can be built for       *
 * x86_64. Instruction execution is done by the vendored z80core (MIT);    *
 * this file keeps the mdZ80 API and Z80_CONTEXT semantics.                *
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

#include "mdZ80.h"
#include "mdZ80_flags.h"
#include "z80core.h"

#include <string.h>

// Z80 flags stored separately in FXY by mdZ80.
#define FLAGS_XY (Z80_FLAG_X | Z80_FLAG_Y)

// IntLine bits.
#define INTLINE_INT	Z80_FLAG_P	// Maskable interrupt.
#define INTLINE_NMI	0x80		// Non-maskable interrupt.

/** Public variables. **/
Z80_CONTEXT M_Z80;
unsigned char mdZ80_def_mem[0x10000];

/** Instruction core. Persistent so the EI delay survives between z80_Exec() calls. **/
static zcore core;
static int core_initialized = 0;


/** Default memory handlers. **/

unsigned char FASTCALL mdZ80_def_ReadB(uint32_t address)
{
	return mdZ80_def_mem[address & 0xFFFF];
}

unsigned char FASTCALL mdZ80_def_In(uint32_t address)
{
	return mdZ80_def_mem[address & 0xFFFF];
}

unsigned short FASTCALL mdZ80_def_ReadW(uint32_t address)
{
	address &= 0xFFFF;
	return mdZ80_def_mem[address] | (mdZ80_def_mem[(address + 1) & 0xFFFF] << 8);
}

void FASTCALL mdZ80_def_WriteB(uint32_t address, unsigned char data)
{
	mdZ80_def_mem[address & 0xFFFF] = data;
}

void FASTCALL mdZ80_def_Out(uint32_t address, unsigned char data)
{
	mdZ80_def_mem[address & 0xFFFF] = data;
}

void FASTCALL mdZ80_def_WriteW(uint32_t address, unsigned short data)
{
	address &= 0xFFFF;
	mdZ80_def_mem[address] = data & 0xFF;
	mdZ80_def_mem[(address + 1) & 0xFFFF] = data >> 8;
}


/** Handler table setup. low_adr/high_adr are 256-byte page numbers. **/

#define ADD_HANDLER(name, table, type)						\
uint32_t z80_Add_##name(Z80_CONTEXT *z80, uint32_t low_adr, uint32_t high_adr, type func) \
{										\
	uint32_t i;								\
	if (low_adr > 0xFF || high_adr > 0xFF || low_adr > high_adr)		\
		return 1;							\
	for (i = low_adr; i <= high_adr; i++)					\
		z80->table[i] = func;						\
	return 0;								\
}

ADD_HANDLER(ReadB,  ReadB,  Z80_RB*)
ADD_HANDLER(ReadW,  ReadW,  Z80_RW*)
ADD_HANDLER(WriteB, WriteB, Z80_WB*)
ADD_HANDLER(WriteW, WriteW, Z80_WW*)

uint32_t z80_Add_Fetch(Z80_CONTEXT *z80, uint32_t low_adr, uint32_t high_adr, uint8_t *Region)
{
	uint32_t i;
	if (low_adr > 0xFF || high_adr > 0xFF || low_adr > high_adr)
		return 1;

	// Same as mdZ80: Fetch[page] = Region - (low_adr << 8),
	// so that Fetch[page] + address points to the byte.
	// (Only kept for compatibility; z80core fetches through ReadB.)
	for (i = low_adr; i <= high_adr; i++)
		z80->Fetch[i] = Region - (low_adr << 8);
	return 0;
}


/** z80core callbacks. **/

static uint8_t cb_read_byte(void *userdata, uint16_t address)
{
	Z80_CONTEXT *z80 = (Z80_CONTEXT*)userdata;
	return z80->ReadB[address >> 8](address);
}

static void cb_write_byte(void *userdata, uint16_t address, uint8_t data)
{
	Z80_CONTEXT *z80 = (Z80_CONTEXT*)userdata;
	z80->WriteB[address >> 8](address, data);
}

static uint8_t cb_port_in(zcore *z, uint8_t port)
{
	Z80_CONTEXT *z80 = (Z80_CONTEXT*)z->userdata;
	return z80->IN_C(port);
}

static void cb_port_out(zcore *z, uint8_t port, uint8_t data)
{
	Z80_CONTEXT *z80 = (Z80_CONTEXT*)z->userdata;
	z80->OUT_C(port, data);
}


/** Register synchronization between Z80_CONTEXT and z80core. **/

static void load_regs(Z80_CONTEXT *z80)
{
	if (!core_initialized)
	{
		zcore_init(&core);
		core_initialized = 1;
	}

	core.read_byte = cb_read_byte;
	core.write_byte = cb_write_byte;
	core.port_in = cb_port_in;
	core.port_out = cb_port_out;
	core.userdata = z80;

	core.a = z80->AF.b.A;
	zcore_set_f(&core, (z80->AF.b.F & ~FLAGS_XY) | (z80->AF.b.FXY & FLAGS_XY));
	core.b = z80->BC.b.B;
	core.c = z80->BC.b.C;
	core.d = z80->DE.b.D;
	core.e = z80->DE.b.E;
	core.h = z80->HL.b.H;
	core.l = z80->HL.b.L;

	core.a_ = z80->AF2.b.A2;
	core.f_ = (z80->AF2.b.F2 & ~FLAGS_XY) | (z80->AF2.b.FXY2 & FLAGS_XY);
	core.b_ = z80->BC2.b.B2;
	core.c_ = z80->BC2.b.C2;
	core.d_ = z80->DE2.b.D2;
	core.e_ = z80->DE2.b.E2;
	core.h_ = z80->HL2.b.H2;
	core.l_ = z80->HL2.b.L2;

	core.ix = z80->IX.w.IX;
	core.iy = z80->IY.w.IY;
	core.sp = z80->SP.w.SP;
	core.pc = (uint16_t)(z80->PC.d - z80->BasePC);

	core.i = z80->I;
	core.r = z80->R.b.R1;
	core.interrupt_mode = z80->IM;
	core.iff1 = (z80->IFF.b.IFF1 != 0);
	core.iff2 = (z80->IFF.b.IFF2 != 0);
	core.halted = ((z80->Status & Z80_STATE_HALTED) != 0);

	core.int_pending = ((z80->IntLine & INTLINE_INT) != 0);
	core.nmi_pending = ((z80->IntLine & INTLINE_NMI) != 0);
	core.int_data = z80->IntVect;
}

static void store_regs(Z80_CONTEXT *z80)
{
	uint8_t f;

	f = zcore_get_f(&core);
	z80->AF.b.A = core.a;
	z80->AF.b.F = f & ~FLAGS_XY;
	z80->AF.b.FXY = f & FLAGS_XY;
	z80->BC.w.BC = (core.b << 8) | core.c;
	z80->DE.w.DE = (core.d << 8) | core.e;
	z80->HL.w.HL = (core.h << 8) | core.l;

	z80->AF2.b.A2 = core.a_;
	z80->AF2.b.F2 = core.f_ & ~FLAGS_XY;
	z80->AF2.b.FXY2 = core.f_ & FLAGS_XY;
	z80->BC2.w.BC2 = (core.b_ << 8) | core.c_;
	z80->DE2.w.DE2 = (core.d_ << 8) | core.e_;
	z80->HL2.w.HL2 = (core.h_ << 8) | core.l_;

	z80->IX.w.IX = core.ix;
	z80->IY.w.IY = core.iy;
	z80->SP.w.SP = core.sp;
	z80->BasePC = 0;
	z80->PC.d = core.pc;

	z80->I = core.i;
	z80->R.b.R1 = core.r;
	z80->IM = core.interrupt_mode;
	z80->IFF.b.IFF1 = (core.iff1 ? Z80_FLAG_P : 0);
	z80->IFF.b.IFF2 = (core.iff2 ? Z80_FLAG_P : 0);

	if (core.halted)
		z80->Status |= Z80_STATE_HALTED;
	else
		z80->Status &= ~Z80_STATE_HALTED;

	// Interrupts stay pending until accepted, like mdZ80.
	z80->IntLine = (core.int_pending ? INTLINE_INT : 0) |
		       (core.nmi_pending ? INTLINE_NMI : 0);
}


/**
 * mdZ80_c_reset_core(): Reset internal core state that isn't in Z80_CONTEXT.
 * Called by mdZ80_reset().
 */
void mdZ80_c_reset_core(void)
{
	zcore_init(&core);
	core_initialized = 1;
}


/**
 * z80_Exec(): Run the Z80 until the odometer reaches odo.
 * @param z80 Z80 context.
 * @param odo Odometer value to reach.
 * @return 0 on success; -1 if there are no cycles to run.
 */
uint32_t FASTCALL z80_Exec(Z80_CONTEXT *z80, int odo)
{
	uint32_t need;
	unsigned long start;
	int32_t ran;

	// mdZ80: "sub edx, [CycleCnt]; jbe" (unsigned).
	if ((uint32_t)odo <= z80->CycleCnt)
		return (uint32_t)-1;
	need = (uint32_t)odo - z80->CycleCnt;

	if (z80->Status & (Z80_STATE_RUNNING | Z80_STATE_FAULTED))
		return z80->Status;

	load_regs(z80);

	// mdZ80 checks for interrupts before the first instruction.
	start = core.cyc;
	if (core.iff_delay == 0)
		zcore_process_interrupts(&core);

	z80->Status |= Z80_STATE_RUNNING;
	z80->CycleIO = 0;	// mdZ80_add_cycles() subtracts from this while running.

	for (;;)
	{
		ran = (int32_t)(core.cyc - start) - (int32_t)z80->CycleIO;
		if (ran >= (int32_t)need)
			break;

		if (core.halted && !core.nmi_pending &&
		    !(core.int_pending && core.iff1 && core.iff_delay == 0))
		{
			// Halted with nothing to wake it up: burn the rest of the cycles.
			ran = (int32_t)need;
			break;
		}

		zcore_step(&core);
	}

	z80->Status &= ~Z80_STATE_RUNNING;
	z80->CycleIO = 0;
	z80->CycleCnt += (uint32_t)ran;

	store_regs(z80);
	return 0;
}
