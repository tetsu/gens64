/***************************************************************************
 * Gens: Starscream-compatible 68000 API on top of the Musashi C core.     *
 *                                                                         *
 * Copyright (c) 2026 by the gens64 contributors                           *
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
 * This file replaces the x86-only Starscream cores (main68k_* and sub68k_*)
 * so the emulator can be built for x86_64.
 *
 * Model:
 * - Outside of exec(), the Starscream context structs (main68k_context,
 *   sub68k_context) are authoritative: the debugger, savestates and the
 *   register plugins read and write them directly.
 * - exec() loads the registers into Musashi, runs, and stores them back.
 * - Musashi has a single global CPU state; each Gens CPU keeps its own
 *   saved Musashi context and they are swapped as needed.
 * - Interrupts:
 *   - Main 68000: one level-sensitive IPL line. When an interrupt is taken,
 *     VDP_Int_Ack() supplies the new IPL (same as Gens' Starscream).
 *   - Sub 68000: standard Starscream queued interrupts (one pending bit and
 *     vector per level, cleared when taken).
 * - Odometer: absolute cycle counter, like Starscream's __odometer.
 */

#include "star_68k.h"

#include <string.h>
#include <stdlib.h>

// Musashi internals. (MUSASHI_CNF is set by the build system.)
#include "m68kcpu.h"

// Memory handlers.
#include "gens_core/mem/mem_m68k.h"
#include "gens_core/mem/mem_s68k.h"

// VDP interrupt acknowledge.
#include "gens_core/vdp/vdp_io.h"

// Starscream exec() return codes.
#define STAR_EXEC_SUCCESS	0x80000000U
#define STAR_EXEC_ALREADY_DONE	0x80000003U

typedef enum
{
	IRQ_MODEL_LEVEL = 0,	// Main 68000: level-sensitive, VDP_Int_Ack().
	IRQ_MODEL_QUEUED = 1,	// Sub 68000: Starscream queued interrupts.
} irq_model_t;

typedef struct _gens_m68k_t
{
	struct S68000CONTEXT *ctx;	// Starscream-compatible context.
	m68ki_cpu_core musashi;		// Saved Musashi state.

	// Memory handlers. (address is 24-bit)
	unsigned char  (*rb)(unsigned int address);
	unsigned short (*rw)(unsigned int address);
	void (*wb)(unsigned int address, unsigned char data);
	void (*ww)(unsigned int address, unsigned short data);

	// Main 68000 only: direct RAM at 0xE00000-0xFFFFFF (64 KB mirrored).
	int has_main_ram;

	irq_model_t irq_model;
	int irq_level;			// Current IPL line level.
	uint8_t irq_pending;		// QUEUED: bit n = level n pending.
	uint8_t irq_vector[8];		// QUEUED: vector for each level.

	int odometer;			// Cycles counted at the start of this timeslice.
	int executing;			// Inside exec().
	int quit;			// releaseTimeslice() was called.

	// Fetch region cache.
	unsigned int fetch_lo, fetch_hi;
	uintptr_t fetch_offset;
	int fetch_valid;
} gens_m68k_t;

// Starscream-compatible contexts.
struct S68000CONTEXT main68k_context;
struct S68000CONTEXT sub68k_context;

static unsigned char main_rb(unsigned int address);
static unsigned short main_rw(unsigned int address);
static void main_wb(unsigned int address, unsigned char data);
static void main_ww(unsigned int address, unsigned short data);

static gens_m68k_t cpu_main;
static gens_m68k_t cpu_sub;

// CPU whose state is currently loaded into Musashi's m68ki_cpu.
static gens_m68k_t *active_cpu = NULL;

// One-time Musashi initialization.
static int musashi_initialized = 0;


/** Main 68000 memory access (Starscream fast path for RAM). **/

static unsigned char main_rb(unsigned int address)
{
	if (address >= 0xE00000)
		return Ram_68k.u8[(address & 0xFFFF) ^ 1];
	return M68K_RB(address);
}

static unsigned short main_rw(unsigned int address)
{
	if (address >= 0xE00000)
		return Ram_68k.u16[(address & 0xFFFF) >> 1];
	return M68K_RW(address);
}

static void main_wb(unsigned int address, unsigned char data)
{
	if (address >= 0xE00000)
	{
		Ram_68k.u8[(address & 0xFFFF) ^ 1] = data;
		return;
	}
	M68K_WB(address, data);
}

static void main_ww(unsigned int address, unsigned short data)
{
	if (address >= 0xE00000)
	{
		Ram_68k.u16[(address & 0xFFFF) >> 1] = data;
		return;
	}
	M68K_WW(address, data);
}


/** CPU state management. **/

/**
 * activate(): Load a CPU's saved Musashi state into Musashi.
 * @param cpu CPU.
 * @return Previously active CPU.
 */
static gens_m68k_t *activate(gens_m68k_t *cpu)
{
	gens_m68k_t *prev = active_cpu;
	if (prev == cpu)
		return prev;

	if (prev)
		m68k_get_context(&prev->musashi);
	m68k_set_context(&cpu->musashi);
	active_cpu = cpu;
	return prev;
}


/**
 * load_regs(): Copy the Starscream context registers into Musashi.
 * The CPU must be active.
 */
static void load_regs(gens_m68k_t *cpu)
{
	const struct S68000CONTEXT *ctx = cpu->ctx;
	int i;

	// SR without stack pointer swapping or interrupt checks.
	m68ki_set_sr_noint_nosp(ctx->sr);

	for (i = 0; i < 8; i++)
	{
		REG_D[i] = ctx->dreg[i];
		REG_A[i] = ctx->areg[i];
	}

	// areg[7] is the active stack pointer; asp is the inactive one.
	if (ctx->sr & 0x2000)
		REG_USP = ctx->asp;
	else
		REG_ISP = ctx->asp;

	REG_PC = ctx->pc;
	CPU_INT_LEVEL = (cpu->irq_level & 7) << 8;

	// Invalidate the fetch cache. (Fetch maps may have been changed.)
	cpu->fetch_valid = 0;
}


/**
 * store_regs(): Copy the Musashi registers into the Starscream context.
 * The CPU must be active.
 */
static void store_regs(gens_m68k_t *cpu)
{
	struct S68000CONTEXT *ctx = cpu->ctx;
	int i;

	for (i = 0; i < 8; i++)
	{
		ctx->dreg[i] = REG_D[i];
		ctx->areg[i] = REG_A[i];
	}

	ctx->sr = (unsigned short)m68ki_get_sr();
	ctx->asp = (FLAG_S ? REG_USP : REG_ISP);
	ctx->pc = REG_PC & 0xFFFFFF;
	ctx->odometer = cpu->odometer;

	// Starscream's __interrupts.
	if (cpu->irq_model == IRQ_MODEL_LEVEL)
	{
		ctx->interrupts[0] = (unsigned char)(cpu->irq_level & 7);
		if (CPU_STOPPED)
			ctx->interrupts[0] |= 0x10;
	}
	else
	{
		ctx->interrupts[0] = cpu->irq_pending & 0xFE;
		for (i = 1; i < 8; i++)
			ctx->interrupts[i] = cpu->irq_vector[i];
	}
}


/**
 * restore_interrupts_from_ctx(): Apply Starscream's interrupt fields.
 * Used by SetContext() (e.g. savestate loading).
 */
static void restore_interrupts_from_ctx(gens_m68k_t *cpu)
{
	const struct S68000CONTEXT *ctx = cpu->ctx;
	int i;

	if (cpu->irq_model == IRQ_MODEL_LEVEL)
	{
		cpu->irq_level = ctx->interrupts[0] & 7;
	}
	else
	{
		cpu->irq_pending = ctx->interrupts[0] & 0xFE;
		cpu->irq_level = 0;
		for (i = 1; i < 8; i++)
		{
			cpu->irq_vector[i] = ctx->interrupts[i];
			if (cpu->irq_pending & (1 << i))
				cpu->irq_level = i;
		}
	}
}


/**
 * cpu_setup(): Initialize a CPU's Musashi state.
 */
static void cpu_setup(gens_m68k_t *cpu)
{
	gens_m68k_t *prev;

	if (!musashi_initialized)
	{
		// Build the opcode tables.
		memset(&m68ki_cpu, 0, sizeof(m68ki_cpu));
		m68k_init();
		musashi_initialized = 1;
	}

	// Save the active CPU, then build a fresh state for this CPU.
	prev = active_cpu;
	if (prev)
		m68k_get_context(&prev->musashi);

	memset(&m68ki_cpu, 0, sizeof(m68ki_cpu));
	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_get_context(&cpu->musashi);

	// Restore the previously-active CPU.
	if (prev)
		m68k_set_context(&prev->musashi);
	else
		active_cpu = NULL;
	if (prev == cpu)
		active_cpu = NULL;

	cpu->irq_level = 0;
	cpu->irq_pending = 0;
	memset(cpu->irq_vector, 0, sizeof(cpu->irq_vector));
	cpu->odometer = 0;
	cpu->executing = 0;
	cpu->quit = 0;
	cpu->fetch_valid = 0;
}


/** Musashi callbacks. **/

/**
 * fetch_lookup(): Find the fetch region for an address.
 * @return Host pointer to the word at the address, or NULL.
 */
static inline const uint16_t *fetch_lookup(gens_m68k_t *cpu, unsigned int address)
{
	const struct STARSCREAM_PROGRAMREGION *reg;

	if (cpu->fetch_valid &&
	    address >= cpu->fetch_lo && address <= cpu->fetch_hi)
	{
		return (const uint16_t*)(cpu->fetch_offset + address);
	}

	reg = cpu->ctx->fetch;
	if (!reg)
		return NULL;

	for (; reg->lowaddr != 0xFFFFFFFFU; reg++)
	{
		if (address >= reg->lowaddr && address <= reg->highaddr)
		{
			cpu->fetch_lo = reg->lowaddr;
			cpu->fetch_hi = reg->highaddr;
			cpu->fetch_offset = reg->offset;
			cpu->fetch_valid = 1;
			return (const uint16_t*)(reg->offset + address);
		}
	}

	return NULL;
}

static inline unsigned int fetch16(unsigned int address)
{
	gens_m68k_t *cpu = active_cpu;
	const uint16_t *p;

	address &= 0xFFFFFF;
	p = fetch_lookup(cpu, address & ~1U);
	if (p)
		return *p;

	// No fetch region. Use a data read.
	return cpu->rw(address & ~1U);
}

unsigned int m68k_read_immediate_16(unsigned int address)
{
	return fetch16(address);
}

unsigned int m68k_read_immediate_32(unsigned int address)
{
	return (fetch16(address) << 16) | fetch16(address + 2);
}

unsigned int m68k_read_memory_8(unsigned int address)
{
	return active_cpu->rb(address & 0xFFFFFF);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
	return active_cpu->rw(address & 0xFFFFFF);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
	gens_m68k_t *cpu = active_cpu;
	unsigned int hi = cpu->rw(address & 0xFFFFFF);
	unsigned int lo = cpu->rw((address + 2) & 0xFFFFFF);
	return (hi << 16) | lo;
}

unsigned int m68k_read_pcrelative_8(unsigned int address)
{
	return m68k_read_memory_8(address);
}

unsigned int m68k_read_pcrelative_16(unsigned int address)
{
	return m68k_read_memory_16(address);
}

unsigned int m68k_read_pcrelative_32(unsigned int address)
{
	return m68k_read_memory_32(address);
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{
	return m68k_read_memory_8(address);
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
	return m68k_read_memory_16(address);
}

unsigned int m68k_read_disassembler_32(unsigned int address)
{
	return m68k_read_memory_32(address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
	gens_m68k_t *cpu = active_cpu;
	cpu->wb(address & 0xFFFFFF, (unsigned char)value);
	// A write may have changed a fetch mapping (bank switching).
	cpu->fetch_valid = 0;
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
	gens_m68k_t *cpu = active_cpu;
	cpu->ww(address & 0xFFFFFF, (unsigned short)value);
	cpu->fetch_valid = 0;
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
	// High word first.
	gens_m68k_t *cpu = active_cpu;
	cpu->ww(address & 0xFFFFFF, (unsigned short)(value >> 16));
	cpu->ww((address + 2) & 0xFFFFFF, (unsigned short)value);
	cpu->fetch_valid = 0;
}

void m68k_write_memory_32_pd(unsigned int address, unsigned int value)
{
	// -(An): low word first. (Starscream writememorydecl)
	gens_m68k_t *cpu = active_cpu;
	cpu->ww((address + 2) & 0xFFFFFF, (unsigned short)value);
	cpu->ww(address & 0xFFFFFF, (unsigned short)(value >> 16));
	cpu->fetch_valid = 0;
}


/**
 * gens_m68k_int_ack(): Interrupt acknowledge callback.
 * @param int_level Interrupt level being taken.
 * @return Vector number, or M68K_INT_ACK_AUTOVECTOR.
 */
int gens_m68k_int_ack(int int_level)
{
	gens_m68k_t *cpu = active_cpu;

	if (cpu->irq_model == IRQ_MODEL_LEVEL)
	{
		// Main 68000: the VDP supplies the new IPL.
		cpu->irq_level = VDP_Int_Ack() & 7;
		CPU_INT_LEVEL = cpu->irq_level << 8;
		return M68K_INT_ACK_AUTOVECTOR;
	}
	else
	{
		// Sub 68000: Starscream queued interrupts.
		int vector, lvl;
		int_level &= 7;
		if (!(cpu->irq_pending & (1 << int_level)))
			return M68K_INT_ACK_SPURIOUS;

		vector = cpu->irq_vector[int_level];
		cpu->irq_pending &= ~(1 << int_level);

		// Next highest pending level.
		cpu->irq_level = 0;
		for (lvl = 7; lvl > 0; lvl--)
		{
			if (cpu->irq_pending & (1 << lvl))
			{
				cpu->irq_level = lvl;
				break;
			}
		}
		CPU_INT_LEVEL = cpu->irq_level << 8;
		return vector;
	}
}


/** Generic Starscream API implementation. **/

static int star_init(gens_m68k_t *cpu)
{
	cpu_setup(cpu);
	return 0;
}

static unsigned star_reset(gens_m68k_t *cpu)
{
	gens_m68k_t *prev;
	struct S68000CONTEXT *ctx = cpu->ctx;

	if (cpu->executing)
		return 1;
	if (!ctx->s_fetch)
		return 1;

	prev = activate(cpu);

	// Starscream clears all registers on reset.
	memset(REG_DA, 0, sizeof(REG_DA));
	REG_USP = 0;
	CPU_INT_LEVEL = 0;
	cpu->irq_level = 0;
	cpu->irq_pending = 0;
	cpu->fetch_valid = 0;

	m68k_pulse_reset();
	// Starscream doesn't charge reset cycles.
	RESET_CYCLES = 0;

	store_regs(cpu);

	if (prev)
		activate(prev);

	return (ctx->pc & 1) ? (unsigned)-1 : 0;
}

static unsigned star_exec(gens_m68k_t *cpu, int target)
{
	gens_m68k_t *prev;

	if (target - cpu->odometer <= 0)
		return STAR_EXEC_ALREADY_DONE;

	prev = activate(cpu);
	load_regs(cpu);

	cpu->executing = 1;
	cpu->quit = 0;

	do
	{
		int ran = m68k_execute(target - cpu->odometer);
		cpu->odometer += ran;

		// Interrupts and releaseTimeslice() end Musashi's timeslice
		// early. Interrupts are checked on the next m68k_execute().
	} while (!cpu->quit && (target - cpu->odometer) > 0);

	cpu->executing = 0;
	store_regs(cpu);

	if (prev && prev != cpu)
		activate(prev);

	return STAR_EXEC_SUCCESS;
}

static int star_interrupt(gens_m68k_t *cpu, int level, int vector)
{
	if (cpu->irq_model == IRQ_MODEL_LEVEL)
	{
		// Level-sensitive IPL line.
		cpu->irq_level = level & 7;
	}
	else
	{
		// Starscream queued interrupts.
		int lvl;
		if (level < 1 || level > 7 || vector > 255 || vector < -2)
			return 2;
		if (vector == -2)
			vector = 0x18;		// spurious
		else if (vector < 0)
			vector = 0x18 + level;	// autovector

		if (cpu->irq_pending & (1 << level))
			return 1;
		cpu->irq_pending |= (1 << level);
		cpu->irq_vector[level] = (uint8_t)vector;

		cpu->irq_level = 0;
		for (lvl = 7; lvl > 0; lvl--)
		{
			if (cpu->irq_pending & (1 << lvl))
			{
				cpu->irq_level = lvl;
				break;
			}
		}
	}

	if (active_cpu == cpu)
	{
		CPU_INT_LEVEL = cpu->irq_level << 8;
		if (cpu->executing)
		{
			// End the timeslice so the interrupt is checked
			// before the next instruction. (Starscream does the same.)
			m68k_end_timeslice();
		}
	}

	return 0;
}

static void star_flushInterrupts(gens_m68k_t *cpu)
{
	// Interrupts are processed when exec() starts.
	(void)cpu;
}

static int star_GetContextSize(void)
{
	return sizeof(struct S68000CONTEXT);
}

static void star_GetContext(gens_m68k_t *cpu, void *context)
{
	if (cpu->executing && active_cpu == cpu)
		store_regs(cpu);
	else
		cpu->ctx->odometer = cpu->odometer;
	memcpy(context, cpu->ctx, sizeof(struct S68000CONTEXT));
}

static void star_SetContext(gens_m68k_t *cpu, void *context)
{
	if (context != cpu->ctx)
		memcpy(cpu->ctx, context, sizeof(struct S68000CONTEXT));
	restore_interrupts_from_ctx(cpu);
	cpu->odometer = cpu->ctx->odometer;
	cpu->fetch_valid = 0;

	// Musashi's stopped state.
	if (cpu->irq_model == IRQ_MODEL_LEVEL)
	{
		gens_m68k_t *prev = activate(cpu);
		CPU_STOPPED = (cpu->ctx->interrupts[0] & 0x10) ? STOP_LEVEL_STOP : 0;
		if (prev)
			activate(prev);
	}
}

static int star_fetch(gens_m68k_t *cpu, unsigned address)
{
	const struct STARSCREAM_PROGRAMREGION *reg = cpu->ctx->s_fetch;
	address &= 0xFFFFFF;
	if (!reg)
		return -1;
	for (; reg->lowaddr != 0xFFFFFFFFU; reg++)
	{
		if (address >= reg->lowaddr && address <= reg->highaddr)
			return *(const uint16_t*)(reg->offset + (address & ~1U));
	}
	return -1;
}

static unsigned star_readOdometer(gens_m68k_t *cpu)
{
	if (cpu->executing && active_cpu == cpu)
		return cpu->odometer + m68k_cycles_run();
	return cpu->odometer;
}

static unsigned star_tripOdometer(gens_m68k_t *cpu)
{
	unsigned odo = star_readOdometer(cpu);
	if (cpu->executing && active_cpu == cpu)
		cpu->odometer = -m68k_cycles_run();
	else
		cpu->odometer = 0;
	return odo;
}

static unsigned star_controlOdometer(gens_m68k_t *cpu, int n)
{
	return (n ? star_tripOdometer(cpu) : star_readOdometer(cpu));
}

static void star_releaseTimeslice(gens_m68k_t *cpu)
{
	if (cpu->executing && active_cpu == cpu)
	{
		cpu->quit = 1;
		m68k_end_timeslice();
	}
}

static void star_releaseCycles(gens_m68k_t *cpu, int cycles)
{
	if (cpu->executing && active_cpu == cpu)
		USE_CYCLES(cycles);
	else
		cpu->odometer += cycles;
}

static void star_addCycles(gens_m68k_t *cpu, int cycles)
{
	cpu->odometer += cycles;
}

static unsigned star_readPC(gens_m68k_t *cpu)
{
	if (cpu->executing && active_cpu == cpu)
		return REG_PC & 0xFFFFFF;
	return cpu->ctx->pc;
}


/** Public API: main68k_* and sub68k_* **/

#define STAR_API(SN, CPU)							\
int SN##init(void)								\
{										\
	return star_init(&CPU);							\
}										\
unsigned SN##reset(void)							\
{										\
	return star_reset(&CPU);						\
}										\
unsigned SN##exec(int n)							\
{										\
	return star_exec(&CPU, n);						\
}										\
int SN##interrupt(int level, int vector)					\
{										\
	return star_interrupt(&CPU, level, vector);				\
}										\
void SN##flushInterrupts(void)							\
{										\
	star_flushInterrupts(&CPU);						\
}										\
int SN##GetContextSize(void)							\
{										\
	return star_GetContextSize();						\
}										\
void SN##GetContext(void *context)						\
{										\
	star_GetContext(&CPU, context);						\
}										\
void SN##SetContext(void *context)						\
{										\
	star_SetContext(&CPU, context);						\
}										\
int SN##fetch(unsigned address)							\
{										\
	return star_fetch(&CPU, address);					\
}										\
unsigned SN##readOdometer(void)							\
{										\
	return star_readOdometer(&CPU);						\
}										\
unsigned SN##tripOdometer(void)							\
{										\
	return star_tripOdometer(&CPU);						\
}										\
unsigned SN##controlOdometer(int n)						\
{										\
	return star_controlOdometer(&CPU, n);					\
}										\
void SN##releaseTimeslice(void)							\
{										\
	star_releaseTimeslice(&CPU);						\
}										\
void SN##releaseCycles(int cycles)						\
{										\
	star_releaseCycles(&CPU, cycles);					\
}										\
void SN##addCycles(int cycles)							\
{										\
	star_addCycles(&CPU, cycles);						\
}										\
unsigned SN##readPC(void)							\
{										\
	return star_readPC(&CPU);						\
}

STAR_API(main68k_, cpu_main)
STAR_API(sub68k_, cpu_sub)


/** CPU descriptors. (Musashi state is set up by init().) **/
static gens_m68k_t cpu_main =
{
	.ctx = &main68k_context,
	.rb = main_rb,
	.rw = main_rw,
	.wb = main_wb,
	.ww = main_ww,
	.has_main_ram = 1,
	.irq_model = IRQ_MODEL_LEVEL,
};

static gens_m68k_t cpu_sub =
{
	.ctx = &sub68k_context,
	.rb = S68K_RB,
	.rw = S68K_RW,
	.wb = S68K_WB,
	.ww = S68K_WW,
	.has_main_ram = 0,
	.irq_model = IRQ_MODEL_QUEUED,
};
