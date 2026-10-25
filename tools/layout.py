# Assigns every recompiled module a fixed guest base address and writes modules.json.
# The exe has no relocations and stays at its preferred base; the DLLs are packed from DLL_BASE upwards
# (64 KB aligned) and rebased with their .reloc tables, both by recomp.py and by the runtime loader.
# Usage: python layout.py <game System dir> <out modules.json>
import json, sys, os, pefile

EXE = 'LOTD.exe'
DLLS = ['MSVCR70.dll', 'MSVCP70.dll', 'Core.dll', 'Engine.dll', 'EngineEx.dll', 'Window.dll', 'WinDrv.dll',
        'D3DDrv.dll', 'IpDrv.dll', 'ALAudio.dll', 'Fire.dll', 'GUI.dll', 'ogg.dll', 'vorbis.dll', 'vorbisfile.dll']
DLL_BASE = 0x00400000

sysdir, out = sys.argv[1], sys.argv[2]
mods = []
pe = pefile.PE(os.path.join(sysdir, EXE), fast_load=True)
mods.append({'file': EXE, 'tag': 'lotd', 'orig_base': pe.OPTIONAL_HEADER.ImageBase,
             'base': pe.OPTIONAL_HEADER.ImageBase, 'size': pe.OPTIONAL_HEADER.SizeOfImage})
a = DLL_BASE
for d in DLLS:
    pe = pefile.PE(os.path.join(sysdir, d), fast_load=True)
    size = pe.OPTIONAL_HEADER.SizeOfImage
    mods.append({'file': d, 'tag': os.path.splitext(d)[0].lower(), 'orig_base': pe.OPTIONAL_HEADER.ImageBase,
                 'base': a, 'size': size})
    a = (a + size + 0xffff) & ~0xffff
exe = mods[0]
assert a <= exe['base'], 'DLLs overlap the exe'
json.dump({'modules': mods, 'dll_end': a}, open(out, 'w'), indent=1)
for m in mods:
    print('%-16s %08x-%08x (orig %08x)' % (m['file'], m['base'], m['base'] + m['size'], m['orig_base']))
print('dll end %08x' % a)
