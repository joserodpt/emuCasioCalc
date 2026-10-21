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

/* syscalls_keys.c: whether the key at an OS matrix position (columns 7..2
 * left to right, rows 2..10 bottom to top, AC/on (1, 1)) is held, and the
 * first key held (its KEYS index and position, or -1). Both count as a key
 * poll. */
bool sc_key_held_at(Emu *emu, int col, int row);
void sc_os_matrix(int k, int *col, int *row); /* KEYS[k]'s position */
int sc_key_held(Emu *emu, int *col, int *row);

/* syscalls_fx.c: the monochrome models' OS (fx-9860G) */
void fx_show_vram(Emu *emu);  /* Bdisp_PutDisp_DD: the 1 kB VRAM to the display */
SC(sc_fx_glib_execution_check);
SC(sc_fx_glib_lib_info);
SC(sc_fx_os_version);
SC(sc_fx_set_mmu);
SC(sc_fx_dd_clear);
SC(sc_fx_get_disp_dd);
SC(sc_fx_putdisp_dd);
SC(sc_fx_draw_line_vram);
SC(sc_fx_clear_line_vram);
SC(sc_fx_allclr_dd);
SC(sc_fx_allclr_vram);
SC(sc_fx_allclr_ddvram);
SC(sc_fx_get_disp_vram);
SC(sc_fx_setpoint_vram);
SC(sc_fx_setpoint_dd);
SC(sc_fx_setpoint_ddvram);
SC(sc_fx_getpoint_vram);
SC(sc_fx_areaclr_dd);
SC(sc_fx_areaclr_vram);
SC(sc_fx_areaclr_ddvram);
SC(sc_fx_area_reverse_vram);
SC(sc_fx_popup_win);
SC(sc_fx_popup_at);
SC(sc_fx_print_xy);
SC(sc_fx_print_mini);
SC(sc_fx_print);
SC(sc_fx_print_rev);
SC(sc_fx_print_c);
SC(sc_fx_print_rev_c);
SC(sc_fx_print_line);
SC(sc_fx_print_rline);
SC(sc_fx_print_cr);
SC(sc_fx_cursor_settings);
SC(sc_fx_save_disp);
SC(sc_fx_restore_disp);
SC(sc_fx_key_down_at);
SC(sc_fx_any_key_down);
SC(sc_fx_key_down);
SC(sc_fx_put_key);
SC(sc_fx_keycode_table);
SC(sc_fx_ins_state);
SC(sc_fx_data_table);

/* Reads a NUL-terminated string from emulated memory. */
void sc_string(Emu *emu, uint32_t addr, char *out, size_t cap);

/* syscalls_text.c: drawing and text */
SC(sc_get_vram_address);
SC(sc_bdisp_allclr_vram);
SC(sc_bdisp_putdisp_dd);
SC(sc_bdisp_setpoint_vram);
SC(sc_print_xy);
SC(sc_print_cxy);
SC(sc_print_mini);
SC(sc_get_mini_glyph_ptr);
SC(sc_print_mini_glyph);
SC(sc_draw_frame);
SC(sc_get_fkey_ptr);
SC(sc_fkey_display);
SC(sc_msgbox_push);
SC(sc_msgbox_pop);
SC(sc_display_mbstring2);
SC(sc_save_vram_1);
SC(sc_load_vram_1);
SC(sc_get_secondary_vram_address);
SC(sc_print_mini_mini);
SC(sc_locate);
SC(sc_print_line);
SC(sc_print_os);
SC(sc_display_mbstring);
SC(sc_mmprint);
SC(sc_mmprint_ref);
SC(sc_print_glyph);
SC(sc_print_line2);
SC(sc_process_print_chars);
SC(sc_enable_gb18030);
SC(sc_disable_gb18030);
SC(sc_mb_is_lead);
SC(sc_mb_element_count);

/* syscalls_draw.c: shapes, points, bitmaps, the display (_DD) */
SC(sc_fill_vram);
SC(sc_area_clr);
SC(sc_rectangle);
SC(sc_filled_rectangle);
SC(sc_filled_rectangle_full);
SC(sc_get_point_vram);
SC(sc_set_point_dd);
SC(sc_get_point_dd);
SC(sc_direct_draw_rectangle);
SC(sc_frame_color);
SC(sc_putdisp_stripe);
SC(sc_write_graph_vram);
SC(sc_write_graph_dd);

/* syscalls_status.c: the status area (top 24 rows of VRAM) */
void status_area_draw(Emu *emu);
SC(sc_enable_status_area);
SC(sc_define_status_area_flags);
SC(sc_define_status_message);
SC(sc_define_status_glyph);
SC(sc_display_status_area);
SC(sc_header_text);
SC(sc_header_fill);
SC(sc_draw_header_line);
SC(sc_enable_display_header);
SC(sc_battery_icon);

/* syscalls_time.c: OS timers, RTC, delays */
SC(sc_timer_install);
SC(sc_timer_start);
SC(sc_timer_stop);
SC(sc_timer_deinstall);
SC(sc_rtc_elapsed_ms);
SC(sc_rtc_reset);
SC(sc_rtc_set_date_time);
SC(sc_delay_micros);
SC(sc_delay_100micros);

/* messages.c: OS messages in the chosen language */
bool os_message(int lang, int no, char *out, size_t cap);
void set_language(Emu *emu, int lang);
SC(sc_localize_message1);
SC(sc_print_xy_2);
SC(sc_app_system_language);

/* syscalls_dialogs.c: OS dialogs */
SC(sc_display_message_box);
SC(sc_color_index_dialog);
SC(sc_progress_bar);
SC(sc_progress_bar0);
SC(sc_progress_bar2);
SC(sc_scrollbar);
SC(sc_box_inner_clear);
SC(sc_box_y_limits);
SC(sc_character_select);

/* syscalls_keys.c: keyboard and text input */
SC(sc_get_key);
SC(sc_get_key_wait_os);
SC(sc_prgm_get_key_os);
SC(sc_edit_mbstring_char);
SC(sc_edit_mbstring_ctrl);
SC(sc_edit_mbstring_ctrl2);
SC(sc_put_keycode);
SC(sc_spy_matrix_code);
SC(sc_bkey_clr_all_flags);
SC(sc_bkey_set_flag);
SC(sc_bkey_get_all_flags);
SC(sc_bkey_set_all_flags);
SC(sc_get_menu_return);
SC(sc_enable_menu_return);
SC(sc_disable_menu_return);

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
SC(sc_bfile_name_to_str);
SC(sc_bfile_name_length);
SC(sc_bfile_size);
SC(sc_bfile_match_mask);
SC(sc_bfile_media_free);
SC(sc_bfile_block_address);
SC(sc_bfile_find_first);
SC(sc_bfile_find_next);
SC(sc_bfile_find_close);

/* syscalls_mcs.c: main memory (MCS) files */
SC(sc_mcs_create_dir);
SC(sc_mcs_put_var2);
SC(sc_mcs_ovw_dat2);
SC(sc_mcs_del_var2);
SC(sc_mcs_get_dlen2);
SC(sc_mcs_get_data1);
SC(sc_mcs_write_item);
SC(sc_mcs_delete_dir);
SC(sc_mcs_get_state);
SC(sc_mcs_get_capa);
SC(sc_mcs_get_main_memory_start);
SC(sc_mcs_get_open_item);
SC(sc_clip_store);
SC(sc_alpha_get);
SC(sc_alpha_set);

/* syscalls_system.c: restart, screenshots, icons, BCD */
SC(sc_restart);
SC(sc_take_screenshot);
SC(sc_smem_map_icon);
SC(sc_app_init_dlg_descriptor);
SC(sc_bcd_to_internal);

#endif
