#include "rows.h"

#include <gint/display.h>
#include <stdarg.h>
#include <stdio.h>

#define ROW_X 6
#define ROW_W 8
#define ROW_Y 20
#define ROW_H 14

void row_title(char const *format, ...)
{
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    drect(0, 0, DWIDTH - 1, ROW_Y - 6, C_RGB(0, 0, 31));
    dtext(ROW_X, 2, C_WHITE, text);
}

void row_print_color(int row, int x, int fg, int bg, char const *format, ...)
{
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    dtext_opt(ROW_X + ROW_W * (x - 1), ROW_Y + ROW_H * (row - 1) + 2, fg, bg, DTEXT_LEFT, DTEXT_TOP, text, -1);
}
