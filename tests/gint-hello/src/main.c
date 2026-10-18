/* gint smoke test: exercises the display (DMA), keyboard (KEYSC + timer
   interrupts), timers and the RTC. Press keys to see their names, EXIT quits. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/timer.h>
#include <gint/rtc.h>
#include <gint/clock.h>
#include <stdio.h>

static volatile int ticks;

static int tick(void)
{
	ticks++;
	return TIMER_CONTINUE;
}

static void draw(int key, int presses)
{
	dclear(C_WHITE);
	drect(0, 0, DWIDTH - 1, 20, C_RGB(4, 10, 20));
	dtext(4, 5, C_WHITE, "gint on emuCasioFxCG");

	for(int i = 0; i < 6; i++)
		drect(10 + 30 * i, 40, 35 + 30 * i, 70, C_RGB(31 - 5 * i, 5 * i, 16));

	const clock_frequency_t *f = clock_freq();
	dprint(10, 90, C_BLACK, "CPU %d MHz, Pphi %d MHz",
		f->Iphi_f / 1000000, f->Pphi_f / 1000000);
	dprint(10, 110, C_BLACK, "timer ticks: %d", ticks);

	rtc_time_t t;
	rtc_get_time(&t);
	dprint(10, 130, C_BLACK, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
		t.year, t.month, t.month_day, t.hours, t.minutes, t.seconds);

	if(key)
		dprint(10, 160, C_RED, "key 0x%02x (%d presses)", key, presses);
	else
		dtext(10, 160, C_BLUE, "press a key, EXIT to quit");
	dupdate();
}

int main(void)
{
	int t = timer_configure(TIMER_ANY, 10000, GINT_CALL(tick));
	if(t >= 0) timer_start(t);

	int key = 0, presses = 0;
	draw(key, presses);
	while(1) {
		key_event_t ev = getkey();
		if(ev.key == KEY_EXIT) break;
		key = ev.key;
		presses++;
		draw(key, presses);
	}

	if(t >= 0) timer_stop(t);
	return 0;
}
