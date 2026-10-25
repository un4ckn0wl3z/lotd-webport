// BINKW32 (RAD Bink video: the intro and the cut-scenes in Cinematics/NTSC), as EngineEx uses it: BinkOpen, then
// each frame BinkWait / BinkDoFrame / BinkCopyToBufferRect into a locked texture / BinkNextFrame until FrameNum
// reaches Frames, then BinkClose and the script's "video complete" event.
//
// Video decoding is not done yet: a movie shows black for its first frame and ends, so the game goes on exactly as
// after a finished movie (EngineEx must see a successful open; a failed one fires its completion callback before
// it has stored its player object).
#include "host.h"
#include <stdio.h>

// the public head of RAD's BINK structure
#define BK_WIDTH 0
#define BK_HEIGHT 4
#define BK_FRAMES 8
#define BK_FRAMENUM 12
#define BK_LASTFRAMENUM 16
#define BK_FRAMERATE 20
#define BK_FRAMERATEDIV 24
#define BK_SIZE 0x400

static uint32_t now_ms(void) { return host_ticks(); }
static struct { uint32_t h, start, frames, rate, div; } movies[8];

static void b_BinkOpen(void) {
    const char *name = ARG(0) ? GSTR(ARG(0)) : "";
    char path[1024];
    uint8_t hdr[44] = {0};
    // a missing movie (the web build leaves Cinematics out) still opens, as one black 640x480 frame
    FILE *f = fs_resolve(name, path, sizeof path, 0) ? fopen(path, "rb") : NULL;
    if (f) { if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) memset(hdr, 0, sizeof hdr); fclose(f); }
    if (memcmp(hdr, "BIK", 3)) {
        memset(hdr, 0, sizeof hdr);
        hdr[8] = 1; hdr[20] = 640 & 255; hdr[21] = 640 >> 8; hdr[24] = 480 & 255; hdr[25] = 480 >> 8; hdr[28] = 30; hdr[32] = 1;
    }
    uint32_t frames = hdr[8] | hdr[9] << 8 | hdr[10] << 16 | (uint32_t)hdr[11] << 24;
    uint32_t w = hdr[20] | hdr[21] << 8, h = hdr[24] | hdr[25] << 8;
    uint32_t rate = hdr[28] | hdr[29] << 8 | hdr[30] << 16 | (uint32_t)hdr[31] << 24;
    uint32_t div = hdr[32] | hdr[33] << 8 | hdr[34] << 16 | (uint32_t)hdr[35] << 24;
    uint32_t b = heap_alloc(BK_SIZE);
    memset(G + b, 0, BK_SIZE);
    W32(b + BK_WIDTH, w); W32(b + BK_HEIGHT, h);
    W32(b + BK_FRAMES, 1);          // decoding not implemented: end after the first frame
    W32(b + BK_FRAMERATE, rate); W32(b + BK_FRAMERATEDIV, div ? div : 1);
    for (int i = 0; i < 8; i++) if (!movies[i].h) { movies[i].h = b; movies[i].start = now_ms(); movies[i].frames = frames; movies[i].rate = rate; movies[i].div = div; break; }
    host_log("BinkOpen(%s): %ux%u, %u frames at %u/%u fps (shown as one black frame)\n", name, w, h, frames, rate, div);
    SRET(b, 2);
}
static void b_BinkClose(void) {
    uint32_t b = ARG(0);
    for (int i = 0; i < 8; i++) if (movies[i].h == b) movies[i].h = 0;
    if (b) heap_free(b);
    SRET(0, 1);
}
static void b_BinkWait(void) { SRET(0, 1); }             // the next frame is due
static void b_BinkDoFrame(void) { SRET(0, 1); }
static void b_BinkNextFrame(void) {
    uint32_t b = ARG(0);
    if (b) { W32(b + BK_LASTFRAMENUM, R32(b + BK_FRAMENUM)); W32(b + BK_FRAMENUM, R32(b + BK_FRAMENUM) + 1); }
    SRET(0, 1);
}
// BinkCopyToBufferRect(bink, dest, pitch, height, destx, desty, srcx, srcy, srcw, srch, flags)
static void b_BinkCopyToBufferRect(void) {
    uint32_t dest = ARG(1), pitch = ARG(2), height = ARG(3);
    if (dest && pitch && height < 4096) memset(G + dest, 0, (size_t)pitch * height);
    SRET(0, 11);
}
static void b_ret0_1(void) { SRET(0, 1); }
static void b_ret1_1(void) { SRET(1, 1); }
static void b_ret0_2(void) { SRET(0, 2); }

const ShimDef shims_bink[] = {
    {"binkw32", "_BinkOpen@8", b_BinkOpen}, {"binkw32", "_BinkClose@4", b_BinkClose},
    {"binkw32", "_BinkDoFrame@4", b_BinkDoFrame}, {"binkw32", "_BinkNextFrame@4", b_BinkNextFrame},
    {"binkw32", "_BinkWait@4", b_BinkWait}, {"binkw32", "_BinkOpenDirectSound@4", b_ret1_1},
    {"binkw32", "_BinkSetSoundSystem@8", b_ret0_2}, {"binkw32", "_BinkSetSoundTrack@8", b_ret0_2},
    {"binkw32", "_BinkCopyToBufferRect@44", b_BinkCopyToBufferRect},
    {0, 0, 0}
};
static void unused(void) { (void)b_ret0_1; (void)now_ms; }
