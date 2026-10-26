#!/usr/bin/env python3
"""Land of the Dead: Road to Fiddler's Green web port: one command from your own game folder to a browser build.

    python port.py --game "C:/Games/Land of the Dead"   # analyse, recompile, build for the web, serve on :8080
    python port.py --game ... web                        # build only
    python port.py --game ... native                     # native Windows build (MinGW gcc + SDL2)
    python port.py --game ... run                        # native build, then play it
    python port.py serve                                 # serve the last web build (and the online relay)

Nothing from the game is stored in this repository. Every step reads the files from --game (or the GAME_DIR
environment variable) and writes derived files only into ignored folders: gen/, build/, build_web/.
Web builds that include the game data (build_web/out/game.data) are for you: keep a server that others can reach
behind --password.
"""
import argparse, concurrent.futures, hashlib, json, os, shutil, subprocess, sys, zipfile, urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = 'System/LOTD.exe'
# Builds of LOTD.exe this port knows (sha256). The analysis is automatic, but the addresses in recomp_config.json
# (frame-loop hook, C++ unwind jump, one HLE) belong to this build.
KNOWN_EXES = {
    '231ab70a7663466f3b03bd266bc21c72e0ddae8ed50783a3df24f9f2e306bccb': 'tested: 2005 Russian "8 Bit" release (UE2 build 2226)',
}
# Every module is recompiled; they all have to be there.
MODULES = ['LOTD.exe', 'MSVCR70.dll', 'MSVCP70.dll', 'Core.dll', 'Engine.dll', 'EngineEx.dll', 'Window.dll', 'WinDrv.dll',
           'D3DDrv.dll', 'IpDrv.dll', 'ALAudio.dll', 'Fire.dll', 'GUI.dll', 'ogg.dll', 'vorbis.dll', 'vorbisfile.dll']
# What the engine reads at run time (no installer, movies, saves): about 480 MB.
DATA_DIRS = ['System', 'Levels', 'Textures', 'Audio', 'Animations', 'Meshes', 'KarmaData', 'Help']
DATA_EXCLUDE = ['*.log', '*.bak', '*.tmp', '*.exe.bak', 'Setup*', 'unins*', 'UCC*', '*.bik']
SDL2_MINGW = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip'


def log(*a):
    print('[port]', *a, flush=True)


def run(cmd, **kw):
    log(' '.join(str(c) for c in cmd) if len(cmd) < 12 else '%s ... (%d args)' % (cmd[0], len(cmd)))
    subprocess.check_call(cmd, **kw)


def find_ci(d, rel):
    """Case-insensitive lookup of a relative path ('System/LOTD.exe')."""
    for name in rel.replace('\\', '/').split('/'):
        if not d or not os.path.isdir(d): return None
        d = next((os.path.join(d, n) for n in os.listdir(d) if n.lower() == name.lower()), None)
    return d


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def game_dir(args):
    cands = [args.game, os.environ.get('GAME_DIR'), os.path.join(ROOT, 'game'), os.path.join(os.path.dirname(ROOT), 'game')]
    for c in cands:
        if c and find_ci(c, EXE): return os.path.abspath(c)
    sys.exit('Cannot find %s. Pass your installed game folder: python port.py --game "<folder with System/LOTD.exe>"' % EXE)


def ensure_pydeps():
    try:
        import capstone, pefile  # noqa: F401
    except ImportError:
        run([sys.executable, '-m', 'pip', 'install', '--user', 'capstone>=5', 'pefile'])


def stamp_ok(path, key):
    try: return open(path).read() == key
    except OSError: return False


# ------------------------------------------------------------------ analysis + generation
def generate(gdir, force=False):
    """layout.py (module bases) -> pe_export.py per module (IDA-free analysis) -> gen_all.py (recomp.py per module)."""
    ensure_pydeps()
    sysdir = os.path.dirname(find_ci(gdir, EXE))
    missing = [m for m in MODULES if not find_ci(sysdir, m)]
    if missing: sys.exit('%s is missing from %s' % (', '.join(missing), sysdir))
    digest = sha256(find_ci(sysdir, 'LOTD.exe'))
    log('game: %s' % gdir)
    log('LOTD.exe sha256 %s: %s' % (digest[:16], KNOWN_EXES.get(digest, 'UNKNOWN build - trying anyway')))
    tools = os.path.join(ROOT, 'tools')
    key = ':'.join([sha256(find_ci(sysdir, m)) for m in MODULES] +
                   [sha256(os.path.join(tools, t)) for t in ('layout.py', 'pe_export.py', 'recomp.py', 'gen_all.py')] +
                   [sha256(os.path.join(ROOT, 'recomp_config.json'))])
    stamp = os.path.join(ROOT, 'gen', '.stamp')
    if not force and stamp_ok(stamp, key):
        log('gen/ is up to date')
        return
    run([sys.executable, os.path.join(tools, 'layout.py'), sysdir, os.path.join(ROOT, 'modules.json')])
    mods = json.load(open(os.path.join(ROOT, 'modules.json')))['modules']
    adir = os.path.join(ROOT, 'build', 'analysis')
    os.makedirs(adir, exist_ok=True)
    log('analysing %d modules' % len(mods))
    def analyse(m):
        out = os.path.join(adir, os.path.splitext(m['file'])[0] + '.export.json')
        r = subprocess.run([sys.executable, os.path.join(tools, 'pe_export.py'), find_ci(sysdir, m['file']), out,
                            '--base', hex(m['base'])], capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr).strip()
    with concurrent.futures.ThreadPoolExecutor(min(8, os.cpu_count() or 4)) as ex:
        for rc, out in ex.map(analyse, mods):
            log('  ' + out.splitlines()[-1] if out else '')
            if rc: sys.exit('analysis failed:\n' + out)
    shutil.rmtree(os.path.join(ROOT, 'gen'), ignore_errors=True)
    os.makedirs(os.path.join(ROOT, 'gen'))
    log('generating C')
    r = subprocess.run([sys.executable, os.path.join(tools, 'gen_all.py'), sysdir, adir], capture_output=True, text=True)
    open(os.path.join(adir, 'gen.log'), 'w').write(r.stdout + r.stderr)
    if r.returncode or '(rc 0)' not in r.stdout or any('(rc ' in l and '(rc 0)' not in l for l in r.stdout.splitlines()):
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
        sys.exit('code generation failed (see build/analysis/gen.log)')
    open(stamp, 'w').write(key)


def sources():
    gen = []
    for d in sorted(os.listdir(os.path.join(ROOT, 'gen'))):
        p = os.path.join(ROOT, 'gen', d)
        if os.path.isdir(p):
            gen += [os.path.join('gen', d, f) for f in sorted(os.listdir(p)) if f.endswith('.c')]
    gen += [os.path.join('gen', 'modules.c'), os.path.join('gen', 'import_hle.c')]
    src = sorted(os.path.join('src', f) for f in os.listdir(os.path.join(ROOT, 'src')) if f.endswith('.c'))
    return gen, src


def compile_all(jobs, out_dir, cc_for):
    """jobs: list of (source, flags). Rebuilds objects older than their source, the headers or the flags."""
    hdr_time = max(os.path.getmtime(os.path.join(ROOT, 'src', f)) for f in os.listdir(os.path.join(ROOT, 'src')) if f.endswith('.h'))
    objs, todo = [], []
    os.makedirs(out_dir, exist_ok=True)
    for s, flags in jobs:
        o = os.path.join(out_dir, s.replace(os.sep, '_').replace('/', '_')[:-2] + '.o')
        objs.append(o)
        sp = os.path.join(ROOT, s)
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(sp), hdr_time):
            todo.append(cc_for(s, o, flags))
    if todo:
        log('compiling %d files (the engine is large: the first build takes a while)' % len(todo))
        done = 0
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            for r in ex.map(lambda c: subprocess.run(c, cwd=ROOT, capture_output=True, text=True), todo):
                if r.returncode:
                    sys.stderr.write(r.stdout + r.stderr)
                    sys.exit('compile failed: %s' % ' '.join(r.args[-3:]))
                done += 1
                if done % 50 == 0: log('  %d / %d' % (done, len(todo)))
    return objs


# ------------------------------------------------------------------ web
def find_emsdk(auto_install=True):
    for d in (os.environ.get('EMSDK'), os.path.join(ROOT, '.emsdk'), os.path.expanduser('~/emsdk')):
        if d and os.path.exists(os.path.join(d, 'upstream', 'emscripten', 'emcc.py')): return d
    if not auto_install: return None
    d = os.path.join(ROOT, '.emsdk')
    log('Emscripten not found: installing it into %s (about 1 GB, once)' % d)
    if not os.path.isdir(d):
        run(['git', 'clone', '--depth', '1', 'https://github.com/emscripten-core/emsdk.git', d])
    tool = os.path.join(d, 'emsdk.bat' if os.name == 'nt' else 'emsdk')
    run([tool, 'install', 'latest'], cwd=d)
    run([tool, 'activate', 'latest'], cwd=d)
    return d


def emcc_env(emsdk):
    env = dict(os.environ, EMSDK=emsdk, EM_CONFIG=os.path.join(emsdk, '.emscripten'))
    nd = os.path.join(emsdk, 'node')
    if os.path.isdir(nd):
        for n in os.listdir(nd): env['PATH'] = os.path.join(nd, n, 'bin') + os.pathsep + env['PATH']
    return env


def package_data(gdir, emsdk, env, out):
    """Pack what the game reads into out/game.data + game.js (the page loads it instead of asking for the folder)."""
    srcs = [(find_ci(gdir, d), d) for d in DATA_DIRS]
    for p, d in srcs:
        if not p: sys.exit('%s/ is missing from the game folder' % d)
    key = []
    for p, d in srcs:
        for dp, _, fs in os.walk(p):
            for f in fs:
                st = os.stat(os.path.join(dp, f))
                key.append('%s:%d:%d' % (os.path.relpath(os.path.join(dp, f), gdir), st.st_size, int(st.st_mtime)))
    key = hashlib.sha256('\n'.join(sorted(key)).encode()).hexdigest()
    stamp = os.path.join(out, 'game.data.stamp')
    if stamp_ok(stamp, key) and os.path.exists(os.path.join(out, 'game.data')):
        log('game.data is up to date')
        return
    log('packing the game data into game.data (about 500 MB)')
    cmd = [sys.executable, os.path.join(emsdk, 'upstream', 'emscripten', 'tools', 'file_packager.py'),
           os.path.join(out, 'game.data')]
    for p, d in srcs: cmd += ['--preload', '%s@/game/%s' % (p.replace('\\', '/'), d)]
    cmd += ['--exclude'] + DATA_EXCLUDE + ['--js-output=' + os.path.join(out, 'game.js'), '--no-node']
    run(cmd, env=env)
    open(stamp, 'w').write(key)


def build_web(gdir, with_data=True):
    generate(gdir)
    emsdk = find_emsdk()
    env = emcc_env(emsdk)
    emcc = [sys.executable, os.path.join(emsdk, 'upstream', 'emscripten', 'emcc.py')]
    out = os.path.join(ROOT, 'build_web', 'out')
    os.makedirs(out, exist_ok=True)
    gen, src = sources()
    gflags = ['-O2', '-fno-strict-aliasing', '-w', '-Isrc']
    sflags = ['-O2', '-fno-strict-aliasing', '-sUSE_SDL=2', '-Isrc', '-Wno-unused-function']
    def cc(s, o, flags): return emcc + flags + ['-c', s, '-o', o]
    os.environ.update(env)          # the emcc subprocesses need the emsdk environment
    objs = compile_all([(s, gflags) for s in gen] + [(s, sflags) for s in src], os.path.join(ROOT, 'build_web', 'obj'), cc)
    link = emcc + ['-o', os.path.join(out, 'index.html')] + objs + [
        '-O2', '-sUSE_SDL=2', '-sMAX_WEBGL_VERSION=2', '-sMIN_WEBGL_VERSION=2',
        '-sINITIAL_MEMORY=1024MB', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=4GB',
        '-sSTACK_SIZE=16MB', '-sINVOKE_RUN=0', '-sEXIT_RUNTIME=0', '-sENVIRONMENT=web', '-sFORCE_FILESYSTEM=1',
        '-sEXPORTED_RUNTIME_METHODS=callMain,ccall,cwrap,FS,addRunDependency,removeRunDependency', '-lidbfs.js',
        '--pre-js', 'web/prejs.js', '--shell-file', 'web/shell.html']
    run(link, cwd=ROOT, env=env)
    shutil.copy2(os.path.join(ROOT, 'web', 'vnet.js'), out)
    if with_data: package_data(gdir, emsdk, env, out)
    else:
        for f in ('game.data', 'game.js', 'game.data.stamp'):
            if os.path.exists(os.path.join(out, f)): os.remove(os.path.join(out, f))
        log('no data package: the page asks each player for their own game folder')
    log('web build ready: %s' % out)


# ------------------------------------------------------------------ native (Windows)
def find_sdl2():
    tp = os.path.join(ROOT, 'thirdparty')
    for d in sorted(os.listdir(tp)) if os.path.isdir(tp) else []:
        r = os.path.join(tp, d, 'x86_64-w64-mingw32')
        if d.startswith('SDL2-') and os.path.isdir(r):
            return (['-I' + os.path.join(r, 'include', 'SDL2'), '-DSDL_MAIN_HANDLED'],
                    ['-L' + os.path.join(r, 'lib'), '-lSDL2'], os.path.join(r, 'bin', 'SDL2.dll'))
    os.makedirs(tp, exist_ok=True)
    z = os.path.join(tp, 'sdl2-mingw.zip')
    log('downloading SDL2 (MinGW development package)')
    urllib.request.urlretrieve(SDL2_MINGW, z)
    zipfile.ZipFile(z).extractall(tp)
    os.remove(z)
    return find_sdl2()


def build_native(gdir):
    if os.name != 'nt': sys.exit('the native build is Windows-only (Winsock pass-through); use the web build elsewhere')
    generate(gdir)
    cc = os.environ.get('CC') or shutil.which('gcc') or sys.exit('no gcc: install MinGW-w64 (e.g. TDM-GCC or MSYS2) and put it on PATH')
    machine = subprocess.check_output([cc, '-dumpmachine'], text=True).strip()
    if not machine.startswith('x86_64'): sys.exit('%s targets %s: a 64-bit MinGW-w64 gcc is needed' % (cc, machine))
    cflags, lflags, dll = find_sdl2()
    gen, src = sources()
    gflags = ['-O1', '-fno-strict-aliasing', '-w', '-Isrc']
    sflags = ['-O2', '-g', '-fno-strict-aliasing', '-Wall', '-Wno-unused-function', '-Wno-misleading-indentation', '-Isrc'] + cflags
    def ccf(s, o, flags): return [cc] + flags + ['-c', s, '-o', o]
    out = os.path.join(ROOT, 'build')
    objs = compile_all([(s, gflags) for s in gen] + [(s, sflags) for s in src], os.path.join(out, 'obj'), ccf)
    exe = os.path.join(out, 'lotdport.exe')
    run([cc, '-o', exe] + objs + lflags + ['-lws2_32', '-lm'], cwd=ROOT)
    shutil.copy2(dll, out)
    log('native build ready: %s' % exe)
    return exe


def play_native(gdir, extra):
    exe = build_native(gdir)
    save = os.path.join(ROOT, 'build', 'save')
    os.makedirs(save, exist_ok=True)
    run([exe, gdir, save] + (['--'] + extra if extra else []), cwd=os.path.dirname(exe))


# ------------------------------------------------------------------ serve
def serve(port, password=None):
    out = os.path.join(ROOT, 'build_web', 'out')
    if not os.path.exists(os.path.join(out, 'index.html')): sys.exit('no web build yet: run python port.py --game <folder>')
    node = shutil.which('node')
    env = dict(os.environ)
    if password: env['LOTD_PASSWORD'] = password
    if node:
        log('serving http://localhost:%d (solo and online play; rooms via ?room=name)' % port)
        if os.path.exists(os.path.join(out, 'game.data')) and not password:
            log('the build includes your game data: add --password before letting anyone else reach this server')
        run([node, os.path.join(ROOT, 'server', 'server.js'), '--port', str(port), '--game', out], env=env)
    else:
        log('Node.js not found: serving http://localhost:%d for solo play only (install Node 18+ for online play)' % port)
        run([sys.executable, '-m', 'http.server', str(port), '--bind', '127.0.0.1', '--directory', out])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', default='all', choices=['all', 'gen', 'web', 'native', 'run', 'serve'])
    ap.add_argument('--game', help='your installed game folder (contains System/LOTD.exe)')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--password', help='serve: require this password (HTTP basic auth); use it whenever others can reach the server')
    ap.add_argument('--no-data', action='store_true', help='web: no game.data package; every player picks their own game folder in the page')
    ap.add_argument('--force', action='store_true', help='redo the analysis and code generation')
    ap.add_argument('args', nargs=argparse.REMAINDER, help='run: game command line after --, e.g. -- CH01')
    a = ap.parse_args()
    if a.command == 'serve': return serve(a.port, a.password)
    g = game_dir(a)
    if a.command == 'gen': return generate(g, a.force)
    if a.force: generate(g, True)
    if a.command == 'native': return build_native(g)
    if a.command == 'run': return play_native(g, [x for x in a.args if x != '--'])
    build_web(g, not a.no_data)
    if a.command == 'all': serve(a.port, a.password)


if __name__ == '__main__':
    main()
