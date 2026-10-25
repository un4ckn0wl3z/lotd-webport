// Per-game settings: Land of the Dead: Road to Fiddler's Green (Brainbox Games, 2005; Unreal Engine 2 build 2226,
// MSVC7, Direct3D 8). The game is one exe plus 15 recompiled DLLs (modules.json, tools/layout.py).
#pragma once

// The executable, relative to the game directory. The game's working directory is its System folder.
#define GAME_EXE        "System/LOTD.exe"
#define GAME_SYSDIR     "System"
// Window / page title.
#define GAME_TITLE      "Land of the Dead: Road to Fiddler's Green"
// Environment variable that may point at the game directory (native builds).
#define GAME_DIR_ENV    "LOTD_DIR"
// Size of the flat guest address space. Must exceed the modules + peak heap (watch the "perf:" log line).
#ifndef GUEST_SIZE_MB
#define GUEST_SIZE_MB   320
#endif
// The DLLs are packed from 0x400000 to 0x1020000 (modules.json); the heap starts above them and skips the exe's
// fixed image at 0x10900000 (runtime.c reserves every module range).
#define HEAP_BASE       0x01100000u
