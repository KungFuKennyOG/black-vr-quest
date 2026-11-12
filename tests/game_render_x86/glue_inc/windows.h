/* tests/game_render_x86, x86_fault_glue.c only: what platform/mmio_decode.h's x86-64 decoder needs from
 * <windows.h> (DWORD). That TU borrows the production decoder (the one the Windows build runs) by presenting a
 * CONTEXT-shaped register file built from the Linux ucontext_t; see x86_fault_glue.c. */
#ifndef GRX_GLUE_WINDOWS_H
#define GRX_GLUE_WINDOWS_H
#include <stdint.h>
typedef uint32_t DWORD;
#endif
