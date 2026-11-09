#ifndef GENS_GFX_CD_H
#define GENS_GFX_CD_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct Rot_Comp_t
{
	unsigned int Stamp_Size;
	unsigned int Stamp_Map_Adr;
	unsigned int IB_V_Cell_Size;
	unsigned int IB_Adr;
	unsigned int IB_Offset;
	unsigned int IB_H_Dot_Size;
	unsigned int IB_V_Dot_Size;
	unsigned int Vector_Adr;
	int Rotation_Running;
} Rot_Comp;

extern const int Table_Rot_Time[4 * 4 * 4];

// GENS Rerecording [GENS Savestate v7]
extern int Float_Part, Draw_Speed;
extern int XS, YS, DXS, DYS, XD, YD, XD_Mul, H_Dot;

#ifdef GENS_CORE_C
// C core: Stamp_Map_Adr, Buffer_Adr, and Vector_Adr are byte offsets into
// Ram_Word_2M[]. Jmp_Adr is an index into the rotation renderer table.
// (32-bit values, so savestates store them in 4 bytes on 64-bit hosts.)
extern uint32_t Stamp_Map_Adr, Buffer_Adr, Vector_Adr, Jmp_Adr;
#else
// ERROR: These are addresses, not ints!
// Stamp_Map_Adr, Buffer_Adr, and Vector_Adr are offsets into Ram_Word_2M[].
// Jmp_Adr[] is an offset into Table_Jump_Rot[].
extern void *Stamp_Map_Adr, *Buffer_Adr, *Vector_Adr, *Jmp_Adr;
#endif

void Init_RS_GFX(void);
void Calcul_Rot_Comp(void);
void Update_Rot(void);

#ifdef __cplusplus
}
#endif

#endif /* GENS_GFX_CD_H */
