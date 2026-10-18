#include "display.h"

#include "casio_font.h"
#include "font.h"
#include "mem.h"
#include "platform.h"

/* TEXT_COLOR_BLACK, BLUE, GREEN, CYAN, RED, PURPLE, YELLOW, WHITE */
static const uint16_t TEXT_COLORS[8] = {0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF};

uint16_t display_text_color(int index)
{
    return TEXT_COLORS[index & 7];
}

void display_fill(uint8_t *vram, uint16_t color)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        vram[2 * i] = color >> 8;
        vram[2 * i + 1] = color & 0xFF;
    }
}

void display_set_pixel(uint8_t *vram, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    uint8_t *p = vram + 2 * (y * LCD_WIDTH + x);
    p[0] = color >> 8;
    p[1] = color & 0xFF;
}

uint16_t display_get_pixel(const uint8_t *vram, int x, int y)
{
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return 0;
    const uint8_t *p = vram + 2 * (y * LCD_WIDTH + x);
    return (uint16_t)(p[0] << 8 | p[1]);
}

void display_rect(uint8_t *vram, int x0, int y0, int x1, int y1, uint16_t color)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            display_set_pixel(vram, x, y, color);
}

/* The 8x8 font, unscaled: the emulator's own small labels (status bar). */
static void draw_char8(uint8_t *vram, int px, int py, char c, uint16_t fg)
{
    const uint8_t *g = platform_glyph(c);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (g[y] & (0x80 >> x))
                display_set_pixel(vram, px + x, py + y, fg);
}

void display_text_small(uint8_t *vram, int x, int y, const char *text, uint16_t fg)
{
    for (const char *c = text; *c; c++, x += SMALL_CELL_W)
        draw_char8(vram, x, y + 1, *c, fg);
}

/*
 * Casio text. Strings are bytes in the OS's character set: ASCII, a few
 * symbols above 0x80, and 2-byte characters after the lead bytes 0x7F,
 * 0xE5, 0xE6, 0xE7, 0xF7, 0xF9 (MB_ElementCount's list). With GB 18030 on,
 * 0x81-0xFE start 2-byte (or, before 0x30-0x39, 4-byte) Chinese
 * characters. The symbols mapped are those whose codes are known (the
 * GetKey character codes); other characters draw as U+FFFD, Chinese ones as
 * an empty wide box (no CJK font is embedded).
 */
#define CP_WIDE_BOX 0x110000u /* not Unicode: an empty box of a wide cell */

static bool gb18030;

void display_set_gb18030(bool on)
{
    gb18030 = on;
}

bool display_gb18030(void)
{
    return gb18030;
}

bool text_is_lead(uint8_t c)
{
    if (gb18030 && c >= 0x81 && c <= 0xFE)
        return true;
    return c == 0x7F || c == 0xE5 || c == 0xE6 || c == 0xE7 || c == 0xF7 || c == 0xF9;
}

int text_char_len(const char *text)
{
    const uint8_t *s = (const uint8_t *)text;
    if (!s[0])
        return 0;
    if (!text_is_lead(s[0]) || !s[1])
        return 1;
    if (gb18030 && s[0] >= 0x81 && s[1] >= 0x30 && s[1] <= 0x39 && s[2] && s[3])
        return 4;
    return 2;
}

int text_length(const char *text)
{
    int n = 0;
    for (int len; (len = text_char_len(text)); text += len)
        n++;
    return n;
}

static uint32_t single_byte(uint8_t c)
{
    if (c >= 0x20 && c < 0x7F)
        return c;
    switch (c) {
    case 0x0A: return 0x21B5; /* line feed, shown as a symbol */
    case 0x0E: return 0x2192; /* -> (store) */
    case 0x0F: return 0x1D07; /* small E of x10^x */
    case 0x10: return 0x2264; /* <= */
    case 0x11: return 0x2260; /* not equal */
    case 0x12: return 0x2265; /* >= */
    case 0x13: return 0x21D2; /* => */
    case 0x86: return 0x221A; /* square root */
    case 0x87: return 0x207B; /* (-), raised minus */
    case 0x89: return '+';
    case 0x8B: return 0x00B2; /* squared */
    case 0x96: return 0x221B; /* cube root */
    case 0x99: return 0x2212; /* minus */
    case 0xA8: return '^';
    case 0xA9: return 0x00D7; /* times */
    case 0xB9: return 0x00F7; /* divided by */
    case 0xBB: return 0x231F; /* fraction mark */
    case 0xCD: return 'r';
    case 0xCE: return 0x03B8; /* theta */
    case 0xD0: return 0x03C0; /* pi */
    }
    return 0xFFFD;
}

uint32_t text_decode(const char *text, int *len)
{
    const uint8_t *s = (const uint8_t *)text;
    *len = text_char_len(text);
    if (*len == 1)
        return single_byte(s[0]);
    if (gb18030 && s[0] >= 0x81)
        return CP_WIDE_BOX;
    uint16_t code = (uint16_t)(s[0] << 8 | s[1]);
    switch (code) {
    case 0x7F50: return 'i';    /* imaginary unit */
    case 0x7F54: return 0x2220; /* angle */
    }
    return 0xFFFD;
}

const FontGlyph *font_glyph(const Font *font, uint32_t cp)
{
    int lo = 0, hi = font->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font->glyphs[mid].cp == cp)
            return &font->glyphs[mid];
        if (font->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* The OS's code of the character at text (len bytes): a byte, or lead
 * byte << 8 | byte; NO_CODE for Chinese characters. */
#define NO_CODE 0xFFFFFFFFu

static uint32_t text_code(const char *text, int len)
{
    const uint8_t *s = (const uint8_t *)text;
    if (len == 1)
        return s[0];
    if (len == 2 && !(gb18030 && s[0] >= 0x81))
        return (uint32_t)(s[0] << 8 | s[1]);
    return NO_CODE;
}

/* How cp looks in font: its glyph (from FONT_EXTRA if the font lacks it,
 * scaled to the font's height), as source and target sizes. */
typedef struct {
    const Font *src;
    const FontGlyph *g;
    const uint8_t *g8; /* or an 8x8 glyph (Classic), scaled sx x sy */
    int sx, sy;
    const uint8_t *cg; /* or Casio's glyph (font style Casio), 1:1 */
    int cstride;
    bool own_bold;     /* Casio's bold font: not drawn twice */
    int w, h;          /* drawn size */
    int left, adv;     /* proportional placement, drawn pixels */
} Shape;

/* View > Font: which glyphs the three text sizes use. */
static int font_style = FONT_STYLE_CLASSIC;

void display_set_font_style(int style)
{
    font_style = style;
}

static Shape shape_of(const Font *font, uint32_t cp, uint32_t code, bool bold)
{
    Shape sh = {0};
    sh.h = font->h;
    /* Casio: the OS's own glyphs, from the user's OS dump (casio_font.h);
     * without one, Spleen */
    if (font_style == FONT_STYLE_CASIO && code != NO_CODE) {
        CasioFontId id = font == &FONT_BIG    ? CFONT_BIG
                         : font == &FONT_MINI ? CFONT_MINI
                         : bold               ? CFONT_TINY_BOLD
                                              : CFONT_TINY;
        CasioGlyph cg;
        if (casio_glyph(id, (uint16_t)code, &cg)) {
            sh.cg = cg.bits;
            sh.cstride = cg.stride;
            sh.own_bold = id == CFONT_TINY_BOLD;
            sh.w = sh.adv = cg.w;
            sh.h = cg.h;
            return sh;
        }
    }
    if (cp == CP_WIDE_BOX) {
        sh.w = font->h;
        sh.adv = font->h + 1;
        return sh;
    }
    /* Classic: the emulator's first look, the 8x8 font scaled (2x3 in the
     * big cells, 1x2 in PrintMini's fixed 10 px cells, 1x1 in 8 px ones) */
    if (font_style == FONT_STYLE_CLASSIC && cp >= 0x20 && cp < 0x7F) {
        sh.g8 = platform_glyph((char)cp);
        sh.sx = font == &FONT_BIG ? 2 : 1;
        sh.sy = font == &FONT_BIG ? 3 : font == &FONT_MINI ? 2 : 1;
        sh.w = 8 * sh.sx;
        sh.h = 8 * sh.sy;
        sh.left = font == &FONT_MINI ? -1 : 0;
        sh.adv = font == &FONT_MINI ? MINI_CELL_W : 8 * sh.sx;
        return sh;
    }
    const FontGlyph *g = NULL;
    const Font *src = font;
    /* Unifont: its 8x16 glyphs, scaled to the big and mini sizes */
    if (font_style == FONT_STYLE_UNIFONT && font != &FONT_TINY) {
        src = &FONT_EXTRA;
        g = font_glyph(src, cp);
        if (!g)
            src = font;
    }
    if (!g)
        g = font_glyph(font, cp);
    if (!g) {
        src = &FONT_EXTRA;
        g = font_glyph(src, cp);
        if (!g)
            g = font_glyph(src, 0xFFFD);
    }
    sh.src = src;
    sh.g = g;
    sh.w = g->w * font->h / src->h;
    sh.left = g->left * font->h / src->h;
    sh.adv = g->adv * font->h / src->h;
    if (sh.adv < 2)
        sh.adv = 2;
    return sh;
}

static bool shape_pixel(const Shape *sh, int x, int y)
{
    if (sh->g8)
        return sh->g8[y / sh->sy] & (0x80 >> (x / sh->sx));
    if (sh->cg)
        return sh->cg[y * sh->cstride + x / 8] >> (7 - x % 8) & 1;
    if (!sh->g) /* the wide box: an outline */
        return x == 1 || x == sh->w - 2 || y == 2 || y == sh->h - 3;
    int sx = x * sh->src->h / sh->h, sy = y * sh->src->h / sh->h;
    int nb = (sh->g->w + 7) / 8;
    uint8_t byte = sh->src->bits[sh->g->offset + (uint32_t)(sy * nb + sx / 8)];
    return byte >> (7 - sx % 8) & 1;
}

/* Draws a shape's columns from x0 (drawn pixels) at (px, py); bold draws
 * each lit pixel twice, the second one pixel right. */
static void draw_shape(uint8_t *vram, const Shape *sh, int px, int py, int x0, int width, uint16_t fg,
                       bool bold)
{
    for (int y = 0; y < sh->h; y++) {
        for (int x = 0; x < width; x++) {
            int gx = x0 + x;
            if (gx >= 0 && gx < sh->w && shape_pixel(sh, gx, y)) {
                display_set_pixel(vram, px + x, py + y, fg);
                if (bold)
                    display_set_pixel(vram, px + x + 1, py + y, fg);
            }
        }
    }
}

/* Fixed cells (PrintXY, PrintCXY): BIG_CELL_W x BIG_CELL_H, glyph centred;
 * Chinese characters take a wider cell. */
void display_text_big(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg, bool opaque)
{
    for (int len; (len = text_char_len(text)); text += len) {
        uint32_t cp = text_decode(text, &len);
        Shape sh = shape_of(&FONT_BIG, cp, text_code(text, len), false);
        int cw = cp == CP_WIDE_BOX ? BIG_CELL_H : BIG_CELL_W;
        if (opaque)
            display_rect(vram, x, y, x + cw - 1, y + BIG_CELL_H - 1, bg);
        draw_shape(vram, &sh, x + (cw - sh.w) / 2, y, 0, sh.w, fg, false);
        x += cw;
    }
}

void display_print_xy(uint8_t *vram, int x, int y, const char *text, int mode, int color)
{
    uint16_t fg = display_text_color(color), bg = 0xFFFF;
    if (mode & TEXT_MODE_INVERT) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    bool opaque = !(mode & TEXT_MODE_TRANSPARENT_BACKGROUND);
    display_text_big(vram, (x - 1) * BIG_CELL_W, y * BIG_CELL_H, text, fg, bg, opaque);
}

/* Proportional text in `font`, glyphs top at y + oy in lines of line_h;
 * stops before xlimit; draws only if draw. Returns the x after it. */
int display_text_prop(uint8_t *vram, const Font *font, int x, int y, int line_h, int oy, const char *text,
                      uint16_t fg, uint16_t bg, bool opaque, bool bold, int xlimit, bool draw)
{
    for (int len; (len = text_char_len(text)); text += len) {
        uint32_t cp = text_decode(text, &len);
        Shape sh = shape_of(font, cp, text_code(text, len), bold);
        bool twice = bold && !sh.own_bold;
        int adv = sh.adv + (twice ? 1 : 0);
        if (x + adv > xlimit)
            break;
        if (draw) {
            /* Casio's glyphs are whole cells: from y, as tall as the font */
            int top = sh.cg ? y : y + oy, height = sh.cg ? sh.h : line_h;
            if (opaque)
                display_rect(vram, x, y, x + adv - 1, y + height - 1, bg);
            draw_shape(vram, &sh, x, top, sh.left, adv, fg, twice);
        }
        x += adv;
    }
    return x;
}

int display_text_mini(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg,
                      bool opaque, int xlimit, bool draw)
{
    return display_text_prop(vram, &FONT_MINI, x, y, MINI_CELL_H, 1, text, fg, bg, opaque, false, xlimit, draw);
}
