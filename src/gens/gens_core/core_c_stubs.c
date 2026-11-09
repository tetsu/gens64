/***************************************************************************
 * Gens: Placeholder definitions for the SegaCD / 32X cores (C core).      *
 *                                                                         *
 * The SegaCD and 32X emulation code is still 32-bit x86 asm and hasn't    *
 * been ported to C yet. These placeholders let the Genesis-only C core    *
 * (e.g. x86_64 builds) link. SegaCD and 32X games are refused at load     *
 * time while GENS_CORE_C_NO_SEGACD_32X is defined (see core_c_stubs.h).   *
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

__typeof__(BRAM_Ex_Size) BRAM_Ex_Size;
__typeof__(BRAM_Ex_State) BRAM_Ex_State;
__typeof__(Bank_SH2) Bank_SH2;
__typeof__(Buffer_Adr) Buffer_Adr;
__typeof__(CDC) CDC;
__typeof__(CDD) CDD;
__typeof__(CD_Access_Timer) CD_Access_Timer;
__typeof__(COMM) COMM;
__typeof__(CPL_MSH2) CPL_MSH2;
__typeof__(CPL_S68K) CPL_S68K;
__typeof__(CPL_SSH2) CPL_SSH2;
__typeof__(Cell_Conv_Tab) Cell_Conv_Tab;
__typeof__(Cycles_MSH2) Cycles_MSH2;
__typeof__(Cycles_S68K) Cycles_S68K;
__typeof__(Cycles_SSH2) Cycles_SSH2;
__typeof__(DXS) DXS;
__typeof__(DYS) DYS;
__typeof__(Draw_Speed) Draw_Speed;
__typeof__(Float_Part) Float_Part;
__typeof__(Font_BITS) Font_BITS;
__typeof__(Font_COLOR) Font_COLOR;
__typeof__(H_Dot) H_Dot;
__typeof__(Init_Timer_INT3) Init_Timer_INT3;
__typeof__(Int_Mask_S68K) Int_Mask_S68K;
__typeof__(Jmp_Adr) Jmp_Adr;
__typeof__(LED_Status) LED_Status;
__typeof__(M_SH2) M_SH2;
__typeof__(Memory_Control_Status) Memory_Control_Status;
__typeof__(OPDS_Table) OPDS_Table;
__typeof__(OP_Table) OP_Table;
__typeof__(Ram_Backup) Ram_Backup;
__typeof__(Ram_Backup_Ex) Ram_Backup_Ex;
__typeof__(Ram_Prg) Ram_Prg;
__typeof__(Ram_Word_1M) Ram_Word_1M;
__typeof__(Ram_Word_2M) Ram_Word_2M;
__typeof__(Ram_Word_State) Ram_Word_State;
__typeof__(Rot_Comp) Rot_Comp;
__typeof__(S68K_Mem_PM) S68K_Mem_PM;
__typeof__(S68K_Mem_WP) S68K_Mem_WP;
__typeof__(S68K_State) S68K_State;
__typeof__(S_SH2) S_SH2;
__typeof__(SegaCD_M68K_Read_Byte_Table) SegaCD_M68K_Read_Byte_Table;
__typeof__(SegaCD_M68K_Read_Word_Table) SegaCD_M68K_Read_Word_Table;
__typeof__(SegaCD_M68K_Write_Byte_Table) SegaCD_M68K_Write_Byte_Table;
__typeof__(SegaCD_M68K_Write_Word_Table) SegaCD_M68K_Write_Word_Table;
__typeof__(Set_SR_Table) Set_SR_Table;
__typeof__(Stamp_Map_Adr) Stamp_Map_Adr;
__typeof__(Timer_INT3) Timer_INT3;
__typeof__(Timer_Step) Timer_Step;
__typeof__(Vector_Adr) Vector_Adr;
__typeof__(XD) XD;
__typeof__(XD_Mul) XD_Mul;
__typeof__(XS) XS;
__typeof__(YD) YD;
__typeof__(YS) YS;
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
	fprintf(stderr, "gens: SegaCD/32X function '%s' is not available in the C core yet.\n", name);
}
