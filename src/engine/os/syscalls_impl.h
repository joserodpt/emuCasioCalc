#ifndef EMU_SYSCALLS_IMPL_H
#define EMU_SYSCALLS_IMPL_H

/* Shared by the syscall implementation files (syscalls*.c). */

#include <stddef.h>
#include <stdint.h>

#include "emu.h"

/* a[0..3] = r4-r7, a[4..] = stack arguments @(0,r15), @(4,r15)... */
#define SYSCALL_ARGS 12
#define SC(name) uint32_t name(Emu *emu, const uint32_t *a)

/* Charges emulated time for work the real OS would do. */
void sc_spend(Emu *emu, double cycles);

/* Message boxes (syscalls_text.c), shared by MsgBoxPush and the dialogs. */
typedef struct {
    int x0, y0, x1, y1;
} BoxRect;
BoxRect msgbox_rect(int lines);
void msgbox_open(Emu *emu, int lines);  /* saves VRAM, draws the box */
void msgbox_close(Emu *emu);            /* puts VRAM back */

/* Shows VRAM and waits for a key like GetKey (syscalls_keys.c). Returns its
 * GetKey code, or -1 when there is none left (headless) or the window was
 * closed; the add-in then stops, as with GetKey. */
int sc_wait_key(Emu *emu);

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
SC(sc_save_vram_1);
SC(sc_load_vram_1);
SC(sc_print_mini_mini);
SC(sc_locate);
SC(sc_print_line);
SC(sc_print_os);

/* syscalls_dialogs.c: OS dialogs */
SC(sc_display_message_box);
SC(sc_color_index_dialog);
SC(sc_progress_bar);
SC(sc_progress_bar0);
SC(sc_progress_bar2);
SC(sc_scrollbar);
SC(sc_box_inner_clear);
SC(sc_box_y_limits);

/* syscalls_keys.c: keyboard and text input */
SC(sc_get_key);
SC(sc_get_key_wait_os);
SC(sc_prgm_get_key_os);
SC(sc_edit_mbstring_char);
SC(sc_edit_mbstring_ctrl2);

/* syscalls_bfile.c: storage memory files (Bfile_*) */
SC(sc_bfile_open);
SC(sc_bfile_close);
SC(sc_bfile_read);
SC(sc_bfile_write);
SC(sc_bfile_create);
SC(sc_bfile_delete);
SC(sc_bfile_seek);
SC(sc_bfile_tell);
SC(sc_bfile_rename);
SC(sc_bfile_str_to_name);

/* syscalls_mcs.c: main memory (MCS) files */
SC(sc_mcs_create_dir);
SC(sc_mcs_put_var2);
SC(sc_mcs_ovw_dat2);
SC(sc_mcs_del_var2);
SC(sc_mcs_get_dlen2);
SC(sc_mcs_get_data1);

#endif
