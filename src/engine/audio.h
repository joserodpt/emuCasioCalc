#ifndef EMU_AUDIO_H
#define EMU_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The sound of the serial port's TX pin (the 2.5 mm link jack). The pin is
 * a 1-bit output, so add-ins make tones by switching it (square waves) or
 * by streaming a delta-sigma bit pattern through it. We record when it
 * changes, in CPU cycles, and turn that into PCM the way a resistor and a
 * capacitor on the jack would:
 *
 * - each sub-sample (4 x AUDIO_RATE) is the exact fraction of its time the
 *   pin was high (a box filter: the delta-sigma decoder);
 * - a 2nd-order low-pass at 15 kHz, then every 4th sub-sample is kept;
 * - a DC blocker at about 10 Hz (the pin idles at 3.3 V).
 *
 * While the pin has not changed for a while (AUDIO_IDLE_MS), nothing is
 * produced: add-ins that never touch it cost nothing.
 */
#define AUDIO_RATE 48000
#define AUDIO_OVERSAMPLE 4
#define AUDIO_IDLE_MS 250
#define AUDIO_RING 32768 /* samples buffered for the host, a power of 2 */

typedef struct {
    bool enabled;     /* someone listens (the window, --audio-out) */
    bool active;      /* the pin changed in the last AUDIO_IDLE_MS */
    int level;        /* the pin, 0 or 1 */
    uint32_t hz;      /* CPU clock the cycles count */
    uint64_t last;    /* cycle up to which the pin has been integrated */
    uint64_t last_edge;
    /* the sub-sample being filled, in units of 1 / (AUDIO_RATE * AUDIO_OVERSAMPLE)
     * cycle: it is hz units long */
    uint64_t fill, high;
    int sub;          /* sub-samples of the output sample so far */
    double b0, b1, b2, a1, a2, z1, z2; /* the low-pass (transposed direct form II) */
    double dc_x, dc_y;                 /* the DC blocker */
    float ring[AUDIO_RING];
    uint32_t head, tail;
} Audio;

void audio_init(Audio *a);
/* Listening or not; turning it back on doesn't replay the time between. */
static inline void audio_enable(Audio *a, bool on)
{
    if (a->enabled != on)
        a->active = false;
    a->enabled = on;
}
/* The pin is at level from cycle on (cycle is clamped to be in order).
 * hz: the CPU clock. */
void audio_edge(Audio *a, uint64_t cycle, int level, uint32_t hz);
/* Produces the samples up to cycle now. */
void audio_flush(Audio *a, uint64_t now);
/* Takes up to max samples (-1 to 1); returns how many. */
int audio_take(Audio *a, float *out, int max);

#endif
