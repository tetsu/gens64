/* Vendored from https://github.com/superzazu/z80 (MIT, see z80core_LICENSE.txt).
 * Modified for Gens: symbols renamed z80_* -> zcore_*, and zcore_get_f(),
 * zcore_set_f(), zcore_process_interrupts() exported. */
#ifndef ZCORE_H_
#define ZCORE_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct zcore zcore;
struct zcore {
  uint8_t (*read_byte)(void*, uint16_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  uint8_t (*port_in)(zcore*, uint8_t);
  void (*port_out)(zcore*, uint8_t, uint8_t);
  void* userdata;

  unsigned long cyc; // cycle count (t-states)

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
};

void zcore_init(zcore* const z);
void zcore_step(zcore* const z);
void zcore_debug_output(zcore* const z);
void zcore_gen_nmi(zcore* const z);
void zcore_gen_int(zcore* const z, uint8_t data);
uint8_t zcore_get_f(zcore* const z);
void zcore_set_f(zcore* const z, uint8_t val);
void zcore_process_interrupts(zcore* const z);

#endif // ZCORE_H_
