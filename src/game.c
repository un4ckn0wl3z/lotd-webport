// Land of the Dead glue: the frame loop, the engine's helper threads.
//
// LOTD.exe's MainLoop (0x1090ce70) builds a loop object on its stack at [ebp-0x6c] and does
//     while (*GIsRunning && !*GIsRequestingExit) FrameLoop::Tick(&loop);     // 0x1090117c, __thiscall
// The recompiler hook replaces the loop head (0x1090cec5) and resumes at 0x1090cee4, after the loop
// (recomp_config.json "lotd" section). GIsRunning and GIsRequestingExit are Core.dll data the exe imports.
#include "host.h"

#define IMP_GISRUNNING          0x10959b18u     // LOTD.exe IAT slot -> Core!GIsRunning
#define IMP_GISREQUESTINGEXIT   0x10959b1cu     // LOTD.exe IAT slot -> Core!GIsRequestingExit
#define FRAME_TICK              0x1090117cu
#define LOOP_OBJ_OFS            0x6cu

// Core.dll's file streaming thread (FFileStream, music): 0x5520f0 loops over the streams and reads the next chunk
// of each one that asks for it, until FFileStream::Destroyed is set. Its work is done here instead, each frame and
// whenever the game sleeps or waits.
#define CORE_STREAM_THREAD      0x005520f0u
#define FS_STREAMS              0x005f7d08u     // FStream* (0x28 bytes each)
#define FS_MAXSTREAMS           0x005f7d10u
#define FS_DESTROYED            0x005f7d14u
#define G_FILESTREAM            0x005b2158u
#define FS_READ                 0x004e1afau     // FFileStream::Read(this, int stream, int bytes)
#define FS_DESTROY              0x004e34dbu     // FFileStream::Destroy(this, int stream)

static uint32_t loop_obj;
static int stream_thread;

int game_thread_kind(uint32_t start) {
    if (start == CORE_STREAM_THREAD) { stream_thread = 1; return 1; }
    return 0;
}

void game_background_tick(void) {
    static int busy;
    if (!stream_thread || busy) return;
    busy = 1;
    uint32_t gfs = R32(G_FILESTREAM), n = R32(FS_MAXSTREAMS);
    if (R32(FS_DESTROYED) == 1) {
        for (uint32_t i = 0; i < n; i++)
            if (R32(R32(FS_STREAMS) + i * 0x28 + 4)) guest_call_this(FS_DESTROY, gfs, 1, i);
        W32(FS_DESTROYED, 2);
        stream_thread = 0;
    } else if (!R32(FS_DESTROYED)) {
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 64; k++) {
                uint32_t s = R32(FS_STREAMS) + i * 0x28;
                if (!R32(s + 0x18) || !R32(s + 0x14)) break;
                guest_call_this(FS_READ, gfs, 2, i, R32(s + 0x10));
                s = R32(FS_STREAMS) + i * 0x28;
                W32(s + 0x14, R32(s + 0x14) - 1);
                W32(s, R32(s) + R32(s + 0x10));
            }
        }
    }
    busy = 0;
}

static int running(void) {
    return R32(R32(IMP_GISRUNNING)) && !R32(R32(IMP_GISREQUESTINGEXIT));
}

// one iteration of the game's main loop; nonzero when the game wants to quit
static uint32_t frames;
uint32_t host_frame_count(void) { return frames; }
int port_frame_once(void) {
    if (!running()) return 1;
    frames++;
    game_background_tick();
    guest_call_this(FRAME_TICK, loop_obj, 0);
    if (g_unwind) host_fatal("exception escaped the frame (unwinding to %08x)", g_unwind_target);
    return !running();
}

// recompiler hook (main.c: host_frame_loop calls this first)
void game_loop_begin(void) {
    loop_obj = R.ebp - LOOP_OBJ_OFS;
    host_log("main loop: loop object %08x\n", loop_obj);
}
void port_after_loop(void) {}
