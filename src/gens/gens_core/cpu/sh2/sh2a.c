/****************************************************************************
 * SH2 emulator 1.60 (C source part, emulation core)                        *
 * Copyright 2002 Stéphane Dallongeville                                    *
 * Used for the 32X emulation in Gens                                       *
 *                                                                          *
 * C port for 64-bit builds: Copyright (c) 2026 by Tetsuro Mori            *
 *                                                                          *
 * This is a faithful C port of sh2a.asm (the x86 SH2 interpreter).         *
 * Register mapping used throughout this file:                              *
 *   ebp -> sh2 / SH2_Cur   (SH2 context pointer)                           *
 *   esi -> esi             (host pointer to PC: Base_PC + SH2 address)     *
 *   edi -> edi             (cycle counter, signed)                         *
 *   ebx -> opc             (current opcode word, as read from host memory: *
 *                           SH2 memory is stored big-endian, so this is    *
 *                           the byte-swapped opcode, like in the asm)      *
 * Instructions are fetched from PC - 4, like on a real SH2 pipeline.       *
 *                                                                          *
 * C core context convention (see sh2.h): SH2_CONTEXT.PC and .DS_PC are SH2 *
 * addresses (the asm stores host pointers there). Base_PC / Fetch_Start / *
 * Fetch_End are the fetch cache for the region containing PC.             *
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sh2.h"


/* Constants (sh2a.asm) */
/* SH2_SPEED is REALIST: the middle cycle count of the asm RET macros is used. */
#define SH2_NOP_CODE	0x0900

#define SH2_RUNNING	0x01
#define SH2_HALTED	0x02
#define SH2_DISABLE	0x04
#define SH2_FAULTED	0x10


/* Data (sh2a.asm .bss) */
void *OP_Table[0x10000] __attribute__((aligned(64)));
void *OPDS_Table[0x10000] __attribute__((aligned(64)));
uint32_t Set_SR_Table[0x400] __attribute__((aligned(64)));
SH2_CONTEXT M_SH2 __attribute__((aligned(64)));
SH2_CONTEXT S_SH2 __attribute__((aligned(64)));

/* Current SH2 context (the asm's ebp). */
SH2_CONTEXT *SH2_Cur = &M_SH2;


/* Interpreter state (the asm keeps these in registers). */
typedef void (*SH2_OP)(void);

enum
{
	ST_RUN = 0,		// keep dispatching
	ST_QUIT,		// cycle counter went negative: SH2_Exec_Quit
	ST_REALLY_QUIT		// SH2_Exec_Really_Quit
};

static SH2_CONTEXT *sh2 = &M_SH2;	// ebp
static uintptr_t esi;			// PC (host pointer)
static int32_t edi;			// cycle counter
static uint32_t opc;			// ebx: current opcode word
static uintptr_t ds_pc;			// SH2.DS_PC (host pointer) while executing
static int st;				// interpreter state


/************************************************************
 * Helpers                                                  *
 ************************************************************/

/** Read a 16-bit host word (instruction fetch, "movzx ebx, word [x]"). **/
static inline uint32_t fetch16(uintptr_t host)
{
	uint16_t w;
	memcpy(&w, (const void*)host, sizeof(w));
	return w;
}

/**
 * region_base(): Host base for a fetch region, such that
 * host address = base + SH2 address.
 * sh2.c stores Fetch_Reg = &Fetch[0 - (low_adr >> 1)], i.e. Fetch plus an
 * UNSIGNED 32-bit element index. On a 32-bit host that wraps to the
 * intended "Fetch - low_adr" bias (what the asm uses directly), but on a
 * 64-bit host it does not. Undo sh2.c's index exactly and rebuild the bias
 * with pointer-width arithmetic. (Correct on both 32-bit and 64-bit hosts.)
 */
static inline uintptr_t region_base(const FETCHREG *r)
{
	const uint32_t idx = 0u - (r->Low_Adr >> 1);
	const uintptr_t fetch = (uintptr_t)r->Fetch_Reg - ((uintptr_t)idx * 2u);
	return fetch - ((uintptr_t)(r->Low_Adr >> 1) * 2u);
}

/**
 * fetch_search(): The asm REBASE_PC search loop.
 * @return 1 if found (Fetch_Start/Fetch_End/Base_PC updated);
 *         0 on error (the SH2 is marked faulted, Base_PC = Fetch_End = 0).
 */
static int fetch_search(SH2_CONTEXT *c, uint32_t pc)
{
	const FETCHREG *r = c->Fetch_Region;
	unsigned int i;

	for (i = 0; i < 0x100; i++, r++)
	{
		if (pc >= r->Low_Adr && pc <= r->High_Adr)
		{
			c->Fetch_Start = r->Low_Adr;
			c->Fetch_End = r->High_Adr;
			c->Base_PC = region_base(r);
			return 1;
		}

		// The list is terminated by a region with Fetch_Reg == -1.
		if ((uintptr_t)r->Fetch_Reg == (uintptr_t)-1)
			break;
	}

	// Error: asm stores the terminating Fetch_Reg (-1) in Fetch_Start.
	c->Fetch_Start = 0xFFFFFFFFu;
	c->Status |= SH2_FAULTED;
	c->Base_PC = 0;
	c->Fetch_End = 0;
	return 0;
}

/**
 * fast_rebase(): FAST_REBASE_PC macro. pc = unbased PC.
 * On success, esi = based PC. On error, jumps to SH2_Exec_Really_Quit
 * (esi = unbased PC, edi = -1).
 */
static int fast_rebase(uint32_t pc)
{
	if (pc < (uint32_t)sh2->Fetch_Start || pc > (uint32_t)sh2->Fetch_End)
	{
		if (!fetch_search(sh2, pc))
		{
			esi = pc;
			edi = -1;
			st = ST_REALLY_QUIT;
			return 0;
		}
	}

	esi = sh2->Base_PC + pc;
	return 1;
}

/** GET_SR macro. **/
static inline uint32_t get_sr(const SH2_CONTEXT *c)
{
	uint8_t lo = (uint8_t)(c->SR.T | (uint8_t)(c->SR.IMask << 4) | (uint8_t)(c->SR.S + c->SR.S));
	return ((uint32_t)c->SR.MQ << 8) | lo;
}

/** "mov [ebp + SH2.SR], eax" with a packed Set_SR_Table value. **/
static inline void set_sr_packed(SH2_CONTEXT *c, uint32_t v)
{
	c->SR.T = (uint8_t)v;
	c->SR.S = (uint8_t)(v >> 8);
	c->SR.IMask = (uint8_t)(v >> 16);
	c->SR.MQ = (uint8_t)(v >> 24);
}


/* Memory access (READ_* / WRITE_* macros): the cycle counter is passed to
 * the handlers through SH2.Cycle_IO and reloaded afterwards. */

static inline uint32_t mem_rb(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr)
{
	uint8_t v;
	c->Cycle_IO = (uint32_t)*cyc;
	v = c->Read_Byte[adr >> 24](adr);
	*cyc = (int32_t)c->Cycle_IO;
	return v;
}

static inline uint32_t mem_rw(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr)
{
	uint16_t v;
	c->Cycle_IO = (uint32_t)*cyc;
	v = c->Read_Word[adr >> 24](adr);
	*cyc = (int32_t)c->Cycle_IO;
	return v;
}

static inline uint32_t mem_rl(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr)
{
	uint32_t v;
	c->Cycle_IO = (uint32_t)*cyc;
	v = c->Read_Long[adr >> 24](adr);
	*cyc = (int32_t)c->Cycle_IO;
	return v;
}

static inline void mem_wb(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr, uint32_t data)
{
	c->Cycle_IO = (uint32_t)*cyc;
	c->Write_Byte[adr >> 24](adr, (uint8_t)data);
	*cyc = (int32_t)c->Cycle_IO;
}

static inline void mem_ww(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr, uint32_t data)
{
	c->Cycle_IO = (uint32_t)*cyc;
	c->Write_Word[adr >> 24](adr, (uint16_t)data);
	*cyc = (int32_t)c->Cycle_IO;
}

static inline void mem_wl(SH2_CONTEXT *c, int32_t *cyc, uint32_t adr, uint32_t data)
{
	c->Cycle_IO = (uint32_t)*cyc;
	c->Write_Long[adr >> 24](adr, data);
	*cyc = (int32_t)c->Cycle_IO;
}

#define READ_BYTE(a)		mem_rb(sh2, &edi, (a))
#define READ_WORD(a)		mem_rw(sh2, &edi, (a))
#define READ_LONG(a)		mem_rl(sh2, &edi, (a))
#define WRITE_BYTE(a, d)	mem_wb(sh2, &edi, (a), (d))
#define WRITE_WORD(a, d)	mem_ww(sh2, &edi, (a), (d))
#define WRITE_LONG(a, d)	mem_wl(sh2, &edi, (a), (d))


/* Instruction end (RET / RET_DS macros, REALIST cycles). */

#define SUB_CYCLES(c)	(edi = (int32_t)((uint32_t)edi - (uint32_t)(c)))

#define RET(c) \
	do { \
		esi += 2; \
		SUB_CYCLES(c); \
		if (edi < 0) \
			st = ST_QUIT; \
	} while (0)

#define RET_DS(c) \
	do { \
		esi = ds_pc; \
		SUB_CYCLES(c); \
		if (edi < 0) \
			st = ST_QUIT; \
	} while (0)

/** GO_DS macro: execute the delay slot instruction at ecx - 4. **/
static void go_ds(uintptr_t ecx, int cycles)
{
	opc = fetch16(ecx - 4);
	SUB_CYCLES(cycles);
	ds_pc = esi;
	esi = ecx;
	((SH2_OP)OPDS_Table[opc])();
}


/* Instruction formats (opc is the byte-swapped opcode). */
#define OP_N	(opc & 0x0F)			// xxxx nnnn xxxx xxxx
#define OP_M	((opc >> 12) & 0x0F)		// xxxx xxxx mmmm xxxx
#define OP_I8	((opc >> 8) & 0xFF)		// xxxx xxxx iiii iiii (unsigned)
#define OP_S8	((int32_t)(int8_t)(opc >> 8))	// sign-extended 8-bit immediate/displacement
#define OP_D4	((opc >> 8) & 0x0F)		// xxxx xxxx xxxx dddd
#define RG	sh2->R
#define T_BIT	(sh2->SR.T & 1)

/** Sign-extended 12-bit displacement (BRA/BSR: "ror ax, 4; movsx; sar 4"). **/
static inline int32_t disp12(void)
{
	uint32_t d = ((opc & 0x0F) << 8) | ((opc >> 8) & 0xFF);
	return ((int32_t)(d << 20)) >> 20;
}

/** esi + disp*2 + 4 ("lea esi, [esi + eax * 2 + 4]") **/
#define BRANCH_TARGET(d)	(esi + (uintptr_t)(intptr_t)((int32_t)(d) * 2 + 4))


/* Instruction definition helpers: most instructions have an identical
 * delay-slot version that only differs by RET -> RET_DS. */

#define DEF_OP(name, cyc) \
	static inline void body_##name(void); \
	void SH2I_##name(void) { body_##name(); RET(cyc); } \
	void SH2I_##name##_DS(void) { body_##name(); RET_DS(cyc); } \
	static inline void body_##name(void)


/************************************************************
 * Exception processing (Do_Exception / CHECK_INT)          *
 ************************************************************/

/**
 * do_exception(): Do_Exception.
 * @param vect Interrupt vector (bl).
 * @param level Interrupt level (bh).
 * @param r15 R15 value (ecx).
 */
static void do_exception(uint8_t vect, uint8_t level, uint32_t r15)
{
	uint32_t sr = get_sr(sh2);
	uint32_t sp, pc;

	if (level > 0xF)
		level = 0xF;
	sh2->SR.IMask = level;

	sp = r15 - 8;
	sh2->R[15] = sp;
	WRITE_LONG(sp + 4, sr & 0x3F3);
	pc = (uint32_t)(esi - 4 - sh2->Base_PC);
	WRITE_LONG(sh2->R[15], pc);

	sh2->Status &= ~(uint32_t)SH2_HALTED;
	pc = READ_LONG(sh2->VBR + ((uint32_t)vect * 4));
	if (!fast_rebase(pc + 4))
		return;
	SUB_CYCLES(12);
}

/** CHECK_INT macro. **/
static void check_int(void)
{
	const uint8_t imask = sh2->SR.IMask;
	const uint8_t prio = sh2->INT.Prio;
	const uint8_t vect = sh2->INT.Vect;
	uint32_t r15;
	int32_t k;
	uint8_t al = 0;

	if (prio <= imask)
		return;

	k = prio & 0x1F;
	r15 = sh2->R[15];
	sh2->INT_QUEUE[k] = 0;

	for (k = k - 1; k >= 0; k--)
	{
		al = sh2->INT_QUEUE[k];		// INT_QUEUE[x] = Vector X
		if (al)
			break;
	}

	if (k < 0)
	{
		// No other pending interrupt. ("inc dl" -> 0)
		k = 0;
		al = 0;
	}

	sh2->INT.Vect = al;
	sh2->INT.Prio = (uint8_t)k;
	do_exception(vect, prio, r15);
}


/************************************************************
 * SH2 instructions                                         *
 ************************************************************/

DEF_OP(ADD, 1)	{ RG[OP_N] = RG[OP_M] + RG[OP_N]; }
DEF_OP(ADDI, 1)	{ RG[OP_N] = (uint32_t)OP_S8 + RG[OP_N]; }

DEF_OP(ADDC, 1)
{
	const uint32_t n = OP_N;
	uint64_t sum = (uint64_t)RG[OP_M] + RG[n] + T_BIT;
	sh2->SR.T = (uint8_t)(sum >> 32);
	RG[n] = (uint32_t)sum;
}

DEF_OP(ADDV, 1)
{
	const uint32_t n = OP_N;
	const uint32_t a = RG[OP_M], b = RG[n];
	const uint32_t r = a + b;
	sh2->SR.T = (uint8_t)(((~(a ^ b)) & (a ^ r)) >> 31);
	RG[n] = r;
}

DEF_OP(AND, 1)	{ RG[OP_N] = RG[OP_M] & RG[OP_N]; }
DEF_OP(ANDI, 1)	{ RG[0] = OP_I8 & RG[0]; }

DEF_OP(ANDM, 5)
{
	const uint32_t adr = RG[0] + sh2->GBR;
	const uint32_t imm = OP_I8;
	uint32_t v = READ_BYTE(adr);
	WRITE_BYTE(adr, v & imm);
}

DEF_OP(CLRMAC, 1)	{ sh2->MACH = 0; sh2->MACL = 0; }
DEF_OP(CLRT, 1)		{ sh2->SR.T = 0; }

DEF_OP(CMPEQ, 1)	{ sh2->SR.T = (RG[OP_N] == RG[OP_M]); }
DEF_OP(CMPGE, 1)	{ sh2->SR.T = ((int32_t)RG[OP_N] >= (int32_t)RG[OP_M]); }
DEF_OP(CMPGT, 1)	{ sh2->SR.T = ((int32_t)RG[OP_N] > (int32_t)RG[OP_M]); }
DEF_OP(CMPHI, 1)	{ sh2->SR.T = (RG[OP_N] > RG[OP_M]); }
DEF_OP(CMPHS, 1)	{ sh2->SR.T = (RG[OP_N] >= RG[OP_M]); }
DEF_OP(CMPPL, 1)	{ sh2->SR.T = ((int32_t)RG[OP_N] > 0); }
DEF_OP(CMPPZ, 1)	{ sh2->SR.T = ((int32_t)RG[OP_N] >= 0); }

/* NOTE: the asm sets T = 0 only if ALL four bytes are equal (the
 * real CMP/STR sets T = 1 if ANY byte is equal). Kept as-is. */
DEF_OP(CMPSTR, 2)	{ sh2->SR.T = ((RG[OP_N] ^ RG[OP_M]) != 0); }

DEF_OP(CMPIM, 1)	{ sh2->SR.T = ((uint32_t)OP_S8 == RG[0]); }

DEF_OP(DIV0S, 1)
{
	const uint32_t m = RG[OP_M] >> 31;	// M = sign of Rm
	const uint32_t q = RG[OP_N] >> 31;	// Q = sign of Rn
	sh2->SR.MQ = (uint8_t)((m << 1) | q);
	sh2->SR.T = (uint8_t)(m ^ q);
}

DEF_OP(DIV0U, 1)	{ sh2->SR.T = 0; sh2->SR.MQ = 0; }

DEF_OP(DIV1, 1)
{
	const uint32_t n = OP_N;
	const uint32_t rm = RG[OP_M];
	const uint32_t old = (RG[n] << 1) | T_BIT;	// adc ecx, ecx
	const uint32_t idx = (((uint32_t)sh2->SR.MQ << 1) | (RG[n] >> 31)) & 7;	// M | old Q | Q
	uint32_t res, q, t;

	switch (idx)
	{
		default:
		case 0:	res = old - rm; q = (old < res);  t = q ^ 1; break;	// .D0M0Q0
		case 1:	res = old - rm; q = (old >= res); t = q ^ 1; break;	// .D0M0Q1
		case 2:	res = old + rm; q = (old > res);  t = q ^ 1; break;	// .D1M0Q0
		case 3:	res = old + rm; q = (old <= res); t = q ^ 1; break;	// .D1M0Q1
		case 4:	res = old + rm; q = (old <= res); t = q;     break;	// .D0M1Q0
		case 5:	res = old + rm; q = (old > res);  t = q;     break;	// .D0M1Q1
		case 6:	res = old - rm; q = (old >= res); t = q;     break;	// .D1M1Q0
		case 7:	res = old - rm; q = (old < res);  t = q;     break;	// .D1M1Q1
	}

	RG[n] = res;
	sh2->SR.MQ = (uint8_t)((sh2->SR.MQ & 2) | q);
	sh2->SR.T = (uint8_t)t;
}

DEF_OP(DMULS, 4)
{
	const int64_t p = (int64_t)(int32_t)RG[OP_M] * (int64_t)(int32_t)RG[OP_N];
	sh2->MACL = (uint32_t)p;
	sh2->MACH = (uint32_t)((uint64_t)p >> 32);
}

DEF_OP(DMULU, 4)
{
	const uint64_t p = (uint64_t)RG[OP_M] * (uint64_t)RG[OP_N];
	sh2->MACL = (uint32_t)p;
	sh2->MACH = (uint32_t)(p >> 32);
}

DEF_OP(DT, 1)
{
	const uint32_t n = OP_N;
	const uint32_t r = RG[n] - 1;
	sh2->SR.T = (r == 0);
	RG[n] = r;
}

DEF_OP(EXTSB, 1)	{ RG[OP_N] = (uint32_t)(int32_t)(int8_t)RG[OP_M]; }
DEF_OP(EXTSW, 1)	{ RG[OP_N] = (uint32_t)(int32_t)(int16_t)RG[OP_M]; }
DEF_OP(EXTUB, 1)	{ RG[OP_N] = (uint8_t)RG[OP_M]; }
DEF_OP(EXTUW, 1)	{ RG[OP_N] = (uint16_t)RG[OP_M]; }

DEF_OP(LDCSR, 2)
{
	uint32_t v;
	sh2->Cycle_Sup = (uint32_t)edi;
	v = RG[OP_N] & 0x3F3;
	edi = 0;					// Force Interrupt Check
	set_sr_packed(sh2, Set_SR_Table[v]);
}

DEF_OP(LDCGBR, 1)	{ sh2->GBR = RG[OP_N]; }
DEF_OP(LDCVBR, 1)	{ sh2->VBR = RG[OP_N]; }

DEF_OP(LDCMSR, 5)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	uint32_t v;
	sh2->Cycle_Sup = (uint32_t)edi;
	RG[n] = adr + 4;
	v = READ_LONG(adr) & 0x3F3;
	edi = 0;					// Force Interrupt Check
	set_sr_packed(sh2, Set_SR_Table[v]);
}

DEF_OP(LDCMGBR, 4)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	RG[n] = adr + 4;
	sh2->GBR = READ_LONG(adr);
}

DEF_OP(LDCMVBR, 4)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	RG[n] = adr + 4;
	sh2->VBR = READ_LONG(adr);
}

DEF_OP(LDSMACH, 1)	{ sh2->MACH = RG[OP_N]; }
DEF_OP(LDSMACL, 1)	{ sh2->MACL = RG[OP_N]; }
DEF_OP(LDSPR, 1)	{ sh2->PR = RG[OP_N]; }

DEF_OP(LDSMMACH, 3)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	RG[n] = adr + 4;
	sh2->MACH = READ_LONG(adr);
}

DEF_OP(LDSMMACL, 3)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	RG[n] = adr + 4;
	sh2->MACL = READ_LONG(adr);
}

DEF_OP(LDSMPR, 3)
{
	const uint32_t n = OP_N;
	const uint32_t adr = RG[n];
	RG[n] = adr + 4;
	sh2->PR = READ_LONG(adr);
}

/** MAC.L @Rm+, @Rn+ **/
DEF_OP(MACL, 5)
{
	const uint32_t n = OP_N, m = OP_M;
	uint32_t adr, a, b, lo, hi, old, carry, sum_h;
	int64_t p;

	adr = RG[m] + 4;
	RG[m] = adr;
	a = READ_LONG(adr - 4);
	adr = RG[n];
	RG[n] = adr + 4;
	b = READ_LONG(adr);

	p = (int64_t)(int32_t)b * (int64_t)(int32_t)a;
	lo = (uint32_t)p;
	hi = (uint32_t)((uint64_t)p >> 32);

	old = sh2->MACL;
	sh2->MACL = old + lo;
	carry = (sh2->MACL < old);
	sum_h = sh2->MACH + hi + carry;

	if (!sh2->SR.S)
	{
		sh2->MACH = sum_h;
		return;
	}

	// Saturation (48-bit).
	if ((int32_t)sum_h < (int32_t)0xFFFF8000)
	{
		sh2->MACH = 0xFFFF8000;
		sh2->MACL = 0x00000000;
	}
	else if ((int32_t)sum_h > 0x00007FFF)
	{
		sh2->MACH = 0x00007FFF;
		sh2->MACL = 0xFFFFFFFF;
	}
	else
	{
		sh2->MACH = sum_h;
	}
}

/** MAC.W @Rm+, @Rn+ **/
DEF_OP(MACW, 4)
{
	const uint32_t n = OP_N, m = OP_M;
	uint32_t adr, lo, hi, old, carry, sum_h;
	int32_t a, b;
	int64_t p;

	adr = RG[m] + 2;
	RG[m] = adr;
	a = (int16_t)READ_WORD(adr - 2);
	adr = RG[n] + 2;
	RG[n] = adr;
	b = (int16_t)READ_WORD(adr - 2);

	p = (int64_t)b * (int64_t)a;
	lo = (uint32_t)p;
	hi = (uint32_t)((uint64_t)p >> 32);

	old = sh2->MACL;
	sh2->MACL = old + lo;
	carry = (sh2->MACL < old);
	sum_h = sh2->MACH + hi + carry;

	if (!sh2->SR.S)
	{
		sh2->MACH = sum_h;
		return;
	}

	// Saturation (32-bit), as done by the asm.
	if ((int32_t)sum_h < -1)
	{
		sh2->MACH = 0xFFFFFFFF;
		sh2->MACL = 0x80000000;
	}
	else if ((int32_t)sum_h > 0)
	{
		sh2->MACH = 0x00000000;
		sh2->MACL = 0x7FFFFFFF;
	}
	else
	{
		sh2->MACH = sum_h;
	}
}

DEF_OP(MOV, 1)		{ RG[OP_N] = RG[OP_M]; }

DEF_OP(MOVBS, 2)	{ WRITE_BYTE(RG[OP_N], RG[OP_M]); }
DEF_OP(MOVWS, 2)	{ WRITE_WORD(RG[OP_N], RG[OP_M]); }
DEF_OP(MOVLS, 2)	{ WRITE_LONG(RG[OP_N], RG[OP_M]); }

DEF_OP(MOVBL, 2)
{
	const uint32_t n = OP_N;
	RG[n] = (uint32_t)(int32_t)(int8_t)READ_BYTE(RG[OP_M]);
}

DEF_OP(MOVWL, 2)
{
	const uint32_t n = OP_N;
	RG[n] = (uint32_t)(int32_t)(int16_t)READ_WORD(RG[OP_M]);
}

DEF_OP(MOVLL, 2)
{
	const uint32_t n = OP_N;
	RG[n] = READ_LONG(RG[OP_M]);
}

DEF_OP(MOVBM, 2)
{
	const uint32_t n = OP_N;
	const uint32_t data = RG[OP_M];
	const uint32_t adr = RG[n] - 1;
	RG[n] = adr;
	WRITE_BYTE(adr, data);
}

DEF_OP(MOVWM, 2)
{
	const uint32_t n = OP_N;
	const uint32_t data = RG[OP_M];
	const uint32_t adr = RG[n] - 2;
	RG[n] = adr;
	WRITE_WORD(adr, data);
}

DEF_OP(MOVLM, 2)
{
	const uint32_t n = OP_N;
	const uint32_t data = RG[OP_M];
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(MOVBP, 2)
{
	const uint32_t n = OP_N, m = OP_M;
	const uint32_t adr = RG[m] + 1;
	if (m != n)
		RG[m] = adr;
	RG[n] = (uint32_t)(int32_t)(int8_t)READ_BYTE(adr - 1);
}

DEF_OP(MOVWP, 2)
{
	const uint32_t n = OP_N, m = OP_M;
	const uint32_t adr = RG[m] + 2;
	if (m != n)
		RG[m] = adr;
	RG[n] = (uint32_t)(int32_t)(int16_t)READ_WORD(adr - 2);
}

DEF_OP(MOVLP, 2)
{
	const uint32_t n = OP_N, m = OP_M;
	const uint32_t adr = RG[m] + 4;
	if (m != n)
		RG[m] = adr;
	RG[n] = READ_LONG(adr - 4);
}

DEF_OP(MOVBS0, 2)	{ WRITE_BYTE(RG[OP_N] + RG[0], RG[OP_M]); }
DEF_OP(MOVWS0, 2)	{ WRITE_WORD(RG[OP_N] + RG[0], RG[OP_M]); }
DEF_OP(MOVLS0, 2)	{ WRITE_LONG(RG[OP_N] + RG[0], RG[OP_M]); }

DEF_OP(MOVBL0, 2)
{
	const uint32_t n = OP_N;
	RG[n] = (uint32_t)(int32_t)(int8_t)READ_BYTE(RG[OP_M] + RG[0]);
}

DEF_OP(MOVWL0, 2)
{
	const uint32_t n = OP_N;
	RG[n] = (uint32_t)(int32_t)(int16_t)READ_WORD(RG[OP_M] + RG[0]);
}

DEF_OP(MOVLL0, 2)
{
	const uint32_t n = OP_N;
	RG[n] = READ_LONG(RG[OP_M] + RG[0]);
}

DEF_OP(MOVI, 1)		{ RG[OP_N] = (uint32_t)OP_S8; }

/** MOV.W @(disp,PC), Rn: read directly from the fetch memory. **/
DEF_OP(MOVWI, 2)
{
	const uint8_t *p = (const uint8_t*)(esi + (uintptr_t)OP_I8 * 2);
	RG[OP_N] = (uint32_t)(int32_t)(int16_t)(uint16_t)((p[0] << 8) | p[1]);
}

/** MOV.L @(disp,PC), Rn: read directly from the fetch memory. **/
DEF_OP(MOVLI, 2)
{
	const uint8_t *p = (const uint8_t*)((esi & ~(uintptr_t)3) + (uintptr_t)OP_I8 * 4);
	RG[OP_N] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

DEF_OP(MOVBLG, 2)	{ RG[0] = (uint32_t)(int32_t)(int8_t)READ_BYTE(sh2->GBR + OP_I8); }
DEF_OP(MOVWLG, 2)	{ RG[0] = (uint32_t)(int32_t)(int16_t)READ_WORD(sh2->GBR + OP_I8 * 2); }
DEF_OP(MOVLLG, 2)	{ RG[0] = READ_LONG(sh2->GBR + OP_I8 * 4); }

DEF_OP(MOVBSG, 2)	{ WRITE_BYTE(sh2->GBR + OP_I8, RG[0]); }
DEF_OP(MOVWSG, 2)	{ WRITE_WORD(sh2->GBR + OP_I8 * 2, RG[0]); }
DEF_OP(MOVLSG, 2)	{ WRITE_LONG(sh2->GBR + OP_I8 * 4, RG[0]); }

/* F_ND4 / F_MD4: register in bits 15-12 of opc, displacement in bits 11-8. */
DEF_OP(MOVBS4, 2)	{ WRITE_BYTE(RG[OP_M] + OP_D4, RG[0]); }
DEF_OP(MOVWS4, 2)	{ WRITE_WORD(RG[OP_M] + OP_D4 * 2, RG[0]); }

/* F_NMD: n = bits 3-0, m = bits 15-12, disp = bits 11-8. */
DEF_OP(MOVLS4, 2)	{ WRITE_LONG(RG[OP_N] + OP_D4 * 4, RG[OP_M]); }

DEF_OP(MOVBL4, 2)	{ RG[0] = (uint32_t)(int32_t)(int8_t)READ_BYTE(RG[OP_M] + OP_D4); }
DEF_OP(MOVWL4, 2)	{ RG[0] = (uint32_t)(int32_t)(int16_t)READ_WORD(RG[OP_M] + OP_D4 * 2); }

DEF_OP(MOVLL4, 2)
{
	const uint32_t n = OP_N;
	RG[n] = READ_LONG(RG[OP_M] + OP_D4 * 4);
}

DEF_OP(MOVA, 1)
{
	const uint32_t pc = (uint32_t)(esi - sh2->Base_PC) & ~3u;
	RG[0] = pc + OP_I8 * 4;
}

DEF_OP(MOVT, 1)		{ RG[OP_N] = sh2->SR.T; }

DEF_OP(MULL, 4)		{ sh2->MACL = RG[OP_M] * RG[OP_N]; }
DEF_OP(MULS, 3)		{ sh2->MACL = (uint32_t)((int32_t)(int16_t)RG[OP_M] * (int32_t)(int16_t)RG[OP_N]); }
DEF_OP(MULU, 3)		{ sh2->MACL = (uint32_t)(uint16_t)RG[OP_M] * (uint32_t)(uint16_t)RG[OP_N]; }

DEF_OP(NEG, 1)		{ RG[OP_N] = 0u - RG[OP_M]; }

DEF_OP(NEGC, 1)
{
	const uint64_t t = (uint64_t)RG[OP_M] + T_BIT;
	sh2->SR.T = (t != 0);
	RG[OP_N] = (uint32_t)(0u - (uint32_t)t);
}

DEF_OP(NOP, 1)		{ }
DEF_OP(NOT, 1)		{ RG[OP_N] = ~RG[OP_M]; }
DEF_OP(OR, 1)		{ RG[OP_N] = RG[OP_M] | RG[OP_N]; }
DEF_OP(ORI, 1)		{ RG[0] = OP_I8 | RG[0]; }

DEF_OP(ORM, 5)
{
	const uint32_t adr = RG[0] + sh2->GBR;
	const uint32_t imm = OP_I8;
	uint32_t v = READ_BYTE(adr);
	WRITE_BYTE(adr, v | imm);
}

DEF_OP(ROTCL, 1)
{
	const uint32_t n = OP_N;
	const uint32_t r = RG[n], c = T_BIT;
	sh2->SR.T = (uint8_t)(r >> 31);
	RG[n] = (r << 1) | c;
}

DEF_OP(ROTCR, 1)
{
	const uint32_t n = OP_N;
	const uint32_t r = RG[n], c = T_BIT;
	sh2->SR.T = (uint8_t)(r & 1);
	RG[n] = (r >> 1) | (c << 31);
}

DEF_OP(ROTL, 1)
{
	const uint32_t n = OP_N;
	const uint32_t r = RG[n];
	sh2->SR.T = (uint8_t)(r >> 31);
	RG[n] = (r << 1) | (r >> 31);
}

DEF_OP(ROTR, 1)
{
	const uint32_t n = OP_N;
	const uint32_t r = RG[n];
	sh2->SR.T = (uint8_t)(r & 1);
	RG[n] = (r >> 1) | (r << 31);
}

DEF_OP(SETT, 1)		{ sh2->SR.T = 1; }

DEF_OP(SHAL, 1)
{
	const uint32_t n = OP_N;
	sh2->SR.T = (uint8_t)(RG[n] >> 31);
	RG[n] <<= 1;
}

DEF_OP(SHAR, 1)
{
	const uint32_t n = OP_N;
	sh2->SR.T = (uint8_t)(RG[n] & 1);
	RG[n] = (uint32_t)((int32_t)RG[n] >> 1);
}

DEF_OP(SHLL, 1)
{
	const uint32_t n = OP_N;
	sh2->SR.T = (uint8_t)(RG[n] >> 31);
	RG[n] <<= 1;
}

DEF_OP(SHLL2, 1)	{ RG[OP_N] <<= 2; }
DEF_OP(SHLL8, 1)	{ RG[OP_N] <<= 8; }
DEF_OP(SHLL16, 1)	{ RG[OP_N] <<= 16; }

DEF_OP(SHLR, 1)
{
	const uint32_t n = OP_N;
	sh2->SR.T = (uint8_t)(RG[n] & 1);
	RG[n] >>= 1;
}

DEF_OP(SHLR2, 1)	{ RG[OP_N] >>= 2; }
DEF_OP(SHLR8, 1)	{ RG[OP_N] >>= 8; }
DEF_OP(SHLR16, 1)	{ RG[OP_N] >>= 16; }

DEF_OP(STCSR, 1)	{ RG[OP_N] = get_sr(sh2); }
DEF_OP(STCGBR, 1)	{ RG[OP_N] = sh2->GBR; }
DEF_OP(STCVBR, 1)	{ RG[OP_N] = sh2->VBR; }

DEF_OP(STCMSR, 3)
{
	const uint32_t n = OP_N;
	const uint32_t sr = get_sr(sh2) & 0x3F3;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, sr);
}

DEF_OP(STCMGBR, 3)
{
	const uint32_t n = OP_N;
	const uint32_t data = sh2->GBR;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(STCMVBR, 3)
{
	const uint32_t n = OP_N;
	const uint32_t data = sh2->VBR;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(STSMACH, 1)	{ RG[OP_N] = sh2->MACH; }
DEF_OP(STSMACL, 1)	{ RG[OP_N] = sh2->MACL; }
DEF_OP(STSPR, 1)	{ RG[OP_N] = sh2->PR; }

DEF_OP(STSMMACH, 3)
{
	const uint32_t n = OP_N;
	const uint32_t data = sh2->MACH;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(STSMMACL, 3)
{
	const uint32_t n = OP_N;
	const uint32_t data = sh2->MACL;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(STSMPR, 3)
{
	const uint32_t n = OP_N;
	const uint32_t data = sh2->PR;
	const uint32_t adr = RG[n] - 4;
	RG[n] = adr;
	WRITE_LONG(adr, data);
}

DEF_OP(SUB, 1)		{ RG[OP_N] = RG[OP_N] - RG[OP_M]; }

DEF_OP(SUBC, 1)
{
	const uint32_t n = OP_N;
	const uint32_t a = RG[n], b = RG[OP_M], c = T_BIT;
	sh2->SR.T = ((uint64_t)a < (uint64_t)b + c);
	RG[n] = a - b - c;
}

DEF_OP(SUBV, 1)
{
	const uint32_t n = OP_N;
	const uint32_t a = RG[n], b = RG[OP_M];
	const uint32_t r = a - b;
	sh2->SR.T = (uint8_t)(((a ^ b) & (a ^ r)) >> 31);
	RG[n] = r;
}

DEF_OP(SWAPB, 1)
{
	const uint32_t v = RG[OP_M];
	RG[OP_N] = (v & 0xFFFF0000) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}

DEF_OP(SWAPW, 1)
{
	const uint32_t v = RG[OP_M];
	RG[OP_N] = (v << 16) | (v >> 16);
}

DEF_OP(TAS, 6)
{
	const uint32_t n = OP_N;
	const uint32_t v = READ_BYTE(RG[n]) & 0xFF;
	sh2->SR.T = (v == 0);
	WRITE_BYTE(RG[n], v | 0x80);			// work only if BUS can be locked !
}

DEF_OP(TST, 1)		{ sh2->SR.T = ((RG[OP_N] & RG[OP_M]) == 0); }
DEF_OP(TSTI, 1)		{ sh2->SR.T = ((RG[0] & OP_I8) == 0); }

DEF_OP(TSTM, 4)
{
	const uint32_t imm = OP_I8;
	const uint32_t v = READ_BYTE(RG[0] + sh2->GBR);
	sh2->SR.T = ((v & imm) == 0);
}

DEF_OP(XOR, 1)		{ RG[OP_N] = RG[OP_M] ^ RG[OP_N]; }
DEF_OP(XORI, 1)		{ RG[0] = OP_I8 ^ RG[0]; }

DEF_OP(XORM, 5)
{
	const uint32_t adr = RG[0] + sh2->GBR;
	const uint32_t imm = OP_I8;
	uint32_t v = READ_BYTE(adr);
	WRITE_BYTE(adr, v ^ imm);
}

DEF_OP(XTRCT, 1)
{
	const uint32_t n = OP_N;
	RG[n] = (RG[OP_M] << 16) | (RG[n] >> 16);
}


/* Instructions without a delay slot version, or with special control flow. */

void SH2I_BF(void)
{
	const int32_t d = OP_S8;
	if (T_BIT)
	{
		RET(1);
		return;
	}

	esi = BRANCH_TARGET(d);
	SUB_CYCLES(4);
	if (edi < 0)
		st = ST_QUIT;
}

void SH2I_BFfast(void)
{
	uint32_t prev, n, cnt;

	if (T_BIT)
	{
		RET(1);
		return;
	}

	prev = fetch16(esi - 4 - 2);
	n = prev & 0x0F;
	if ((prev & 0xFFF0) != 0x1040)
	{
		// Not a "DT Rn" loop: normal BF -3.
		esi -= 2;
		SUB_CYCLES(4);
		if (edi < 0)
			st = ST_QUIT;
		return;
	}

	// "DT Rn; BF -3" idle loop: skip it.
	cnt = RG[n];
	sh2->SR.T = 1;
	esi += 2;
	SUB_CYCLES(cnt * 4);
	RG[n] = 0;
	if (edi < 0)
		st = ST_QUIT;
}

void SH2I_BFS(void)
{
	const int32_t d = OP_S8;
	uintptr_t ecx;

	if (T_BIT)
	{
		RET(1);
		return;
	}

	ecx = esi + 2;
	esi = BRANCH_TARGET(d);
	go_ds(ecx, 3);
}

void SH2I_BFSfast(void)
{
	const int32_t d = OP_S8;
	uint32_t prev, both, n, cnt;
	uintptr_t ecx;

	if (T_BIT)
	{
		RET(1);
		return;
	}

	prev = fetch16(esi - 4 - 2);
	both = (fetch16(esi - 4 + 2) << 16) | prev;
	n = prev & 0x0F;

	if ((both & ~0x0Fu) != ((SH2_NOP_CODE << 16) | 0x1040))
	{
		ecx = esi + 2;
		esi = BRANCH_TARGET(d);
		go_ds(ecx, 3);
		return;
	}

	// "DT Rn; BF/S -3; NOP" idle loop: skip it.
	cnt = RG[n];
	sh2->SR.T = 1;
	esi += 2;
	SUB_CYCLES(cnt * 5);
	RG[n] = 0;
	if (edi < 0)
		st = ST_QUIT;
}

void SH2I_BRA(void)
{
	const uintptr_t ecx = esi + 2;
	esi = BRANCH_TARGET(disp12());
	go_ds(ecx, 3);
}

void SH2I_BRAfast1(void)
{
	const uint32_t next = fetch16(esi - 4 + 2);
	const uintptr_t ecx = esi + 2;
	esi = BRANCH_TARGET(disp12());

	if (next == SH2_NOP_CODE)
	{
		// "BRA self; NOP": wait for an interrupt.
		sh2->Status |= SH2_HALTED;
		edi = -1;
		st = ST_REALLY_QUIT;
		return;
	}

	go_ds(ecx, 3);
}

void SH2I_BRAfast2(void)
{
	const uint32_t both = (fetch16(esi - 4 + 2) << 16) | fetch16(esi - 4 - 2);
	const uintptr_t ecx = esi + 2;
	esi = BRANCH_TARGET(disp12());

	if (both == ((SH2_NOP_CODE << 16) | SH2_NOP_CODE))
	{
		// "NOP; BRA -3; NOP": wait for an interrupt.
		sh2->Status |= SH2_HALTED;
		edi = -1;
		st = ST_REALLY_QUIT;
		return;
	}

	go_ds(ecx, 3);
}

void SH2I_BRAF(void)
{
	const uintptr_t ecx = esi + 2;
	const uint32_t pc = (uint32_t)(esi - sh2->Base_PC) + RG[OP_N] + 4;
	if (!fast_rebase(pc))
		return;
	go_ds(ecx, 3);
}

void SH2I_BSR(void)
{
	const uintptr_t ecx = esi + 2;
	sh2->PR = (uint32_t)(esi - sh2->Base_PC);
	esi = BRANCH_TARGET(disp12());
	go_ds(ecx, 3);
}

void SH2I_BSRF(void)
{
	const uintptr_t ecx = esi + 2;
	const uint32_t pc = (uint32_t)(esi - sh2->Base_PC);
	sh2->PR = pc;
	if (!fast_rebase(pc + RG[OP_N] + 4))
		return;
	go_ds(ecx, 3);
}

void SH2I_BT(void)
{
	const int32_t d = OP_S8;
	if (!T_BIT)
	{
		RET(1);
		return;
	}

	esi = BRANCH_TARGET(d);
	SUB_CYCLES(4);
	if (edi < 0)
		st = ST_QUIT;
}

void SH2I_BTS(void)
{
	const int32_t d = OP_S8;
	uintptr_t ecx;

	if (!T_BIT)
	{
		RET(1);
		return;
	}

	ecx = esi + 2;
	esi = BRANCH_TARGET(d);
	go_ds(ecx, 3);
}

void SH2I_ILLEGAL(void)
{
	sh2->Status |= SH2_DISABLE;
	edi = -1;
	st = ST_REALLY_QUIT;
}

void SH2I_ILLEGAL_DS(void)
{
	esi = ds_pc;
	sh2->Status |= SH2_HALTED;
	edi = -1;
	st = ST_REALLY_QUIT;
}

void SH2I_JMP(void)
{
	const uintptr_t ecx = esi + 2;
	if (!fast_rebase(RG[OP_N] + 4))
		return;
	go_ds(ecx, 3);
}

void SH2I_JSR(void)
{
	const uintptr_t ecx = esi + 2;
	sh2->PR = (uint32_t)(esi - sh2->Base_PC);
	if (!fast_rebase(RG[OP_N] + 4))
		return;
	go_ds(ecx, 3);
}

void SH2I_RTE(void)
{
	const uint32_t r15 = RG[15];
	const uintptr_t ecx = esi + 2;
	uint32_t pc, sr;

	RG[15] = r15 + 8;
	pc = READ_LONG(r15);
	sr = READ_LONG(r15 + 4) & 0x3FF;
	sh2->Cycle_Sup = (uint32_t)edi;
	edi = 0;					// Force Interrupt Check
	set_sr_packed(sh2, Set_SR_Table[sr]);
	if (!fast_rebase(pc + 4))
		return;
	go_ds(ecx, 6);
}

void SH2I_RTS(void)
{
	const uintptr_t ecx = esi + 2;
	if (!fast_rebase(sh2->PR + 4))
		return;
	go_ds(ecx, 3);
}

void SH2I_SLEEP(void)
{
	sh2->Status |= SH2_HALTED;
	edi = -1;
	esi += 2;
	st = ST_REALLY_QUIT;
}

void SH2I_SLEEP_DS(void)
{
	sh2->Status |= SH2_HALTED;
	edi = -1;
	esi += 2;
	st = ST_REALLY_QUIT;
}

/* NOTE: like the asm, TRAPA stacks the address of the TRAPA instruction
 * and resumes 2 bytes into the handler (RET after rebasing to PC + 4). */
void SH2I_TRAPA(void)
{
	const uint32_t imm = (opc >> 8) & 0xFF;
	const uint32_t sr = get_sr(sh2) & 0x3F3;
	const uint32_t sp = RG[15] - 8;
	uint32_t pc, vadr;

	RG[15] = sp;
	WRITE_LONG(sp + 4, sr);
	pc = (uint32_t)(esi - 4 - sh2->Base_PC);
	vadr = sh2->VBR + imm * 4;
	WRITE_LONG(RG[15], pc);
	pc = READ_LONG(vadr);
	if (!fast_rebase(pc + 4))
		return;
	RET(12);
}


/************************************************************
 * Public functions                                         *
 ************************************************************/

/** WDT overflow check (SH2_Exec_Check_WTC). dl = WDTSR. **/
static void exec_check_wtc(SH2_CONTEXT *c, uint8_t dl)
{
	const uint32_t inc = (c->Cycle_TD + 1) << (c->WDT_Sft & 31);
	const uint32_t cnt = c->WDTCNT + inc;
	uint8_t rst;

	if (!(cnt & 0xFF000000))
	{
		c->WDTCNT = cnt;
		return;
	}

	// Overflow.
	c->WDTCNT = 0;
	if (!(dl & 0x40))
	{
		// Interval timer mode.
		c->WDTSR = dl | 0x80;
		SH2_Interrupt_Internal(c, ((c->VCRWDT & 0xFF) << 8) | (c->IPWDT & 0xFF));
		return;
	}

	// Watchdog mode.
	c->WDTSR = dl;
	rst = c->WDTRST;
	if (rst & 0x40)
	{
		SH2_Reset(c, (uint32_t)(rst >> 5));
		c->WDTRST |= 0x80;
		return;
	}
	c->WDTRST = rst | 0x80;
}

/** Free-running timer update (SH2_Exec_Do_FRT). **/
static void exec_do_frt(SH2_CONTEXT *c)
{
	const uint32_t inc = (c->Cycle_TD + 1) << ((uint8_t)c->FRT_Sft & 31);
	const uint32_t old = c->FRTCNT;			// old FRT
	uint32_t neu = old + inc;			// new FRT
	uint32_t ocr;
	uint8_t bl = c->FRTCSR;
	const uint8_t bh = c->FRTTIER;

	ocr = c->FRTOCRA;
	if (!(ocr > old) && !(ocr < neu))
	{
		if (bl & 0x01)
			neu = 0;
		bl |= 0x08;
		if (bh & 0x08)
			SH2_Interrupt_Internal(c, ((c->IO_Reg[0x67] & 0x7F) << 8) | (c->IO_Reg[0x60] & 0x0F));
	}

	// NOTE: the asm compares FRTOCRA again here (not FRTOCRB).
	ocr = c->FRTOCRA;
	if (!(ocr > old) && !(ocr < neu))
	{
		bl |= 0x04;
		if (bh & 0x04)
			SH2_Interrupt_Internal(c, ((c->IO_Reg[0x67] & 0x7F) << 8) | (c->IO_Reg[0x60] & 0x0F));
	}

	if (neu & 0xFF000000)
	{
		bl |= 0x02;
		neu &= 0x00FFFFFF;
		if (bh & 0x02)
			SH2_Interrupt_Internal(c, ((c->IO_Reg[0x68] & 0x7F) << 8) | (c->IO_Reg[0x60] & 0x0F));
	}

	c->FRTCNT = neu;
	c->FRTCSR = bl;
}


/**
 * SH2_Exec(): Execute the SH2 until the odometer reaches 'odo'.
 * @return 0 -> ok; 0xFFFFFFFF -> error (or no cycle to do).
 */
uint32_t FASTCALL SH2_Exec(SH2_CONTEXT *c, uint32_t odo)
{
	// Save the interpreter state (in case SH2_Exec is ever re-entered).
	SH2_CONTEXT *const prev_cur = SH2_Cur;
	SH2_CONTEXT *const prev_sh2 = sh2;
	const uintptr_t prev_esi = esi, prev_ds_pc = ds_pc;
	const int32_t prev_edi = edi;
	const uint32_t prev_opc = opc;
	const int prev_st = st;
	uint32_t ret = 0;
	uint8_t status, dl;

	if (odo <= c->Odometer)
		return 0xFFFFFFFF;

	SH2_Cur = c;
	sh2 = c;
	edi = (int32_t)(odo - c->Odometer - 1);
	opc = 0;
	esi = c->Base_PC + c->PC;
	ds_pc = c->Base_PC + c->DS_PC;
	st = ST_RUN;

	check_int();
	if (st == ST_REALLY_QUIT)
		goto really_quit;

	status = (uint8_t)c->Status;
	if (status & (SH2_HALTED | SH2_DISABLE | SH2_FAULTED | SH2_RUNNING))
	{
		if (status & SH2_HALTED)
		{
			// SH2_Exec_Halted
			c->Cycle_TD = (uint32_t)edi;
			c->Odometer += (uint32_t)edi;
			dl = c->WDTSR;
			if (dl & 0x20)
				exec_check_wtc(c, dl);
			exec_do_frt(c);
			goto out;
		}

		if (status & SH2_DISABLE)
		{
			// SH2_Exec_Disable
			c->Odometer += (uint32_t)edi;
			goto out;
		}

		// SH2_Exec_Error
		ret = 0xFFFFFFFF;
		goto out;
	}

	// SH2_Exec_OK
	c->Status = (c->Status & ~0xFFu) | (uint8_t)(status | SH2_RUNNING);
	c->Cycle_Sup = 0;
	c->Cycle_TD = (uint32_t)edi;

	for (;;)
	{
		while (st == ST_RUN)
		{
			opc = fetch16(esi - 4);
			((SH2_OP)OP_Table[opc])();
		}

		if (st != ST_QUIT)
			break;

		// SH2_Exec_Quit
		edi = (int32_t)((uint32_t)edi + c->Cycle_Sup);
		c->Cycle_Sup = 0;
		if (edi < 0)
			break;

		// SH2_Exec_Interrupt_Happened
		st = ST_RUN;
		check_int();
		if (st == ST_REALLY_QUIT)
			break;
	}

really_quit:
	// SH2_Exec_Really_Quit
	c->Odometer = c->Cycle_TD - (uint32_t)edi + c->Odometer;
	c->PC = (uint32_t)(esi - c->Base_PC);
	c->DS_PC = (uint32_t)(ds_pc - c->Base_PC);
	c->Status &= ~(uint32_t)SH2_RUNNING;
	dl = c->WDTSR;
	if (dl & 0x20)
		exec_check_wtc(c, dl);
	exec_do_frt(c);

out:
	SH2_Cur = prev_cur;
	sh2 = prev_sh2;
	esi = prev_esi;
	ds_pc = prev_ds_pc;
	edi = prev_edi;
	opc = prev_opc;
	st = prev_st;
	return ret;
}


/**
 * SH2_Reset(): Reset the SH2.
 * @param manual 1 for manual reset, 0 for power-up.
 * @return 0.
 */
uint32_t FASTCALL SH2_Reset(SH2_CONTEXT *c, uint32_t manual)
{
	SH2_CONTEXT *const prev_cur = SH2_Cur;
	const uint32_t disable = c->Status & SH2_DISABLE;
	uint32_t adr, pc;
	int32_t cyc;

	SH2_Cur = c;

	// Clear everything up to the memory handler tables.
	memset(c, 0x00, offsetof(SH2_CONTEXT, Read_Byte));
	c->Status = (uint8_t)disable;		// we keep the disable status during reset

	adr = (manual & 1) * 8;
	cyc = (int32_t)adr;			// asm: edi = edx (used as Cycle_IO)
	pc = mem_rl(c, &cyc, adr) + 4;		// Read PC
	c->R[15] = mem_rl(c, &cyc, (uint32_t)cyc + 4);	// Read SP

	// REBASE_PC 1
	fetch_search(c, pc);
	c->PC = pc;
	c->SR.IMask = 0x0F;

	c->WDTCNT = 0;
	// The asm clears WDTSR and WDTRST with dword stores,
	// which also clear Unused3, FRT_Tab[0] and FRT_Tab[1].
	c->WDTSR = 0;
	c->WDTRST = 0;
	c->Unused3 = 0;
	c->FRT_Tab[0] = 0;
	c->FRT_Tab[1] = 0;
	c->BARA = 0;
	c->BAMRA = 0;
	c->TCR0 = 0;
	c->TCR1 = 0;

	c->FRTOCRA = 0xFFFF << 8;
	c->FRTOCRB = 0xFFFF << 8;

	c->WDT_Sft = 16 - 1;
	c->FRT_Sft = (c->FRT_Sft & ~0xFFu) | (8 - 3);

	SH2_Cur = prev_cur;
	return 0;
}


void FASTCALL SH2_Enable(SH2_CONTEXT *c)
{
	c->Status &= ~(uint32_t)SH2_DISABLE;
}

void FASTCALL SH2_Disable(SH2_CONTEXT *c)
{
	c->Status |= SH2_DISABLE;
}


/** Number of cycles executed since the last clear odometer command. **/
uint32_t FASTCALL SH2_Read_Odo(SH2_CONTEXT *c)
{
	uint32_t odo = c->Odometer;
	if (c->Status & SH2_RUNNING)
		odo = odo + c->Cycle_TD - c->Cycle_IO;
	return odo;
}

void FASTCALL SH2_Write_Odo(SH2_CONTEXT *c, uint32_t odo)
{
	c->Odometer = odo;
}

void FASTCALL SH2_Clear_Odo(SH2_CONTEXT *c)
{
	c->Odometer = 0;
}

void FASTCALL SH2_Add_Cycles(SH2_CONTEXT *c, uint32_t cycles)
{
	if (c->Status & SH2_RUNNING)
		c->Cycle_IO -= cycles;
	else
		c->Odometer += cycles;
}


/** Raise an external interrupt. **/
void FASTCALL SH2_Interrupt(SH2_CONTEXT *c, uint32_t level)
{
	const uint8_t vect = (uint8_t)(((uint8_t)level >> 1) + 64);
	const uint32_t prio = level & 0x0F;

	c->INT_QUEUE[prio] = vect;
	if (prio <= c->INT.Prio)
		return;

	c->INT.Prio = (uint8_t)prio;
	c->INT.Vect = vect;
	if (c->Status & SH2_RUNNING)
	{
		// Make SH2_Exec check interrupts as soon as the
		// current memory access returns.
		c->Cycle_Sup = c->Cycle_IO;
		c->Cycle_IO = 0;
	}
}

/** Raise an internal interrupt. level_vector: bits 7-0 = level, bits 15-8 = vector. **/
void FASTCALL SH2_Interrupt_Internal(SH2_CONTEXT *c, uint32_t level_vector)
{
	const uint8_t vect = (uint8_t)(level_vector >> 8);
	const uint32_t prio = level_vector & 0x1F;

	c->INT_QUEUE[prio] = vect;
	if (prio <= c->INT.Prio)
		return;

	c->INT.Prio = (uint8_t)prio;
	c->INT.Vect = vect;
	if (c->Status & SH2_RUNNING)
	{
		c->Cycle_Sup = c->Cycle_IO;
		c->Cycle_IO = 0;
	}
}

void FASTCALL SH2_NMI(SH2_CONTEXT *c)
{
	c->DMAOR |= 2;
	SH2_Interrupt_Internal(c, (11 << 8) | 16);
}


/** DREQ lines: the asm calls the 0xFF write handler with ebp = context. **/
void FASTCALL SH2_DMA0_Request(SH2_CONTEXT *c, uint8_t state)
{
	SH2_CONTEXT *const prev_cur = SH2_Cur;
	c->DREQ0 = state;
	SH2_Cur = c;
	c->Write_Long[0xFF](0xFFFFFF8C, c->CHCR0);
	SH2_Cur = prev_cur;
}

void FASTCALL SH2_DMA1_Request(SH2_CONTEXT *c, uint8_t state)
{
	SH2_CONTEXT *const prev_cur = SH2_Cur;
	c->DREQ1 = state;
	SH2_Cur = c;
	c->Write_Long[0xFF](0xFFFFFF9C, c->CHCR1);
	SH2_Cur = prev_cur;
}


void FASTCALL SH2_FRT_Signal(SH2_CONTEXT *c)
{
	const uint8_t csr = c->FRTCSR | 0x80;
	c->FRTICR = c->FRTCNT;
	c->FRTCSR = csr;
	if (c->FRTTIER & 0x80)
		SH2_Interrupt_Internal(c, ((c->IO_Reg[0x66] & 0x7F) << 8) | (c->IO_Reg[0x60] & 0x0F));
}


/* Register access */

uint32_t FASTCALL SH2_Get_R(SH2_CONTEXT *c, uint32_t num)
{
	return c->R[num & 0x0F];
}

/** Current PC value (doesn't work during SH2_Exec). **/
uint32_t FASTCALL SH2_Get_PC(SH2_CONTEXT *c)
{
	if (c->Status & SH2_RUNNING)
		return 0xFFFFFFFF;
	return c->PC;
}

uint32_t FASTCALL SH2_Get_SR(SH2_CONTEXT *c)
{
	return get_sr(c);
}

uint32_t FASTCALL SH2_Get_GBR(SH2_CONTEXT *c)	{ return c->GBR; }
uint32_t FASTCALL SH2_Get_VBR(SH2_CONTEXT *c)	{ return c->VBR; }
uint32_t FASTCALL SH2_Get_PR(SH2_CONTEXT *c)	{ return c->PR; }
uint32_t FASTCALL SH2_Get_MACH(SH2_CONTEXT *c)	{ return c->MACH; }
uint32_t FASTCALL SH2_Get_MACL(SH2_CONTEXT *c)	{ return c->MACL; }

/**
 * SH2_Set_PC(): Set the PC. (Doesn't work during SH2_Exec.)
 * NOTE: like the asm, val is stored as-is: the next instruction
 * is fetched from val - 4.
 */
void FASTCALL SH2_Set_PC(SH2_CONTEXT *c, uint32_t val)
{
	if (c->Status & SH2_RUNNING)
		return;

	if (fetch_search(c, val))
		c->PC = val;
}

void FASTCALL SH2_Set_SR(SH2_CONTEXT *c, uint32_t val)
{
	set_sr_packed(c, Set_SR_Table[val & 0x3F3]);
}

void FASTCALL SH2_Set_GBR(SH2_CONTEXT *c, uint32_t val)	{ c->GBR = val; }
void FASTCALL SH2_Set_VBR(SH2_CONTEXT *c, uint32_t val)	{ c->VBR = val; }
void FASTCALL SH2_Set_PR(SH2_CONTEXT *c, uint32_t val)	{ c->PR = val; }
void FASTCALL SH2_Set_MACH(SH2_CONTEXT *c, uint32_t val)	{ c->MACH = val; }
void FASTCALL SH2_Set_MACL(SH2_CONTEXT *c, uint32_t val)	{ c->MACL = val; }
