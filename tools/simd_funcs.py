# List the functions that contain instructions recomp.py does not handle (SSE/SSE2/3DNow!/MMX...),
# grouped per function with IDA's name, so we can decide how each is reached (CPU-feature dispatch or not).
import json, sys, collections, bisect, pefile, capstone, re
pe = pefile.PE(sys.argv[1])
img = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
d = json.load(open(sys.argv[2]))
src = open(sys.argv[3]).read() if len(sys.argv) > 3 else open('tools/recomp.py').read()
supported = set(re.findall(r'def i_(\w+)', src))
x87 = lambda m: m.startswith('f')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
funcs = sorted((f['start'], f['end'], f.get('name', '')) for f in d['funcs'])
starts = [f[0] for f in funcs]
per = collections.defaultdict(collections.Counter)
for ea in d['code_heads']:
    off = ea - base
    ins = next(md.disasm(img[off:off+16], ea), None)
    if ins is None: continue
    m = ins.mnemonic.split()[-1]
    if m.startswith('j') or m.startswith('set') or m.startswith('cmov') or x87(m) or m in supported: continue
    if m in ('rep', 'repe', 'repne', 'movsd', 'movsb', 'movsw', 'stosd', 'stosb', 'stosw', 'cmpsb', 'cmpsd', 'cmpsw', 'scasb', 'lodsb', 'loop', 'cpuid', 'pushal', 'popal', 'pushfd', 'popfd', 'ret'):
        if not (m == 'movsd' and 'xmm' in ins.op_str): continue
    i = bisect.bisect_right(starts, ea) - 1
    key = funcs[i] if i >= 0 and ea < funcs[i][1] else (ea, ea, '?')
    per[key][m] += 1
for (s, e, n), c in sorted(per.items()):
    print("%06x-%06x %-40s %s" % (s, e, n[:40], ' '.join('%s:%d' % kv for kv in c.most_common(8))))
