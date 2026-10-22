/* Emulation > Product ID: the calculator's product ID (8 bytes, which
 * add-ins read to tell calculators apart) as 16 hex digits to edit, paste
 * (Ctrl+V) or draw at random. Apply keeps it for the next runs and runs the
 * add-in again with it. Laid out at 100% and drawn at the desktop's
 * scaling, like the About window. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "view.h"

#define ID_W     460
#define ID_H     190
#define MARGIN   24
#define DIGITS   16

static SDL_Window *win;
static SDL_Renderer *ren;
static float scale = 1;
static char in_use[DIGITS + 1]; /* the one the calculator has (main.c) */
static char chosen[DIGITS + 1]; /* applied here, for the next (re)load */
static bool chosen_ready;
static char edit[DIGITS + 1];   /* the field */
static int hover = -1, down = -1; /* a button */

static const SDL_FRect FIELD = {MARGIN, 68, ID_W - 2 * MARGIN, 36};
enum { B_RANDOM, B_CANCEL, B_APPLY, BUTTONS };
static const char *const LABELS[BUTTONS] = {"Random", "Cancel", "Apply"};
static const SDL_FRect BOX[BUTTONS] = {
    {MARGIN, ID_H - 60, 100, 36},
    {ID_W - MARGIN - 210, ID_H - 60, 100, 36},
    {ID_W - MARGIN - 100, ID_H - 60, 100, 36},
};

void platform_show_device_id(const char *hex)
{
    snprintf(in_use, sizeof in_use, "%s", hex);
}

const char *platform_take_device_id(void)
{
    if (!chosen_ready)
        return NULL;
    chosen_ready = false;
    return chosen;
}

static void color(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
}

static bool inside(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool complete(void)
{
    return strlen(edit) == DIGITS;
}

static void redraw(void)
{
    color(250, 250, 250);
    SDL_RenderClear(ren);
    color(20, 20, 24);
    view_text_px(ren, MARGIN, MARGIN + 7, "Calculator product ID", 17);
    color(100, 100, 108);
    view_text_px(ren, MARGIN, MARGIN + 30, "16 hex digits (Ctrl+V pastes); Apply restarts the add-in", 13);

    /* The field: the digits in groups of 4, a caret after them */
    color(255, 255, 255);
    SDL_RenderFillRect(ren, &FIELD);
    color(0, 120, 215);
    SDL_RenderRect(ren, &FIELD);
    char shown[DIGITS + DIGITS / 4];
    size_t n = 0;
    for (size_t i = 0; edit[i]; i++) {
        if (i && i % 4 == 0)
            shown[n++] = ' ';
        shown[n++] = edit[i];
    }
    shown[n] = '\0';
    float tx = FIELD.x + 12, cy = FIELD.y + FIELD.h / 2;
    color(20, 20, 24);
    view_text_px(ren, tx, cy, shown, 18);
    if (!complete()) {
        SDL_FRect caret = {tx + view_text_w_px(shown, 18) + 2, FIELD.y + 8, 1.5f, FIELD.h - 16};
        SDL_RenderFillRect(ren, &caret);
        char left[48];
        snprintf(left, sizeof left, "%d more", DIGITS - (int)strlen(edit));
        color(170, 0, 0);
        view_text_px(ren, FIELD.x + FIELD.w - 12 - view_text_w_px(left, 13), cy, left, 13);
    }

    for (int b = 0; b < BUTTONS; b++) {
        bool off = b == B_APPLY && !complete();
        Uint8 c = down == b ? 190 : hover == b && !off ? 214 : 232;
        color(c, c, (Uint8)(c + 6));
        SDL_RenderFillRect(ren, &BOX[b]);
        color(170, 170, 178);
        SDL_RenderRect(ren, &BOX[b]);
        if (off)
            color(160, 160, 166);
        else
            color(20, 20, 24);
        float w = view_text_w_px(LABELS[b], 15);
        view_text_px(ren, BOX[b].x + (BOX[b].w - w) / 2, BOX[b].y + BOX[b].h / 2, LABELS[b], 15);
    }
    SDL_RenderPresent(ren);
}

void id_window_close(void)
{
    if (!win)
        return;
    SDL_StopTextInput(win);
    view_text_forget(ren);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    ren = NULL;
    win = NULL;
    hover = down = -1;
}

void id_window_open(void)
{
    if (view_headless)
        return;
    if (!win) {
        scale = view_ui_scale;
        if (!SDL_CreateWindowAndRenderer("Product ID", (int)(ID_W * scale), (int)(ID_H * scale),
                                         SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
            fprintf(stderr, "product ID window: %s\n", SDL_GetError());
            return;
        }
        float d = SDL_GetWindowPixelDensity(win);
        SDL_SetRenderScale(ren, scale * d, scale * d);
        snprintf(edit, sizeof edit, "%s", in_use);
        SDL_StartTextInput(win);
    }
    SDL_RaiseWindow(win);
    redraw();
}

/* Adds the hex digits of s (others are left out) while there is room. */
static void type(const char *s)
{
    size_t n = strlen(edit);
    for (; *s && n < DIGITS; s++)
        if (isxdigit((unsigned char)*s))
            edit[n++] = (char)toupper((unsigned char)*s);
    edit[n] = '\0';
}

static void apply(void)
{
    if (!complete())
        return;
    bool same = !strcmp(edit, in_use);
    snprintf(chosen, sizeof chosen, "%s", edit);
    platform_set_device_id(edit); /* kept for the next runs */
    id_window_close();
    if (same)
        return;
    chosen_ready = true;
    char msg[80];
    if (view_have_app) {
        view_pending = CMD_RESET; /* emu_init writes it into the OS's ROM */
        view_paused = false;
        snprintf(msg, sizeof msg, "Product ID %s: add-in restarted", chosen);
    } else {
        snprintf(msg, sizeof msg, "Product ID %s: used by the next add-in", chosen);
    }
    view_flash_status(msg);
}

static void press(int b)
{
    if (b == B_RANDOM) {
        for (int i = 0; i < DIGITS; i++)
            edit[i] = "0123456789ABCDEF"[SDL_rand(16)];
        edit[DIGITS] = '\0';
        redraw();
    } else if (b == B_CANCEL) {
        id_window_close();
    } else if (b == B_APPLY) {
        apply();
    }
}

static int button_at(float x, float y)
{
    for (int b = 0; b < BUTTONS; b++)
        if (inside(BOX[b], x, y))
            return b;
    return -1;
}

bool id_window_event(const SDL_Event *e)
{
    if (!win)
        return false;
    SDL_WindowID id = SDL_GetWindowID(win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id)
            return false;
        id_window_close();
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        if (e->window.windowID != id)
            return false;
        redraw();
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e->window.windowID != id)
            return false;
        hover = down = -1;
        redraw();
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        if (e->motion.windowID != id)
            return false;
        int h = button_at(e->motion.x / scale, e->motion.y / scale);
        if (h != hover) {
            hover = h;
            redraw();
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.windowID != id)
            return false;
        down = button_at(e->button.x / scale, e->button.y / scale);
        redraw();
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e->button.windowID != id)
            return false;
        int b = button_at(e->button.x / scale, e->button.y / scale);
        bool hit = b >= 0 && b == down;
        down = -1;
        if (hit)
            press(b); /* may close the window */
        else
            redraw();
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (e->text.windowID != id)
            return false;
        type(e->text.text);
        redraw();
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.windowID != id)
            return false;
        if (e->type == SDL_EVENT_KEY_UP)
            return true;
        if (e->key.key == SDLK_ESCAPE) {
            id_window_close();
        } else if (e->key.key == SDLK_RETURN || e->key.key == SDLK_KP_ENTER) {
            apply();
        } else if (e->key.key == SDLK_BACKSPACE) {
            size_t n = strlen(edit);
            if (e->key.mod & SDL_KMOD_CTRL)
                edit[0] = '\0';
            else if (n)
                edit[n - 1] = '\0';
            redraw();
        } else if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                /* a whole ID replaces the field */
                size_t digits = 0;
                for (const char *p = clip; *p; p++)
                    digits += isxdigit((unsigned char)*p) != 0;
                if (digits == DIGITS)
                    edit[0] = '\0';
                type(clip);
                SDL_free(clip);
            }
            redraw();
        }
        return true;
    }
    return false;
}
