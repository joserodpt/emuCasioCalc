/* The on-screen keyboard: the fx-CG50's keys right of the screen (View >
 * Keyboard). Clicking presses a key (held while the mouse button is), keys
 * light up while pressed by any means, and each shows its SHIFT function
 * (yellow) and ALPHA letter (red) above it, as on the calculator. */
#include <stdio.h>
#include <string.h>

#include "keys.h"
#include "view.h"

/* Grid: 6 columns for the top rows, 5 for the four rows of big keys; the
 * arrow pad takes the right two columns of rows 1-2. Units: a row is a
 * label strip (LABEL_H) plus a key (KEY_H). */
typedef struct {
    const char *name;    /* KEYS name */
    const char *label;   /* on the key */
    const char *shift;   /* above it, yellow */
    int row, col, cols;  /* position in a grid of `cols` columns */
} PadKey;

static const PadKey PAD_KEYS[] = {
    {"F1", "F1", NULL, 0, 0, 6}, {"F2", "F2", NULL, 0, 1, 6}, {"F3", "F3", NULL, 0, 2, 6},
    {"F4", "F4", NULL, 0, 3, 6}, {"F5", "F5", NULL, 0, 4, 6}, {"F6", "F6", NULL, 0, 5, 6},
    {"SHIFT", "SHIFT", NULL, 1, 0, 6}, {"OPTN", "OPTN", NULL, 1, 1, 6},
    {"VARS", "VARS", "PRGM", 1, 2, 6}, {"MENU", "MENU", "SETUP", 1, 3, 6},
    {"ALPHA", "ALPHA", NULL, 2, 0, 6}, {"SQUARE", "x2", "sqrt", 2, 1, 6},
    {"POWER", "^", "xrt", 2, 2, 6}, {"EXIT", "EXIT", "QUIT", 2, 3, 6},
    {"XOT", "X,T", "ang", 3, 0, 6}, {"LOG", "log", "10^x", 3, 1, 6}, {"LN", "ln", "e^x", 3, 2, 6},
    {"SIN", "sin", "asin", 3, 3, 6}, {"COS", "cos", "acos", 3, 4, 6}, {"TAN", "tan", "atan", 3, 5, 6},
    {"FRAC", "ab/c", "mixd", 4, 0, 6}, {"FD", "F<>D", "frac", 4, 1, 6}, {"LPAR", "(", "3rt", 4, 2, 6},
    {"RPAR", ")", "x-1", 4, 3, 6}, {"COMMA", ",", NULL, 4, 4, 6}, {"ARROW", "->", NULL, 4, 5, 6},
    {"7", "7", "CAPT", 5, 0, 5}, {"8", "8", "CLIP", 5, 1, 5}, {"9", "9", "PASTE", 5, 2, 5},
    {"DEL", "DEL", "UNDO", 5, 3, 5}, {"AC", "AC/ON", "OFF", 5, 4, 5},
    {"4", "4", "CATLG", 6, 0, 5}, {"5", "5", "FRMT", 6, 1, 5}, {"6", "6", NULL, 6, 2, 5},
    {"MULT", "x", "{", 6, 3, 5}, {"DIV", "/", "}", 6, 4, 5},
    {"1", "1", "List", 7, 0, 5}, {"2", "2", "Mat", 7, 1, 5}, {"3", "3", NULL, 7, 2, 5},
    {"PLUS", "+", "[", 7, 3, 5}, {"MINUS", "-", "]", 7, 4, 5},
    {"0", "0", "i", 8, 0, 5}, {"DP", ".", "=", 8, 1, 5}, {"EXP", "x10^", "pi", 8, 2, 5},
    {"NEG", "(-)", "Ans", 8, 3, 5}, {"EXE", "EXE", "CR", 8, 4, 5},
};
#define PAD_COUNT ((int)(sizeof PAD_KEYS / sizeof PAD_KEYS[0]))

/* The arrow pad, in the right two columns of rows 1-2: up, left, right,
 * down around the centre. */
static const struct { const char *name, *label; float dx, dy; } ARROWS[] = {
    {"UP", "^", 0, -1}, {"LEFT", "<", -1, 0}, {"RIGHT", ">", 1, 0}, {"DOWN", "v", 0, 1},
};

#define ROWS 9
#define LABEL_H 0.38f /* label strip, in key heights */
#define KEY_GAP 0.12f

static SDL_FRect area;       /* where the keypad is drawn */
static int mouse_key = -1;   /* KEYS index held with the mouse */
static bool shown_down[256]; /* keys held, as the emulator sees them */

static float row_h(void)
{
    return area.h / ROWS;
}

/* The key body of PAD_KEYS[i] (without its label strip). */
static SDL_FRect key_rect(const PadKey *p)
{
    float rh = row_h(), cw = area.w / (float)p->cols;
    float label = rh * LABEL_H / (1 + LABEL_H), gap = rh * KEY_GAP;
    return (SDL_FRect){area.x + p->col * cw + gap / 2, area.y + p->row * rh + label, cw - gap, rh - label - gap / 2};
}

/* Size of an arrow key: three fit across the two columns and two rows. */
static float arrow_size(void)
{
    float rh = row_h(), cw = area.w / 6;
    float s = rh * 0.6f;
    return s < cw * 0.6f ? s : cw * 0.6f;
}

static SDL_FRect arrow_rect(int i, SDL_FPoint *centre)
{
    float rh = row_h(), cw = area.w / 6;
    float cx = area.x + 5 * cw, cy = area.y + 2 * rh + rh * 0.1f; /* centre of the 2x2 cells */
    float s = arrow_size();
    if (centre)
        *centre = (SDL_FPoint){cx, cy};
    return (SDL_FRect){cx + ARROWS[i].dx * s * 1.08f - s / 2, cy + ARROWS[i].dy * s * 1.02f - s / 2, s, s};
}

static bool inside(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* KEYS index under (x, y), or -1. */
static int key_at(float x, float y)
{
    if (!inside(area, x, y))
        return -1;
    for (int i = 0; i < PAD_COUNT; i++)
        if (inside(key_rect(&PAD_KEYS[i]), x, y))
            return key_find(PAD_KEYS[i].name);
    for (int i = 0; i < 4; i++)
        if (inside(arrow_rect(i, NULL), x, y))
            return key_find(ARROWS[i].name);
    return -1;
}

void keypad_set_area(SDL_FRect r)
{
    area = r;
}

int keypad_mouse_key(void)
{
    return mouse_key;
}

/* Keys held as the emulator sees them (host keyboard, mouse, key script):
 * redraws when that changes. */
void keypad_show_down(const bool *down)
{
    bool changed = false;
    for (int k = 0; k < KEY_COUNT; k++) {
        if (shown_down[k] != down[k])
            changed = true;
        shown_down[k] = down[k];
    }
    if (changed && view_show_keypad)
        view_redraw();
}

/* Mouse presses on the keypad: returns the KEYS index of a key pressed
 * (as handle_event does for the keyboard), -2 for an event used otherwise,
 * -1 for one that isn't the keypad's. */
int keypad_event(const SDL_Event *e)
{
    if (!view_show_keypad)
        return -1;
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT)
            return -1;
        mouse_key = key_at(e->button.x, e->button.y);
        if (mouse_key < 0)
            return inside(area, e->button.x, e->button.y) ? -2 : -1;
        view_redraw();
        return mouse_key;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (mouse_key < 0)
            return -1;
        mouse_key = -1;
        view_redraw();
        return -2;
    }
    return -1;
}

static void set_color(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(view_renderer, r, g, b, 255);
}

/* Text centred in a box, as large as fits (1x, 2x or 3x the 8x8 font). */
static void centred(const char *s, float cx, float cy, float maxw, float maxh, int kmax)
{
    int n = (int)strlen(s), k = kmax;
    while (k > 1 && (n * 8 * k > maxw || 8 * k > maxh))
        k--;
    view_text(view_renderer, cx - n * 4.0f * k, cy - 4.0f * k, s, k);
}

static void draw_key(SDL_FRect r, const char *label, bool down, int style)
{
    /* style: 0 dark key, 1 light (digits), 2 F-key, 3 SHIFT, 4 ALPHA, 5 AC/EXE */
    static const Uint8 FACE[6][3] = {{58, 60, 66}, {226, 226, 222}, {236, 236, 236}, {214, 170, 40},
                                     {186, 44, 44}, {52, 82, 130}};
    const Uint8 *f = FACE[style];
    float d = down ? 0.55f : 1.0f;
    set_color((Uint8)(f[0] * d), (Uint8)(f[1] * d), (Uint8)(f[2] * d));
    SDL_RenderFillRect(view_renderer, &r);
    set_color(20, 20, 20);
    SDL_RenderRect(view_renderer, &r);
    bool light = style == 1 || style == 2;
    if (light && !down)
        set_color(20, 20, 20);
    else
        set_color(250, 250, 250);
    centred(label, r.x + r.w / 2, r.y + r.h / 2, r.w - 6, r.h - 4, 3);
}

static bool is_down(int k)
{
    if (k < 0)
        return false;
    return k == mouse_key || shown_down[k] || view_key_held(k);
}

void keypad_draw(void)
{
    if (!view_show_keypad || area.w <= 0)
        return;
    set_color(28, 30, 36);
    SDL_RenderFillRect(view_renderer, &area);
    float rh = row_h(), label = rh * LABEL_H / (1 + LABEL_H);
    for (int i = 0; i < PAD_COUNT; i++) {
        const PadKey *p = &PAD_KEYS[i];
        int k = key_find(p->name);
        SDL_FRect r = key_rect(p);
        int style = p->row == 0 ? 2 : p->row >= 5 ? 1 : 0;
        if (!strcmp(p->name, "SHIFT")) style = 3;
        else if (!strcmp(p->name, "ALPHA")) style = 4;
        else if (!strcmp(p->name, "AC") || !strcmp(p->name, "EXE") || !strcmp(p->name, "DEL")) style = 5;
        draw_key(r, p->label, is_down(k), style);
        /* SHIFT (yellow, left) and ALPHA (red, right) labels above */
        float top = r.y - label / 2;
        if (p->shift) {
            set_color(236, 196, 60);
            centred(p->shift, r.x + r.w * 0.3f, top, r.w * 0.6f, label, 2);
        }
        if (k >= 0 && KEYS[k].alpha) {
            char a[4];
            int c = KEYS[k].alpha;
            snprintf(a, sizeof a, "%s", c == ' ' ? "SP" : c == 0xCD ? "r" : c == 0xCE ? "th" : (char[]){(char)c, 0});
            set_color(232, 80, 80);
            centred(a, r.x + r.w * 0.82f, top, r.w * 0.35f, label, 2);
        }
    }
    /* arrow pad: a round base and four keys */
    SDL_FPoint c;
    arrow_rect(0, &c);
    float s = arrow_size() * 1.65f;
    SDL_FRect base = {c.x - s, c.y - s, 2 * s, 2 * s};
    set_color(46, 48, 54);
    SDL_RenderFillRect(view_renderer, &base);
    for (int i = 0; i < 4; i++)
        draw_key(arrow_rect(i, NULL), ARROWS[i].label, is_down(key_find(ARROWS[i].name)), 0);
}
