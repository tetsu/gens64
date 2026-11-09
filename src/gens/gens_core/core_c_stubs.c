/***************************************************************************
 * Gens: Placeholder definitions for the 32X core (C core).                *
 *                                                                         *
 * The 32X emulation code (SH2 core, 32X memory maps) is still 32-bit x86  *
 * asm and hasn't been ported to C yet. These placeholders let the C core  *
 * (e.g. x86_64 builds) link. 32X games are refused at load time           *
 * (see Init_32X() in emulator/g_32x.cpp).                                 *
 *                                                                         *
 * Generated from the linker's undefined symbol list. Remove entries as   *
 * the corresponding asm files are ported to C.                            *
 ***************************************************************************/

/* Data placeholders: same types as the extern declarations. */

#include "gens_core/mem/mem_m68k.h"
#include "gens_core/mem/mem_m68k_cd.h"
#include "gens_core/mem/mem_m68k_32x.h"
#include "gens_core/mem/mem_s68k.h"
#include "gens_core/mem/mem_sh2.h"
#include "gens_core/cpu/sh2/sh2.h"
#include "gens_core/gfx/gfx_cd.h"
#include "segacd/lc89510.h"

__typeof__(Bank_SH2) Bank_SH2;
__typeof__(CPL_MSH2) CPL_MSH2;
__typeof__(CPL_SSH2) CPL_SSH2;
__typeof__(Cycles_MSH2) Cycles_MSH2;
__typeof__(Cycles_SSH2) Cycles_SSH2;
__typeof__(M_SH2) M_SH2;
__typeof__(OPDS_Table) OPDS_Table;
__typeof__(OP_Table) OP_Table;
__typeof__(S_SH2) S_SH2;
__typeof__(Set_SR_Table) Set_SR_Table;
__typeof__(_32X_ADEN) _32X_ADEN;
__typeof__(_32X_Comm) _32X_Comm;
__typeof__(_32X_DREQ_DST) _32X_DREQ_DST;
__typeof__(_32X_DREQ_LEN) _32X_DREQ_LEN;
__typeof__(_32X_DREQ_SRC) _32X_DREQ_SRC;
__typeof__(_32X_DREQ_ST) _32X_DREQ_ST;
__typeof__(_32X_FIFO_A) _32X_FIFO_A;
__typeof__(_32X_FIFO_B) _32X_FIFO_B;
__typeof__(_32X_FIFO_Block) _32X_FIFO_Block;
__typeof__(_32X_FIFO_Read) _32X_FIFO_Read;
__typeof__(_32X_FIFO_Write) _32X_FIFO_Write;
__typeof__(_32X_FM) _32X_FM;
__typeof__(_32X_Genesis_Rom) _32X_Genesis_Rom;
__typeof__(_32X_HIC) _32X_HIC;
__typeof__(_32X_M68K_Read_Byte_Table) _32X_M68K_Read_Byte_Table;
__typeof__(_32X_M68K_Read_Word_Table) _32X_M68K_Read_Word_Table;
__typeof__(_32X_M68K_Write_Byte_Table) _32X_M68K_Write_Byte_Table;
__typeof__(_32X_M68K_Write_Word_Table) _32X_M68K_Write_Word_Table;
__typeof__(_32X_MINT) _32X_MINT;
__typeof__(_32X_MSH2_Rom) _32X_MSH2_Rom;
__typeof__(_32X_RES) _32X_RES;
__typeof__(_32X_RV) _32X_RV;
__typeof__(_32X_Ram) _32X_Ram;
__typeof__(_32X_Rom) _32X_Rom;
__typeof__(_32X_SINT) _32X_SINT;
__typeof__(_32X_SSH2_Rom) _32X_SSH2_Rom;
__typeof__(_MSH2_Reg) _MSH2_Reg;
__typeof__(_SH2_VDP_Reg) _SH2_VDP_Reg;
__typeof__(_SSH2_Reg) _SSH2_Reg;

/* Called if one of the function placeholders is ever reached. */
#include <stdio.h>
void core_c_stub_called(const char *name);
void core_c_stub_called(const char *name)
{
	fprintf(stderr, "gens: 32X function '%s' is not available in the C core yet.\n", name);
}
