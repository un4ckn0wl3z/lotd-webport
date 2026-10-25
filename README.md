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

## Build
Needs: IDA Pro 9 (analysis), Python 3 with `pefile` and `capstone`, TDM-GCC/MinGW + SDL2 (`thirdparty/sdl2`), emsdk.

```bash
# 1. analysis (once per module, in a folder with copies of the game's DLLs and the two IDA scripts):
#    idat -A -S"ida_rebase_export.py <Module>.export.json <base from modules.json>" <Module>.dll   (tools/)
# 2. C generation
python tools/gen_all.py ../game/System ../work/ida
# 3. native
mingw32-make -j12
# 4. web (+ optional private data package of ~500 MB)
mingw32-make -f Makefile.web -j12
mingw32-make -f Makefile.web data
```

## Run
Native, from `run/`: `../build/lotdport.exe <game folder> <save folder> [-- <game command line>]`.

Web: `node server/server.js --port 8080` serves `build_web/out` and the relay (`LOTD_PASSWORD=...` for basic auth),
then open `http://localhost:8080/`. Without `game.data` the page asks for the player's own game folder and reads it
locally (nothing is uploaded). Page options: `?room=name` (online room), `?args=CH01` (start a map),
`?env=PORT_SEH=1,...` (debug switches). It must be served over HTTP(S), not opened as a file.

## Debugging switches (environment variables)
`PORT_SHOT=100,500` screenshots (BMP) at those frames · `PORT_INPUT="600:move:320,248;610:click:320,248;700:key:0x57"`
scripted input · `PORT_WATCHDOG=60` dump guest state and shim call counts after N seconds · `PORT_SEH=1` trace
exceptions · `PORT_D3DTRACE=<frame>` log every draw of a frame · `PORT_D3DSOLID=1` flat-red geometry ·
`PORT_INPUTLOG=1` · `PORT_LOGFILES=1` · build with `EXTRA=-DMEMCHECK BUILD=build_mc` for range-checked memory.

## Status (2026-10-25)
Verified: menus, profiles, chapter 1 (loading, rendering, mouse look, walking, prompts) natively and in Chrome
(~75 fps, frame-capped); a native host + native client deathmatch; a browser host + browser client deathmatch on
DMDowntown through the relay, with the host tab in the background.

Not done / known limits:
- Cut-scenes: Bink video is not decoded; each movie ends after one black frame.
- Audio was not listened to (the mixer runs; music/sound correctness unverified).
- After a deathmatch time limit the server changes map and the browser client timed out during that change.
- TCP (master server, HTTP map download) is unavailable online; there is no internet server list, only rooms.
- No SSE/3DNow! paths (the port reports a CPU without them); EAX, force feedback and the editor are absent.
- Only the 2005 Russian "8 Bit" release was tested (module sizes are checked at load).
