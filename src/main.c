// Host entry point: init SDL, load the game's modules, run the recompiled exe.
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern char g_save_dir[512];
extern char g_cmdline_extra[512];
int g_fs_dirty;

uint32_t host_ticks(void) { return SDL_GetTicks(); }
uint64_t host_time_ns(void) {
    static uint64_t f;
    if (!f) f = SDL_GetPerformanceFrequency();
    uint64_t c = SDL_GetPerformanceCounter();
    return (uint64_t)((double)c * (1e9 / (double)f));
}
void host_yield(void) {
#ifndef __EMSCRIPTEN__
    SDL_Delay(1);
#endif
}
void host_sleep(uint32_t ms) {
#ifndef __EMSCRIPTEN__
    if (ms > 200) ms = 200;
    if (ms) SDL_Delay(ms);
#else
    (void)ms;
#endif
}

void host_shutdown(void) {
    static int done;
    if (done) return;
    done = 1;
    host_log("shutdown: guest heap used %u KB, peak %u KB\n", heap_used_bytes() >> 10, heap_peak_bytes() >> 10);
    audio_shutdown();
    SDL_Quit();
}

void host_sync_fs(void) {
#ifdef __EMSCRIPTEN__
    if (!g_fs_dirty) return;
    g_fs_dirty = 0;
    EM_ASM(FS.syncfs(false, function (err) { if (err) console.log('save sync failed', err); }););
#endif
}

void host_exit_game(int code) {
    host_sync_fs();
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
    EM_ASM(if (Module.onGameExit) Module.onGameExit(););
    emscripten_force_exit(code);
#endif
    host_shutdown();
    exit(code);
}

// one iteration of the game's main loop, timed
static int tact_once(void) {
    uint64_t t0 = SDL_GetPerformanceCounter();
    int r = port_frame_once();
    static double acc, worst; static uint32_t n;
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    acc += ms; if (ms > worst) worst = ms;
#ifdef __EMSCRIPTEN__
    EM_ASM({ Module.tacts = (Module.tacts || 0) + 1; Module.lastTactMs = $0; }, ms);
#endif
    if (++n == 600) {
        host_log("perf: frame avg %.2f ms, worst %.1f ms, guest heap %u KB (peak %u KB)\n",
                 acc / n, worst, heap_used_bytes() >> 10, heap_peak_bytes() >> 10);
        acc = worst = 0; n = 0;
    }
    return r;
}

#ifdef __EMSCRIPTEN__
static double last_frame_ms;
static int loop_running;
static void em_frame(void) {
    last_frame_ms = emscripten_get_now();
    if (tact_once()) {
        host_log("game requested exit\n");
        host_exit_game(0);
        return;
    }
    static uint32_t last_sync;
    uint32_t now = SDL_GetTicks();
    if (now - last_sync > 2000) { last_sync = now; host_sync_fs(); }
}
// A background tab gets no animation frames. Online, a host that stops would freeze every other player, so the page
// calls this from a worker timer (web/shell.html) and it runs a frame whenever the animation frames have stopped.
EMSCRIPTEN_KEEPALIVE void port_background_tick(void) {
    if (loop_running && emscripten_get_now() - last_frame_ms > 100) em_frame();
}
#endif

// recompiler hook target: replaces the game's main loop (src/game.c, recomp_config.json)
void game_loop_begin(void);
void host_frame_loop(void) {
    game_loop_begin();
    host_log("entering main loop\n");
#ifdef __EMSCRIPTEN__
    loop_running = 1;
    emscripten_set_main_loop(em_frame, 0, 1);
#else
    while (!tact_once()) {}
    port_after_loop();
#endif
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

// PORT_WATCHDOG=<seconds> (native): dump where the guest is after that long, then exit. For hangs.
#ifndef __EMSCRIPTEN__
static int watchdog(void *arg) {
    int secs = (int)(intptr_t)arg;
    for (int i = 0; i < 3; i++) {
        SDL_Delay((uint32_t)secs * 1000 / 3);
        void host_recent_dump(void);
        host_log("watchdog sample %d (frame %u):\n", i, host_frame_count());
        host_recent_dump();
        host_backtrace();
    }
    void host_call_stats(void);
    host_call_stats();
    host_log("watchdog: exiting\n");
    exit(3);
    return 0;
}
#endif

// Arguments (native): [game dir] [save dir] [-- extra game command line]. The web page passes the same through
// callMain. PORT_ARGS adds to the game's command line too (e.g. PORT_ARGS="CH01 -log").
int main(int argc, char **argv) {
    char *pos[4] = {0};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            for (int k = i + 1; k < argc; k++) {
                size_t l = strlen(g_cmdline_extra);
                snprintf(g_cmdline_extra + l, sizeof g_cmdline_extra - l, "%s%s", l ? " " : "", argv[k]);
            }
            break;
        }
        if (npos < 4) pos[npos++] = argv[i];
    }
    if (getenv("PORT_ARGS")) {
        size_t l = strlen(g_cmdline_extra);
        snprintf(g_cmdline_extra + l, sizeof g_cmdline_extra - l, "%s%s", l ? " " : "", getenv("PORT_ARGS"));
    }
#ifdef __EMSCRIPTEN__
    snprintf(g_game_dir, sizeof g_game_dir, "/game");
    snprintf(g_save_dir, sizeof g_save_dir, "/save");
#else
    const char *cands[] = { pos[0], getenv(GAME_DIR_ENV), ".", "..", "../game", "../../game" };
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        if (!cands[i]) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/" GAME_EXE, cands[i]);
        if (file_exists(p)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", cands[i]); break; }
    }
    if (pos[1]) snprintf(g_save_dir, sizeof g_save_dir, "%s", pos[1]);
#endif
    host_log("game dir: %s, save dir: %s, command line: %s\n", g_game_dir, g_save_dir, g_cmdline_extra);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        host_log("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    char sysdir[1024];
    snprintf(sysdir, sizeof sysdir, "%s/" GAME_SYSDIR, g_game_dir);
    if (!runtime_init(sysdir)) {
        host_log("Cannot load the game from %s. Pass the game directory as the first argument.\n", sysdir);
        return 1;
    }
#ifndef __EMSCRIPTEN__
    if (getenv("PORT_WATCHDOG")) SDL_CreateThread(watchdog, "watchdog", (void *)(intptr_t)atoi(getenv("PORT_WATCHDOG")));
#endif
    uint32_t runtime_exe_entry(void);
    runtime_run_entry(runtime_exe_entry());
    host_shutdown();
    return 0;
}
