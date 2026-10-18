#ifndef EMU_SYSCALLS_IMPL_H
#define EMU_SYSCALLS_IMPL_H

/* Shared by the syscall implementation files (syscalls*.c). */

#include <stddef.h>
#include <stdint.h>

#include "emu.h"

/* a[0..3] = r4-r7, a[4..] = stack arguments @(0,r15), @(4,r15)... */
#define SYSCALL_ARGS 12
#define SC(name) uint32_t name(Emu *emu, const uint32_t *a)

/* Reads a NUL-terminated string from emulated memory. */
void sc_string(Emu *emu, uint32_t addr, char *out, size_t cap);

/* syscalls_text.c: drawing and text */
SC(sc_get_vram_address);
SC(sc_bdisp_allclr_vram);
SC(sc_bdisp_putdisp_dd);
SC(sc_bdisp_setpoint_vram);
SC(sc_print_xy);
SC(sc_print_xy_2);
SC(sc_print_cxy);
SC(sc_print_mini);
SC(sc_get_mini_glyph_ptr);
SC(sc_print_mini_glyph);
SC(sc_draw_frame);
SC(sc_get_fkey_ptr);
SC(sc_msgbox_push);
SC(sc_msgbox_pop);
SC(sc_display_mbstring2);

/* syscalls_keys.c: keyboard and text input */
SC(sc_get_key);
SC(sc_get_key_wait_os);
SC(sc_prgm_get_key_os);
SC(sc_edit_mbstring_char);
SC(sc_edit_mbstring_ctrl2);

/* syscalls_mcs.c: main memory (MCS) files */
SC(sc_mcs_put_var2);
SC(sc_mcs_ovw_dat2);
SC(sc_mcs_del_var2);
SC(sc_mcs_get_dlen2);
SC(sc_mcs_get_data1);

#endif
