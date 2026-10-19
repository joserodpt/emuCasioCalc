/* The calculator's sound (the serial port's TX pin, engine/audio.c) through
 * the default output device, and --audio-out's WAV file. */
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "view.h"

/* Played sound is kept between these amounts ahead of the speakers: the
 * emulator runs in 10 ms slices, at a pace that wobbles a little. */
#define QUEUE_MIN_MS 40
#define QUEUE_MAX_MS 150

bool view_sound = true;

static SDL_AudioStream *stream;
static bool stream_failed;
static double stream_speed = 1;

static FILE *wav;
static char wav_path[1024];
static uint32_t wav_samples;

static bool open_stream(void)
{
    if (stream || stream_failed)
        return stream != NULL;
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, AUDIO_RATE};
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        fprintf(stderr, "sound: %s\n", SDL_GetError());
        stream_failed = true; /* once: the emulator goes on without it */
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return true;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* The 44-byte header of a mono 16-bit PCM WAV file. */
static void wav_header(uint8_t *h, uint32_t samples)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + samples * 2);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); /* PCM */
    put16(h + 22, 1); /* mono */
    put32(h + 24, AUDIO_RATE);
    put32(h + 28, AUDIO_RATE * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, samples * 2);
}

bool platform_sound_record(const char *path)
{
    uint8_t h[44];
    if (!(wav = fopen(path, "wb")))
        return false;
    snprintf(wav_path, sizeof wav_path, "%s", path);
    wav_header(h, 0);
    fwrite(h, 1, sizeof h, wav);
    return true;
}

bool platform_sound_wanted(void)
{
    return wav || (!view_headless && view_sound && !stream_failed);
}

static void record(const float *pcm, int n)
{
    uint8_t buf[2 * 1024];
    while (n > 0) {
        int k = n < 1024 ? n : 1024;
        for (int i = 0; i < k; i++) {
            float v = pcm[i] < -1 ? -1 : pcm[i] > 1 ? 1 : pcm[i];
            put16(buf + 2 * i, (uint16_t)(int16_t)(v * 32767));
        }
        fwrite(buf, 2, (size_t)k, wav);
        wav_samples += (uint32_t)k;
        pcm += k;
        n -= k;
    }
}

void platform_sound(const float *pcm, int n, double speed)
{
    if (wav)
        record(pcm, n);
    if (view_headless || !view_sound || speed <= 0 || !open_stream())
        return;
    if (speed != stream_speed) { /* slower or faster: the pitch follows, as a tape's */
        SDL_SetAudioStreamFrequencyRatio(stream, (float)speed);
        stream_speed = speed;
    }
    /* What is queued, in ms of real time */
    double ms = SDL_GetAudioStreamQueued(stream) / (double)sizeof(float) / (AUDIO_RATE * speed) * 1000;
    if (ms > QUEUE_MAX_MS)
        return; /* the emulator is ahead of the speakers: skip a slice */
    if (ms <= 0) { /* starting, or after a gap: a cushion of silence first */
        static const float silence[AUDIO_RATE * QUEUE_MIN_MS / 1000];
        SDL_PutAudioStreamData(stream, silence, sizeof silence);
    }
    SDL_PutAudioStreamData(stream, pcm, n * (int)sizeof(float));
}

void sound_stop(void)
{
    if (stream)
        SDL_DestroyAudioStream(stream);
    stream = NULL;
    if (wav) {
        uint8_t h[44];
        wav_header(h, wav_samples);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, sizeof h, wav);
        fclose(wav);
        wav = NULL;
        printf("saved %s (%.1f s of sound)\n", wav_path, (double)wav_samples / AUDIO_RATE);
    }
}
