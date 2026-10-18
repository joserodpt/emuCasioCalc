/* gint stress test: renders FRAMES frames as fast as possible (rectangles,
   images through gint's DSP-loop blitters, formatted text), timing them with
   a 1 kHz timer. Then waits for a key: F1 triggers an address error to show
   gint's panic screen, anything else quits. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 60

extern bopti_image_t img_sprite;
static volatile int ms;

static int tick(void)
{
	ms++;
	return TIMER_CONTINUE;
}

int main(void)
{
	int t = timer_configure(TIMER_ANY, 1000, GINT_CALL(tick));
	if(t >= 0) timer_start(t);

	/* Some heap traffic, checked afterwards */
	int *blocks[16];
	for(int i = 0; i < 16; i++) {
		blocks[i] = malloc(1000 * (i + 1));
		if(blocks[i]) memset(blocks[i], i, 1000 * (i + 1));
	}
	int heap_ok = 1;
	for(int i = 0; i < 16; i++) {
		if(!blocks[i] || ((char *)blocks[i])[999 * (i + 1)] != i) heap_ok = 0;
		free(blocks[i]);
	}

	int start = ms;
	for(int f = 0; f < FRAMES; f++) {
		dclear(C_RGB(f % 32, 8, 31 - f % 32));
		for(int i = 0; i < 40; i++) {
			int x = (i * 37 + f * 5) % (DWIDTH - 44);
			int y = (i * 23 + f * 3) % (DHEIGHT - 11);
			dimage(x, y, &img_sprite);
		}
		drect(10, 10, 250, 50, C_WHITE);
		dprint(16, 16, C_BLACK, "frame %d/%d", f + 1, FRAMES);
		dprint(16, 32, C_BLACK, "%.3f s elapsed", (ms - start) / 1000.0);
		dupdate();
	}
	int elapsed = ms - start;

	dclear(C_WHITE);
	dprint(10, 10, C_BLACK, "%d frames in %d ms", FRAMES, elapsed);
	dprint(10, 30, C_BLACK, "%d.%d FPS", FRAMES * 1000 / (elapsed ? elapsed : 1),
		(FRAMES * 10000 / (elapsed ? elapsed : 1)) % 10);
	dprint(10, 50, heap_ok ? C_GREEN : C_RED, "heap: %s", heap_ok ? "ok" : "BROKEN");
	dimage(10, 80, &img_sprite);
	dtext(10, 120, C_BLACK, "F1: address error (panic screen)");
	dtext(10, 140, C_BLACK, "other key: quit");
	dupdate();

	key_event_t ev = getkey();
	if(ev.key == KEY_F1) {
		/* mov.l to an odd address (C would split it into bytes) */
		__asm__ volatile("mov.l %0, @%1" :: "r"(1), "r"(0x08100001));
	}
	if(t >= 0) timer_stop(t);
	return 0;
}
