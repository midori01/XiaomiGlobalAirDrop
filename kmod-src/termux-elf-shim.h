/* Definitions missing from bionic's <elf.h> that kbuild host tools (modpost) need. */
#include <elf.h>
/* bionic defines these via ELF_ST_BIND/TYPE, which modpost.h redefines back to them. */
#undef ELF64_ST_BIND
#undef ELF64_ST_TYPE
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#ifndef R_ARM_CALL
#define R_ARM_CALL 28
#endif
#ifndef R_ARM_JUMP24
#define R_ARM_JUMP24 29
#endif
#ifndef R_ARM_THM_JUMP24
#define R_ARM_THM_JUMP24 30
#endif
#ifndef R_ARM_MOVW_ABS_NC
#define R_ARM_MOVW_ABS_NC 43
#endif
#ifndef R_ARM_MOVT_ABS
#define R_ARM_MOVT_ABS 44
#endif
#ifndef R_ARM_THM_MOVW_ABS_NC
#define R_ARM_THM_MOVW_ABS_NC 47
#endif
#ifndef R_ARM_THM_MOVT_ABS
#define R_ARM_THM_MOVT_ABS 48
#endif
#ifndef R_ARM_THM_JUMP19
#define R_ARM_THM_JUMP19 51
#endif
