/* Text rows for the test add-ins' result screens (gint drawing). */
#ifndef ROWS_H
#define ROWS_H

/* The title bar at the top. */
void row_title(char const *format, ...);

/* Formatted text at row (1 = under the title), column x (1-based, 8 px). */
void row_print_color(int row, int x, int fg, int bg, char const *format, ...);

#endif
