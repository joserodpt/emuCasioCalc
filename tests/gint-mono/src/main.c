/* gint test for the monochrome fx-9860G models: the T6K11 display, the
   keyboard, timers, the RTC, the clocks, and the grey engine (F1: four
   shades from two flickering pictures). Press keys to see their codes,
   EXIT quits. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/timer.h>
#include <gint/rtc.h>
#include <gint/clock.h>
#include <gint/gray.h>
#include <gint/hardware.h>
#include <stdio.h>

static volatile int ticks;

static int tick(void)
{
	ticks++;
	return TIMER_CONTINUE;
}

/* The monochrome screen: text, a frame and a checkerboard */
static void draw_mono(int key, int presses)
{
	dclear(C_WHITE);
	drect(0, 0, DWIDTH - 1, 8, C_BLACK);
	dtext(2, 2, C_WHITE, "gint mono test");

	const clock_frequency_t *f = clock_freq();
	dprint(2, 11, C_BLACK, "CPU %d/%d MHz", f->Iphi_f / 1000000, f->Pphi_f / 1000000);
	dprint(2, 19, C_BLACK, "%s", gint[HWCALC] == HWCALC_G35PE2 ? "Graph 35+E II" : "fx-9860G SH4");
	dprint(2, 27, C_BLACK, "ticks %d", ticks);

	rtc_time_t t;
	rtc_get_time(&t);
	dprint(2, 35, C_BLACK, "%02d:%02d:%02d", t.hours, t.minutes, t.seconds);

	for(int y = 0; y < 16; y++)
	for(int x = 0; x < 16; x++)
		if((x / 4 + y / 4) & 1) dpixel(108 + x, 12 + y, C_BLACK);
	drect_border(106, 10, 125, 29, C_NONE, 1, C_BLACK);

	if(key) dprint(2, 47, C_BLACK, "key 0x%02x (%d)", key, presses);
	else dtext(2, 47, C_BLACK, "F1 grey, EXIT quits");
	dupdate();
}

/* Four shades side by side */
static void draw_grey(void)
{
	dclear(C_WHITE);
	static const int SHADES[] = { C_WHITE, C_LIGHT, C_DARK, C_BLACK };
	for(int i = 0; i < 4; i++)
		drect(i * 32, 16, i * 32 + 31, 47, SHADES[i]);
	dtext(2, 2, C_BLACK, "grey: 4 shades");
	dtext(2, 52, C_BLACK, "any key: back");
	dupdate();
}

int main(void)
{
	int t = timer_configure(TIMER_ANY, 10000, GINT_CALL(tick));
	if(t >= 0) timer_start(t);

	int key = 0, presses = 0;
	draw_mono(key, presses);
	while(1) {
		key_event_t ev = getkey();
		if(ev.key == KEY_EXIT) break;
		if(ev.key == KEY_F1 && dgray(DGRAY_ON) == 0) {
			draw_grey();
			getkey();
			dgray(DGRAY_OFF);
		}
		else {
			key = ev.key;
			presses++;
		}
		draw_mono(key, presses);
	}

	if(t >= 0) timer_stop(t);
	return 0;
}
