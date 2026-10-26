# Land of the Dead: Road to Fiddler's Green — web port

A fan port that runs the 2005 PC game in a browser (WebAssembly + WebGL 2) and natively (SDL2), with online play
between browsers. Research and educational project: bring your own copy of the game; nothing from the game is in
this folder, and builds that contain game data must stay private.

## How it works
- **Route: static recompilation.** The game is Unreal Engine 2 (build 2226): `LOTD.exe` plus 15 DLLs (Core, Engine,
  EngineEx, Window, WinDrv, D3DDrv, IpDrv, ALAudio, Fire, GUI, MSVCR70, MSVCP70, ogg, vorbis, vorbisfile). Every one
  is translated from x86 to C (`tools/recomp.py`, ~1.3M instructions), at a fixed address (`tools/layout.py`,
  `modules.json`: the DLLs are rebased below the exe's fixed 0x10900000). UnrealScript runs in the recompiled VM.
- **Runtime** (`src/runtime.c`): one flat guest memory, a PE loader/linker for all modules (imports between
  modules bind to the other module's exports), DllMain order, and MSVC C++ exceptions: `RaiseException` walks the
  SEH chain, catch funclets run, and `_JumpToContinuation` unwinds the host stack back to the frame that caught.
- **Windows layer:** `kernel32.c` (files on a save overlay over the read-only game folder, seen as `C:\LOTD`),
  `user32.c` (real window objects for Window.dll's classes, input, virtual cursor / pointer lock), `dinput.c`,
  `d3d8.c` + `d3d8_ff.c` (Direct3D 8 fixed-function pipeline as generated GLSL ES 3.00: lighting, fog, 8 texture
  stages, texture transforms, cube maps, render targets, vertex declarations), `openal.c` (OpenAL for ALAudio on
  SDL audio), `wsock.c` (network), `bink.c`, `misc.c`.
- **Threads:** Core's file-streaming thread is done by the host each frame (`game.c`); IpDrv's lookup thread runs to
  completion.
- **Online:** the game keeps its own Unreal netcode. Natively Winsock passes through to the real one. In the
  browser every page joins a room on the relay (`server/server.js`) and gets a LAN address 10.77.0.x; UDP datagrams
  and LAN broadcasts go through a WebSocket (`web/vnet.js`). Host from "Host Multiplayer Game" (or `?args=MAP%3FListen`)
  and join from "Join Multiplayer Game" (LAN list) or `?args=10.77.0.2`.

## Quick start
Bring your own installed copy of the game (the folder with `System/LOTD.exe`). Needs Python 3.8+, Node.js 18+ (the
server and online relay), Git; for the native build also a 64-bit MinGW-w64 gcc (TDM-GCC or MSYS2) on Windows.

```bash
python port.py --game "C:/Games/Land of the Dead"
```

That one command analyses the 16 game modules, translates them to C, builds the WebAssembly version, packs your
game data next to it and serves it on http://localhost:8080/. Missing tools are fetched on first use: the
`capstone`/`pefile` Python packages, Emscripten (into `.emsdk/`, about 1 GB, unless `EMSDK` points at one) and SDL2
for MinGW (into `thirdparty/`). The first build takes a while (about 1.3M instructions become C); later runs only
redo what changed.

| Command | What it does |
|---|---|
| `python port.py --game <dir>` | everything below for the web, then serve |
| `python port.py --game <dir> web` | web build only (`--no-data`: no data package, each player picks their own folder in the page) |
| `python port.py --game <dir> native` | native Windows build: `build/lotdport.exe` |
| `python port.py --game <dir> run -- CH01` | native build, then play (here straight into chapter 1) |
| `python port.py --game <dir> gen` | only the analysis and C generation (`--force` redoes it) |
| `python port.py serve --port 8080 --password secret` | serve the last web build; use a password whenever others can reach it |

`--game` can be left out when `GAME_DIR` is set or the game is in `./game` or `../game`. Everything derived from
the game goes into ignored folders (`gen/`, `build/`, `build_web/`); a web build with `game.data` contains your
game files, so keep it private.

Only the 2005 Russian "8 Bit" release (UE2 build 2226) was tested; `port.py` says when it sees another build of
`LOTD.exe`. The analysis itself is automatic, but `recomp_config.json` holds a few addresses of that build.

## How the pipeline works
1. `tools/layout.py`: a fixed guest address for every module (`modules.json`): the exe stays at 0x10900000, the
   DLLs are packed below it.
2. `tools/pe_export.py`: IDA-free analysis of each module with capstone, at its new base (relocated with .reloc):
   recursive descent from the entry, exports, call targets and every relocated pointer into code; MSVC jump tables;
   incremental-link thunks; catch blocks that end in a rethrow; a linear sweep for code only reached through
   computed pointers. Starts found only through pointers or the sweep are folded back into the function whose code
   runs through them (C++ exception continuations must stay in the function that owns them). On this build it finds
   the same instructions IDA Pro does to within a few dozen out of 1.3M.
3. `tools/gen_all.py` runs `tools/recomp.py` per module (`gen/<module>/`), then `gen/modules.c` and
   `gen/import_hle.c`.
4. C compiler: gcc for native, emcc for the web, run in parallel by `port.py`.

The old route (IDA Pro 9 with `tools/ida_rebase_export.py`, then `python tools/gen_all.py <System dir> <export dir>`
and `mingw32-make` / `mingw32-make -f Makefile.web`) still works.

## Run
Native: `python port.py --game <dir> run`, or `build/lotdport.exe <game folder> <save folder> [-- <game command line>]`.

Web: open `http://localhost:8080/` (it must be served over HTTP(S), not opened as a file). With `game.data` the game
starts from the package; without it the page asks for the player's own game folder and reads it locally (nothing
is uploaded). Page options: `?room=name` (online room), `?args=CH01` (start a map), `?env=PORT_SEH=1,...` (debug
switches).

## Debugging switches (environment variables)
`PORT_SHOT=100,500` screenshots (BMP) at those frames · `PORT_INPUT="600:move:320,248;610:click:320,248;700:key:0x57"`
scripted input · `PORT_WATCHDOG=60` dump guest state and shim call counts after N seconds · `PORT_SEH=1` trace
exceptions · `PORT_D3DTRACE=<frame>` log every draw of a frame · `PORT_D3DSOLID=1` flat-red geometry ·
`PORT_INPUTLOG=1` · `PORT_LOGFILES=1` · build with `EXTRA=-DMEMCHECK BUILD=build_mc` for range-checked memory.

## Status (2026-10-26)
Verified: menus, profiles, chapter 1 (loading, rendering, mouse look, walking, prompts) natively and in Chrome
(~75 fps, frame-capped); a native host + native client deathmatch; a browser host + browser client deathmatch on
DMDowntown through the relay, with the host tab in the background.

Not done / known limits:
- Cut-scenes: Bink video is not decoded; each movie ends after one black frame.
- Audio was not listened to (the mixer runs; music/sound correctness unverified).
- Hosting from the command line (`?args=DMDowntown?Listen`) leaves the map rotation empty, so after the time limit
  the server travels to chapter 1 and clients drop. Host from the "Host Multiplayer Game" menu (pick the maps there):
  the rotation is saved and clients follow map changes.
- TCP (master server, HTTP map download) is unavailable online; there is no internet server list, only rooms.
- No SSE/3DNow! paths (the port reports a CPU without them); EAX, force feedback and the editor are absent.
- Only the 2005 Russian "8 Bit" release was tested (module sizes are checked at load).
