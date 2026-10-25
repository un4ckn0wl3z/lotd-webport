// OpenAL for ALAudio.dll (UE2's audio driver loads OpenAL32.dll / DefOpenAL32.dll by name and takes every
// function with GetProcAddress; runtime.c hands it these). A small software mixer on SDL audio: positional
// sources with the inverse-distance-clamped model, pitch, looping and streaming buffer queues. No EAX.
//
// OpenAL functions are __cdecl: arguments at esp+4..., the caller pops them.
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define CRET(v) do { R.eax = (uint32_t)(v); R.esp += 4; return; } while (0)
#define CARG(n) R32(R.esp + 4 + 4 * (n))
#define FARG(n) ((float)F32(R.esp + 4 + 4 * (n)))

#define MIX_RATE 44100
#define MAXSRC 256
#define MAXBUF 4096
#define MAXQ 64

typedef struct { int used; int16_t *data; uint32_t frames; int channels, rate, bits; } ALBuf;
typedef struct {
    int used, state;            // AL_INITIAL/PLAYING/PAUSED/STOPPED
    uint32_t queue[MAXQ]; int nq, cur;    // buffer ids; cur = index playing
    double pos;                 // frame position in the current buffer
    float gain, pitch, pos3[3], vel[3], dir[3], ref_dist, rolloff, max_dist, min_gain, max_gain;
    int relative, looping, static_buf;
} ALSrc;
static ALBuf bufs[MAXBUF];
static ALSrc srcs[MAXSRC];
static float lis_pos[3], lis_vel[3], lis_at[3] = {0, 0, -1}, lis_up[3] = {0, 1, 0}, lis_gain = 1;
static uint32_t al_error, alc_error;
static uint32_t distance_model = 0xD002;
static SDL_AudioDeviceID dev;
static int opened;

#define AL_SOURCE_RELATIVE 0x202
#define AL_PITCH 0x1003
#define AL_POSITION 0x1004
#define AL_DIRECTION 0x1005
#define AL_VELOCITY 0x1006
#define AL_LOOPING 0x1007
#define AL_BUFFER 0x1009
#define AL_GAIN 0x100A
#define AL_MIN_GAIN 0x100D
#define AL_MAX_GAIN 0x100E
#define AL_ORIENTATION 0x100F
#define AL_SOURCE_STATE 0x1010
#define AL_INITIAL 0x1011
#define AL_PLAYING 0x1012
#define AL_PAUSED 0x1013
#define AL_STOPPED 0x1014
#define AL_BUFFERS_QUEUED 0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_REFERENCE_DISTANCE 0x1020
#define AL_ROLLOFF_FACTOR 0x1021
#define AL_MAX_DISTANCE 0x1023
#define AL_SEC_OFFSET 0x1024
#define AL_SAMPLE_OFFSET 0x1025
#define AL_INVALID_NAME 0xA001
#define AL_INVALID_ENUM 0xA002
#define AL_INVALID_VALUE 0xA003

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

// ---------------------------------------------------------------- mixer (audio thread)
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void src_gains(ALSrc *s, float *gl, float *gr) {
    float g = s->gain * lis_gain;
    float rel[3];
    for (int i = 0; i < 3; i++) rel[i] = s->relative ? s->pos3[i] : s->pos3[i] - lis_pos[i];
    float dist = sqrtf(dot3(rel, rel));
    if (distance_model) {
        float d = dist;
        if (d < s->ref_dist) d = s->ref_dist;
        if (s->max_dist > 0 && d > s->max_dist) d = s->max_dist;
        float den = s->ref_dist + s->rolloff * (d - s->ref_dist);
        if (den > 0) g *= s->ref_dist / den;
    }
    if (g < s->min_gain) g = s->min_gain;
    if (g > s->max_gain) g = s->max_gain;
    // pan: the source direction against the listener's right vector (at x up)
    float right[3] = { lis_at[1] * lis_up[2] - lis_at[2] * lis_up[1], lis_at[2] * lis_up[0] - lis_at[0] * lis_up[2],
                       lis_at[0] * lis_up[1] - lis_at[1] * lis_up[0] };
    float pan = 0;
    if (dist > 1e-3f) pan = dot3(rel, right) / dist;
    float a = (pan + 1) * 0.25f * 3.14159265f;
    *gl = g * cosf(a) * 1.41421356f * 0.70710678f;
    *gr = g * sinf(a) * 1.41421356f * 0.70710678f;
    if (*gl > g) *gl = g; if (*gr > g) *gr = g;
}
static void mix_source(ALSrc *s, float *out, int frames) {
    float gl, gr;
    src_gains(s, &gl, &gr);
    double step0 = s->pitch > 0 ? s->pitch : 1.0;
    for (int i = 0; i < frames; i++) {
        if (s->cur >= s->nq) { s->state = AL_STOPPED; return; }
        ALBuf *b = &bufs[s->queue[s->cur]];
        if (!b->used || !b->frames) {
            s->cur++; s->pos = 0;
            if (s->cur >= s->nq && s->looping) s->cur = 0;
            if (s->cur >= s->nq) { s->state = AL_STOPPED; return; }
            continue;
        }
        uint32_t k = (uint32_t)s->pos;
        if (k >= b->frames) {
            s->pos -= b->frames; s->cur++;
            if (s->cur >= s->nq) { if (s->looping) s->cur = 0; else { s->state = AL_STOPPED; return; } }
            i--; continue;
        }
        float l, r;
        if (b->channels == 2) { l = b->data[2 * k] / 32768.f; r = b->data[2 * k + 1] / 32768.f; }
        else { l = r = b->data[k] / 32768.f; }
        if (b->channels == 2 && !s->relative) { out[2 * i] += l * s->gain * lis_gain; out[2 * i + 1] += r * s->gain * lis_gain; }
        else { float m = b->channels == 2 ? (l + r) * 0.5f : l; out[2 * i] += m * gl; out[2 * i + 1] += m * gr; }
        s->pos += step0 * (double)b->rate / MIX_RATE;
    }
}
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int frames = len / 8;
    memset(out, 0, (size_t)len);
    for (int i = 0; i < MAXSRC; i++) if (srcs[i].used && srcs[i].state == AL_PLAYING) mix_source(&srcs[i], out, frames);
    for (int i = 0; i < frames * 2; i++) { float v = out[i]; out[i] = v > 1 ? 1 : v < -1 ? -1 : v; }
}

void audio_init(void) {
    if (opened) return;
    opened = 1;
    SDL_AudioSpec want = {0}, have;
    want.freq = MIX_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { host_log("audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
    host_log("audio: %d Hz, %d channels\n", have.freq, have.channels);
}
void audio_shutdown(void) { if (dev) SDL_CloseAudioDevice(dev); dev = 0; }

// ---------------------------------------------------------------- ALC
static uint32_t gstr(const char *s) {
    // strings handed to the game live forever; keep one copy each
    static struct { const char *s; uint32_t a; } cache[32]; static int n;
    for (int i = 0; i < n; i++) if (cache[i].s == s) return cache[i].a;
    uint32_t a = guest_static_alloc((uint32_t)strlen(s) + 2);
    memcpy(G + a, s, strlen(s) + 1);
    if (n < 32) { cache[n].s = s; cache[n].a = a; n++; }
    return a;
}
static const char dev_name[] = "Generic Software";
static void alcOpenDevice(void) { audio_init(); CRET(0x00380F00u); }
static void alcCloseDevice(void) { CRET(1); }
static void alcCreateContext(void) { CRET(0x00380F10u); }
static void alcMakeContextCurrent(void) { CRET(1); }
static void alc_void(void) { CRET(0); }
static void alcGetCurrentContext(void) { CRET(0x00380F10u); }
static void alcGetContextsDevice(void) { CRET(0x00380F00u); }
static void alcGetError(void) { uint32_t e = alc_error; alc_error = 0; CRET(e); }
static void alcGetString(void) {
    uint32_t p = CARG(1);
    switch (p) {
    case 0x1004: case 0x1005: CRET(gstr(dev_name));     // (DEFAULT_)DEVICE_SPECIFIER
    case 0x1006: CRET(gstr(""));                         // EXTENSIONS
    case 0: CRET(gstr("No Error"));
    }
    CRET(gstr(""));
}
static void alcGetIntegerv(void) {
    uint32_t p = CARG(1), size = CARG(2), out = CARG(3);
    if (!out || !size) CRET(0);
    switch (p) {
    case 0x1000: W32(out, 1); break;         // ALC_MAJOR_VERSION
    case 0x1001: W32(out, 0); break;         // ALC_MINOR_VERSION
    case 0x1002: W32(out, 1); break;         // ALC_ATTRIBUTES_SIZE
    case 0x1003: W32(out, 0); break;         // ALC_ALL_ATTRIBUTES
    case 0x1007: W32(out, MIX_RATE); break;  // ALC_FREQUENCY
    default: W32(out, 0); break;
    }
    CRET(0);
}

// ---------------------------------------------------------------- AL state
static void alGetError(void) { uint32_t e = al_error; al_error = 0; CRET(e); }
static void alGetString(void) {
    switch (CARG(0)) {
    case 0xB001: CRET(gstr("Creative Labs Inc."));
    case 0xB002: CRET(gstr("1.0"));
    case 0xB003: CRET(gstr("Software"));
    case 0xB004: CRET(gstr(""));
    }
    CRET(gstr(""));
}
static void alIsExtensionPresent(void) { CRET(0); }
static void alGetProcAddress(void) { CRET(0); }
static void alGetEnumValue(void) { CRET(0); }
static void alEnable(void) { CRET(0); }
static void alIsEnabled(void) { CRET(0); }
static void alGetInteger(void) { CRET(CARG(0) == 0xD000 ? distance_model : 0); }
static void alGetIntegerv(void) { if (CARG(1)) W32(CARG(1), CARG(0) == 0xD000 ? distance_model : 0); CRET(0); }
static void alGetFloat(void) { FPUSH(1.0); CRET(R.eax); }
static void alGetFloatv(void) { if (CARG(1)) WF32(CARG(1), 1.0); CRET(0); }
static void alGetDouble(void) { FPUSH(1.0); CRET(R.eax); }
static void alGetDoublev(void) { if (CARG(1)) WF64(CARG(1), 1.0); CRET(0); }
static void alDistanceModel(void) { distance_model = CARG(0); CRET(0); }
static void alDoppler(void) { CRET(0); }

// listener
static void listener3(uint32_t p, float x, float y, float z) {
    if (p == AL_POSITION) { lis_pos[0] = x; lis_pos[1] = y; lis_pos[2] = z; }
    else if (p == AL_VELOCITY) { lis_vel[0] = x; lis_vel[1] = y; lis_vel[2] = z; }
}
static void alListenerf(void) { if (CARG(0) == AL_GAIN) lis_gain = FARG(1); CRET(0); }
static void alListeneri(void) { CRET(0); }
static void alListener3f(void) { lock(); listener3(CARG(0), FARG(1), FARG(2), FARG(3)); unlock(); CRET(0); }
static void alListenerfv(void) {
    uint32_t p = CARG(0), v = CARG(1);
    lock();
    if (p == AL_ORIENTATION) {
        for (int i = 0; i < 3; i++) { lis_at[i] = (float)F32(v + 4 * i); lis_up[i] = (float)F32(v + 12 + 4 * i); }
    } else if (p == AL_GAIN) lis_gain = (float)F32(v);
    else listener3(p, (float)F32(v), (float)F32(v + 4), (float)F32(v + 8));
    unlock();
    CRET(0);
}
static void alGetListenerf(void) { if (CARG(1)) WF32(CARG(1), CARG(0) == AL_GAIN ? lis_gain : 0); CRET(0); }
static void alGetListeneri(void) { if (CARG(1)) W32(CARG(1), 0); CRET(0); }
static void alGetListener3f(void) {
    float *v = CARG(0) == AL_POSITION ? lis_pos : lis_vel;
    for (int i = 0; i < 3; i++) if (CARG(1 + i)) WF32(CARG(1 + i), v[i]);
    CRET(0);
}
static void alGetListenerfv(void) {
    uint32_t p = CARG(0), out = CARG(1);
    if (p == AL_ORIENTATION) { for (int i = 0; i < 3; i++) { WF32(out + 4 * i, lis_at[i]); WF32(out + 12 + 4 * i, lis_up[i]); } }
    else if (p == AL_GAIN) WF32(out, lis_gain);
    else { float *v = p == AL_POSITION ? lis_pos : lis_vel; for (int i = 0; i < 3; i++) WF32(out + 4 * i, v[i]); }
    CRET(0);
}

// sources (ids are index + 1)
static ALSrc *getsrc(uint32_t id) {
    if (id < 1 || id > MAXSRC || !srcs[id - 1].used) { al_error = AL_INVALID_NAME; return NULL; }
    return &srcs[id - 1];
}
static void src_reset(ALSrc *s) {
    memset(s, 0, sizeof *s);
    s->used = 1; s->state = AL_INITIAL; s->gain = 1; s->pitch = 1; s->ref_dist = 1; s->rolloff = 1;
    s->max_dist = 3.4e38f; s->max_gain = 1;
}
static void alGenSources(void) {
    uint32_t n = CARG(0), out = CARG(1);
    lock();
    for (uint32_t k = 0; k < n; k++) {
        int i;
        for (i = 0; i < MAXSRC && srcs[i].used; i++) {}
        if (i == MAXSRC) { al_error = 0xA005; break; }
        src_reset(&srcs[i]);
        W32(out + 4 * k, i + 1);
    }
    unlock();
    CRET(0);
}
static void alDeleteSources(void) {
    uint32_t n = CARG(0), ids = CARG(1);
    lock();
    for (uint32_t k = 0; k < n; k++) { ALSrc *s = getsrc(R32(ids + 4 * k)); if (s) s->used = 0; }
    unlock();
    CRET(0);
}
static void alIsSource(void) { uint32_t id = CARG(0); CRET(id >= 1 && id <= MAXSRC && srcs[id - 1].used); }
static void set_buffer(ALSrc *s, uint32_t b) {
    s->nq = 0; s->cur = 0; s->pos = 0;
    if (b) { s->queue[0] = b; s->nq = 1; s->static_buf = 1; }
}
static void alSourcei(void) {
    uint32_t p = CARG(1), v = CARG(2);
    lock();
    ALSrc *s = getsrc(CARG(0));
    if (s) switch (p) {
    case AL_BUFFER: set_buffer(s, v); break;
    case AL_LOOPING: s->looping = v != 0; break;
    case AL_SOURCE_RELATIVE: s->relative = v != 0; break;
    case AL_REFERENCE_DISTANCE: s->ref_dist = (float)(int32_t)v; break;
    case AL_MAX_DISTANCE: s->max_dist = (float)(int32_t)v; break;
    case AL_ROLLOFF_FACTOR: s->rolloff = (float)(int32_t)v; break;
    }
    unlock();
    CRET(0);
}
static void source_f(ALSrc *s, uint32_t p, float v) {
    switch (p) {
    case AL_GAIN: s->gain = v; break;
    case AL_PITCH: s->pitch = v; break;
    case AL_REFERENCE_DISTANCE: s->ref_dist = v; break;
    case AL_ROLLOFF_FACTOR: s->rolloff = v; break;
    case AL_MAX_DISTANCE: s->max_dist = v; break;
    case AL_MIN_GAIN: s->min_gain = v; break;
    case AL_MAX_GAIN: s->max_gain = v; break;
    case AL_SEC_OFFSET:
        if (s->cur < s->nq) s->pos = v * bufs[s->queue[s->cur]].rate;
        break;
    }
}
static void source_3f(ALSrc *s, uint32_t p, float x, float y, float z) {
    float *d = p == AL_POSITION ? s->pos3 : p == AL_VELOCITY ? s->vel : p == AL_DIRECTION ? s->dir : NULL;
    if (d) { d[0] = x; d[1] = y; d[2] = z; }
}
static void alSourcef(void) { lock(); ALSrc *s = getsrc(CARG(0)); if (s) source_f(s, CARG(1), FARG(2)); unlock(); CRET(0); }
static void alSource3f(void) { lock(); ALSrc *s = getsrc(CARG(0)); if (s) source_3f(s, CARG(1), FARG(2), FARG(3), FARG(4)); unlock(); CRET(0); }
static void alSourcefv(void) {
    uint32_t p = CARG(1), v = CARG(2);
    lock();
    ALSrc *s = getsrc(CARG(0));
    if (s) {
        if (p == AL_POSITION || p == AL_VELOCITY || p == AL_DIRECTION) source_3f(s, p, (float)F32(v), (float)F32(v + 4), (float)F32(v + 8));
        else source_f(s, p, (float)F32(v));
    }
    unlock();
    CRET(0);
}
static int processed(ALSrc *s) {
    if (s->state == AL_STOPPED) return s->nq;
    if (s->state == AL_INITIAL) return 0;
    return s->looping ? 0 : s->cur;
}
static void alGetSourcei(void) {
    uint32_t p = CARG(1), out = CARG(2);
    lock();
    ALSrc *s = getsrc(CARG(0));
    uint32_t v = 0;
    if (s) switch (p) {
    case AL_SOURCE_STATE: v = s->state; break;
    case AL_BUFFERS_QUEUED: v = s->nq; break;
    case AL_BUFFERS_PROCESSED: v = processed(s); break;
    case AL_LOOPING: v = s->looping; break;
    case AL_BUFFER: v = s->nq ? s->queue[s->cur < s->nq ? s->cur : s->nq - 1] : 0; break;
    case AL_SOURCE_RELATIVE: v = s->relative; break;
    case AL_SAMPLE_OFFSET: v = (uint32_t)s->pos; break;
    }
    unlock();
    if (out) W32(out, v);
    CRET(0);
}
static void alGetSourcef(void) {
    uint32_t p = CARG(1), out = CARG(2);
    ALSrc *s = getsrc(CARG(0));
    float v = 0;
    if (s) switch (p) {
    case AL_GAIN: v = s->gain; break;
    case AL_PITCH: v = s->pitch; break;
    case AL_SEC_OFFSET: v = s->cur < s->nq && bufs[s->queue[s->cur]].rate ? (float)(s->pos / bufs[s->queue[s->cur]].rate) : 0; break;
    }
    if (out) WF32(out, v);
    CRET(0);
}
static void alGetSourcefv(void) {
    uint32_t p = CARG(1), out = CARG(2);
    ALSrc *s = getsrc(CARG(0));
    if (s && out) {
        float *d = p == AL_POSITION ? s->pos3 : p == AL_VELOCITY ? s->vel : p == AL_DIRECTION ? s->dir : NULL;
        if (d) for (int i = 0; i < 3; i++) WF32(out + 4 * i, d[i]);
        else if (p == AL_GAIN) WF32(out, s->gain);
    }
    CRET(0);
}
static void play(ALSrc *s) {
    if (s->state == AL_PAUSED) { s->state = AL_PLAYING; return; }
    s->cur = 0; s->pos = 0;
    s->state = s->nq ? AL_PLAYING : AL_STOPPED;
}
static void stop(ALSrc *s) { s->state = AL_STOPPED; if (s->nq) s->cur = s->nq; }
static void rewind_(ALSrc *s) { s->state = AL_INITIAL; s->cur = 0; s->pos = 0; }
static void pause_(ALSrc *s) { if (s->state == AL_PLAYING) s->state = AL_PAUSED; }
#define SRCOP(name, fn) \
    static void name(void) { lock(); ALSrc *s = getsrc(CARG(0)); if (s) fn(s); unlock(); CRET(0); } \
    static void name##v(void) { uint32_t n = CARG(0), ids = CARG(1); lock(); \
        for (uint32_t k = 0; k < n; k++) { ALSrc *s = getsrc(R32(ids + 4 * k)); if (s) fn(s); } unlock(); CRET(0); }
SRCOP(alSourcePlay, play)
SRCOP(alSourceStop, stop)
SRCOP(alSourceRewind, rewind_)
SRCOP(alSourcePause, pause_)
static void alSourceQueueBuffers(void) {
    uint32_t n = CARG(1), ids = CARG(2);
    lock();
    ALSrc *s = getsrc(CARG(0));
    if (s) {
        if (s->static_buf) { s->nq = 0; s->cur = 0; s->static_buf = 0; }
        for (uint32_t k = 0; k < n && s->nq < MAXQ; k++) s->queue[s->nq++] = R32(ids + 4 * k);
        if (s->state == AL_STOPPED && s->cur < s->nq) { /* stays stopped until played again */ }
    }
    unlock();
    CRET(0);
}
static void alSourceUnqueueBuffers(void) {
    uint32_t n = CARG(1), ids = CARG(2);
    lock();
    ALSrc *s = getsrc(CARG(0));
    if (s) {
        int done = processed(s);
        if ((int)n > done) { al_error = AL_INVALID_VALUE; n = (uint32_t)done; }
        for (uint32_t k = 0; k < n; k++) W32(ids + 4 * k, s->queue[k]);
        memmove(s->queue, s->queue + n, (size_t)(s->nq - (int)n) * sizeof(uint32_t));
        s->nq -= (int)n;
        s->cur -= (int)n; if (s->cur < 0) s->cur = 0;
    }
    unlock();
    CRET(0);
}

// buffers (ids are index + 1; bufs[0] unused)
static void alGenBuffers(void) {
    uint32_t n = CARG(0), out = CARG(1);
    lock();
    for (uint32_t k = 0; k < n; k++) {
        int i;
        for (i = 1; i < MAXBUF && bufs[i].used; i++) {}
        if (i == MAXBUF) { al_error = 0xA005; break; }
        memset(&bufs[i], 0, sizeof bufs[i]); bufs[i].used = 1;
        W32(out + 4 * k, i);
    }
    unlock();
    CRET(0);
}
static void alDeleteBuffers(void) {
    uint32_t n = CARG(0), ids = CARG(1);
    lock();
    for (uint32_t k = 0; k < n; k++) {
        uint32_t id = R32(ids + 4 * k);
        if (id && id < MAXBUF && bufs[id].used) { free(bufs[id].data); bufs[id].data = NULL; bufs[id].used = 0; }
    }
    unlock();
    CRET(0);
}
static void alIsBuffer(void) { uint32_t id = CARG(0); CRET(id && id < MAXBUF && bufs[id].used); }
static void alBufferData(void) {
    uint32_t id = CARG(0), fmt = CARG(1), data = CARG(2), size = CARG(3), freq = CARG(4);
    if (!id || id >= MAXBUF || !bufs[id].used) { al_error = AL_INVALID_NAME; CRET(0); }
    int ch = (fmt == 0x1102 || fmt == 0x1103) ? 2 : 1, b16 = (fmt == 0x1101 || fmt == 0x1103);
    uint32_t frames = size / (uint32_t)(ch * (b16 ? 2 : 1));
    int16_t *d = malloc((size_t)frames * ch * 2 + 2);
    for (uint32_t i = 0; i < frames * (uint32_t)ch; i++)
        d[i] = b16 ? (int16_t)R16(data + 2 * i) : (int16_t)(((int)R8(data + i) - 128) << 8);
    lock();
    free(bufs[id].data);
    bufs[id].data = d; bufs[id].frames = frames; bufs[id].channels = ch; bufs[id].rate = (int)freq; bufs[id].bits = b16 ? 16 : 8;
    unlock();
    CRET(0);
}
static void alGetBufferi(void) {
    uint32_t id = CARG(0), p = CARG(1), out = CARG(2), v = 0;
    if (id && id < MAXBUF && bufs[id].used) {
        ALBuf *b = &bufs[id];
        switch (p) {
        case 0x2001: v = (uint32_t)b->rate; break;
        case 0x2002: v = (uint32_t)b->bits; break;
        case 0x2003: v = (uint32_t)b->channels; break;
        case 0x2004: v = b->frames * (uint32_t)b->channels * (uint32_t)(b->bits / 8); break;
        }
    }
    if (out) W32(out, v);
    CRET(0);
}
static void alGetBufferf(void) { if (CARG(2)) WF32(CARG(2), 0); CRET(0); }

#define A(n) {"OpenAL32", #n, n}
const ShimDef shims_openal[] = {
    A(alcOpenDevice), A(alcCloseDevice), A(alcCreateContext), A(alcMakeContextCurrent), A(alcGetCurrentContext),
    A(alcGetContextsDevice), A(alcGetError), A(alcGetString), A(alcGetIntegerv),
    {"OpenAL32", "alcProcessContext", alc_void}, {"OpenAL32", "alcSuspendContext", alc_void},
    {"OpenAL32", "alcDestroyContext", alc_void},
    A(alGetError), A(alGetString), A(alIsExtensionPresent), A(alGetProcAddress), A(alGetEnumValue), A(alEnable),
    {"OpenAL32", "alDisable", alEnable}, A(alIsEnabled), {"OpenAL32", "alGetBoolean", alIsEnabled},
    {"OpenAL32", "alGetBooleanv", alGetIntegerv}, A(alGetInteger), A(alGetIntegerv), A(alGetFloat), A(alGetFloatv),
    A(alGetDouble), A(alGetDoublev), A(alDistanceModel), {"OpenAL32", "alDopplerFactor", alDoppler},
    {"OpenAL32", "alDopplerVelocity", alDoppler},
    A(alListenerf), A(alListeneri), A(alListener3f), A(alListenerfv), A(alGetListenerf), A(alGetListeneri),
    A(alGetListener3f), A(alGetListenerfv),
    A(alGenSources), A(alDeleteSources), A(alIsSource), A(alSourcei), A(alSourcef), A(alSource3f), A(alSourcefv),
    A(alGetSourcei), A(alGetSourcef), A(alGetSourcefv), A(alSourcePlay), A(alSourcePlayv), A(alSourceStop),
    A(alSourceStopv), A(alSourceRewind), A(alSourceRewindv), A(alSourcePause), A(alSourcePausev),
    A(alSourceQueueBuffers), A(alSourceUnqueueBuffers),
    A(alGenBuffers), A(alDeleteBuffers), A(alIsBuffer), A(alBufferData), A(alGetBufferi), A(alGetBufferf),
    {0, 0, 0}
};
