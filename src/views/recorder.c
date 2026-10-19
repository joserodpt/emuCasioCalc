/* File > Record GIF: the calculator screen as an animated GIF, at the pace
 * it was shown in the window (real time, without the pauses); without a
 * window (--headless --record), at the emulated clock's, so a script gives
 * the same file each time however fast it runs. A frame is
 * written once the next one comes, when its duration is known; frames
 * closer than GIF_MIN_CS are merged, as viewers slow down shorter ones. */
#include <stdio.h>
#include <string.h>

#include "gif.h"
#include "r61524.h"
#include "view.h"

#define GIF_SCALE  2 /* each screen pixel as 2 x 2: sharp at forum and README sizes */
#define GIF_MIN_CS 2 /* browsers show frames under 20 ms for 100 ms */

static GifWriter *gif;
static char gif_path[1024];
static uint16_t pending[PANEL_WIDTH * PANEL_HEIGHT]; /* shown since pending_cs, not written yet */
static bool have_pending;
static uint64_t pending_cs;
static uint64_t start_ms;     /* when the recording's clock started */
static uint64_t paused_ms;    /* time spent paused since */
static uint64_t pause_start;  /* when the current pause began, 0 if not paused */
static uint64_t shown_second; /* the elapsed second on the menu bar */
static uint64_t emu_now;      /* the emulated clock (ms), timing headless recordings */

static uint64_t now_ms(void)
{
    return view_headless ? emu_now : SDL_GetTicks();
}

void recorder_clock(uint64_t emu_ms)
{
    emu_now = emu_ms;
}

bool recorder_active(void)
{
    return gif != NULL;
}

/* Follows Emulation > Pause: paused time isn't recorded. */
static void follow_pause(void)
{
    uint64_t now = now_ms();
    if (view_paused && !pause_start)
        pause_start = now;
    else if (!view_paused && pause_start) {
        paused_ms += now - pause_start;
        pause_start = 0;
    }
}

uint64_t recorder_elapsed_ms(void)
{
    if (!gif || !have_pending)
        return 0;
    follow_pause();
    uint64_t now = pause_start ? pause_start : now_ms();
    return now - start_ms - paused_ms;
}

static void begin(const uint16_t *fb)
{
    memcpy(pending, fb, sizeof pending);
    have_pending = true;
    pending_cs = 0;
    start_ms = now_ms();
    paused_ms = 0;
    pause_start = view_paused ? start_ms : 0;
}

bool recorder_start(const char *path, const uint16_t *fb)
{
    if (gif)
        return true;
    gif = gif_open(path, PANEL_WIDTH, PANEL_HEIGHT, GIF_SCALE);
    if (!gif)
        return false;
    snprintf(gif_path, sizeof gif_path, "%s", path);
    have_pending = false;
    shown_second = 0;
    if (fb)
        begin(fb);
    return true;
}

void recorder_frame(const uint16_t *fb)
{
    if (!gif)
        return;
    if (!have_pending) { /* the clock starts with the first frame */
        begin(fb);
        return;
    }
    if (!memcmp(fb, pending, sizeof pending))
        return;
    uint64_t now_cs = recorder_elapsed_ms() / 10;
    if (now_cs - pending_cs >= GIF_MIN_CS) {
        gif_frame(gif, pending, (int)(now_cs - pending_cs));
        pending_cs = now_cs;
    }
    memcpy(pending, fb, sizeof pending);
}

/* Each second, the elapsed time on the menu bar moves on. */
void recorder_tick(void)
{
    if (!gif)
        return;
    uint64_t second = recorder_elapsed_ms() / 1000;
    if (second != shown_second) {
        shown_second = second;
        view_redraw();
    }
}

bool recorder_stop(char *path, size_t cap, uint64_t *ms)
{
    if (!gif)
        return false;
    uint64_t elapsed = recorder_elapsed_ms();
    if (have_pending) {
        uint64_t cs = elapsed / 10;
        gif_frame(gif, pending, cs - pending_cs >= GIF_MIN_CS ? (int)(cs - pending_cs) : GIF_MIN_CS);
    } else { /* nothing was shown: one frame of what the window has */
        gif_frame(gif, view_last_frame(), GIF_MIN_CS);
    }
    bool ok = gif_close(gif);
    gif = NULL;
    snprintf(path, cap, "%s", gif_path);
    *ms = elapsed;
    return ok;
}
