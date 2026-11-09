/***************************************************************************
 * Gens: Musashi 68000 core configuration.                                 *
 *                                                                         *
 * Selected via -DMUSASHI_CNF="\"m68kconf_gens.h\"". Overrides the        *
 * defaults in m68kconf.h, which is included at the end.                   *
 ***************************************************************************/

#ifndef M68KCONF_GENS_H
#define M68KCONF_GENS_H

/* Genesis / Sega CD only need a plain MC68000. */
#define M68K_EMULATE_010		M68K_OPT_OFF
#define M68K_EMULATE_EC020		M68K_OPT_OFF
#define M68K_EMULATE_020		M68K_OPT_OFF
#define M68K_EMULATE_030		M68K_OPT_OFF
#define M68K_EMULATE_040		M68K_OPT_OFF
#define M68K_EMULATE_PMMU		M68K_OPT_OFF

/* Instruction fetches use m68k_read_immediate_*() (fast fetch regions). */
#define M68K_SEPARATE_READS		M68K_OPT_ON

/* -(An) long writes: low word first, like Starscream. */
#define M68K_SIMULATE_PD_WRITES		M68K_OPT_ON

/* Interrupt acknowledge goes to the Gens wrapper. */
#define M68K_EMULATE_INT_ACK		M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)	gens_m68k_int_ack(A)
int gens_m68k_int_ack(int int_level);

/* Starscream doesn't emulate these. */
#define M68K_EMULATE_PREFETCH		M68K_OPT_OFF
#define M68K_EMULATE_ADDRESS_ERROR	M68K_OPT_OFF

#include "m68kconf.h"

#endif /* M68KCONF_GENS_H */
