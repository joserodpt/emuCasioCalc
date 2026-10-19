/* gint test add-in for both families, from one source: the layout follows
   the screen's size (DWIDTH x DHEIGHT: 396x224 or 128x64) and colours are
   used only where there are some (GINT_RENDER_RGB). Shows the model, the
   clock and a counter; arrows move a box, EXIT quits. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/clock.h>
#include <gint/hardware.h>
#include <stdio.h>

#if GINT_RENDER_RGB
#define BAR_COLOR C_RGB(4, 10, 20)
#define BOX_COLOR C_RGB(31, 8, 4)
#define SCALE 3     /* the colour screen is about 3x the monochrome one */
#else
#define BAR_COLOR C_BLACK
#define BOX_COLOR C_BLACK
#define SCALE 1
#endif

static char const *model(void)
{
	switch(gint[HWCALC]) {
	case HWCALC_FXCG50:      return "fx-CG50";
	case HWCALC_PRIZM:       return "fx-CG10/20";
	case HWCALC_G35PE2:      return "Graph 35+E II";
	case HWCALC_FX9860G_SH4: return "fx-9860G SH4";
	default:                 return "other";
	}
}

static void draw(int bx, int by, int moves)
{
	int bar = 9 * SCALE, line = 8 * SCALE;
	dclear(C_WHITE);
	drect(0, 0, DWIDTH - 1, bar - 1, BAR_COLOR);
	dtext(2 * SCALE, (bar - 6) / 2, C_WHITE, "gint dual test");

	const clock_frequency_t *f = clock_freq();
	dprint(2 * SCALE, bar + 2, C_BLACK, "%s, %dx%d", model(), DWIDTH, DHEIGHT);
	dprint(2 * SCALE, bar + 2 + line, C_BLACK, "CPU %d MHz", f->Iphi_f / 1000000);
	dprint(2 * SCALE, bar + 2 + 2 * line, C_BLACK, "moves %d", moves);

	int s = 6 * SCALE;
	drect(bx, by, bx + s - 1, by + s - 1, BOX_COLOR);
	dtext(2 * SCALE, DHEIGHT - line, C_BLACK, "arrows, EXIT quits");
	dupdate();
}

int main(void)
{
	int s = 6 * SCALE;
	int bx = DWIDTH / 2, by = DHEIGHT / 2, moves = 0;
	draw(bx, by, moves);
	while(1) {
		key_event_t ev = getkey();
		if(ev.key == KEY_EXIT) break;
		int step = 4 * SCALE;
		if(ev.key == KEY_LEFT && bx >= step) bx -= step;
		else if(ev.key == KEY_RIGHT && bx + s + step <= DWIDTH) bx += step;
		else if(ev.key == KEY_UP && by >= step) by -= step;
		else if(ev.key == KEY_DOWN && by + s + step <= DHEIGHT) by += step;
		else continue;
		moves++;
		draw(bx, by, moves);
	}
	return 0;
}
