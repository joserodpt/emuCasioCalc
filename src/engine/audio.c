#include "audio.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
#define CUTOFF_HZ 15000.0
#define DC_POLE 0.9987 /* about 10 Hz at 48 kHz */
#define GAIN 0.5       /* a full-scale square wave peaks at +-0.5 */

void audio_init(Audio *a)
{
    memset(a, 0, sizeof *a);
    a->level = 1; /* the TX line idles high */
    /* Butterworth low-pass (RBJ cookbook), at the sub-sample rate */
    double fs = (double)AUDIO_RATE * AUDIO_OVERSAMPLE;
    double w = 2 * PI * CUTOFF_HZ / fs, c = cos(w), alpha = sin(w) / (2 * sqrt(0.5));
    double a0 = 1 + alpha;
    a->b0 = (1 - c) / 2 / a0;
    a->b1 = (1 - c) / a0;
    a->b2 = a->b0;
    a->a1 = -2 * c / a0;
    a->a2 = (1 - alpha) / a0;
}

/* The filters as if the pin had always been at level: starting to listen
 * (or again after a silence) doesn't click. */
static void settle(Audio *a)
{
    double v = a->level ? 1 : -1;
    a->z2 = (a->b2 - a->a2) * v;
    a->z1 = (a->b1 - a->a1) * v + a->z2;
    a->dc_x = v;
    a->dc_y = 0;
    a->fill = a->high = 0;
    a->sub = 0;
}

/* One sub-sample: x is the part of it the pin was high, 0 to 1. */
static void sub_sample(Audio *a, double x)
{
    double v = 2 * x - 1;
    double y = a->b0 * v + a->z1;
    a->z1 = a->b1 * v - a->a1 * y + a->z2;
    a->z2 = a->b2 * v - a->a2 * y;
    if (++a->sub < AUDIO_OVERSAMPLE)
        return;
    a->sub = 0;
    a->dc_y = y - a->dc_x + DC_POLE * a->dc_y;
    a->dc_x = y;
    if (a->head - a->tail < AUDIO_RING) /* else nobody takes them: drop */
        a->ring[a->head++ & (AUDIO_RING - 1)] = (float)(a->dc_y * GAIN);
}

/* Integrates the pin from a->last to cycle. */
static void advance(Audio *a, uint64_t cycle)
{
    if (cycle <= a->last)
        return;
    const uint64_t rate = (uint64_t)AUDIO_RATE * AUDIO_OVERSAMPLE;
    uint64_t units = (cycle - a->last) * rate;
    a->last = cycle;
    while (units) {
        uint64_t room = a->hz - a->fill;
        if (units < room) {
            a->fill += units;
            if (a->level)
                a->high += units;
            return;
        }
        if (a->level)
            a->high += room;
        sub_sample(a, (double)a->high / a->hz);
        a->fill = a->high = 0;
        units -= room;
    }
}

void audio_edge(Audio *a, uint64_t cycle, int level, uint32_t hz)
{
    if (!a->enabled)
        return;
    level = level != 0;
    if (!a->active || hz != a->hz) {
        if (level == a->level && !a->active)
            return;
        a->hz = hz;
        a->last = cycle;
        settle(a);
        a->active = true;
    }
    if (cycle < a->last)
        cycle = a->last;
    advance(a, cycle);
    a->level = level;
    a->last_edge = cycle;
}

void audio_flush(Audio *a, uint64_t now)
{
    if (!a->enabled || !a->active)
        return;
    advance(a, now);
    uint64_t idle = (uint64_t)a->hz / 1000 * AUDIO_IDLE_MS;
    if (now > a->last_edge && now - a->last_edge > idle)
        a->active = false;
}

int audio_take(Audio *a, float *out, int max)
{
    int n = 0;
    while (n < max && a->tail != a->head)
        out[n++] = a->ring[a->tail++ & (AUDIO_RING - 1)];
    return n;
}
