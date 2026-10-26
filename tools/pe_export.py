# IDA-free analysis of one MSVC-built PE module, writing the JSON that recomp.py reads (the format of
# ida_export.py). Based on the House of the Dead port's cap_export.py, extended for MSVC DLLs and exes:
# - the image is rebased to --base first, so every address is the one the port runs it at;
# - function seeds: the entry point, the exports, every relocated pointer into code (vtables, callbacks, EH
#   tables) and, for an image without .reloc (LOTD.exe), aligned code pointers in its data and immediates in code;
# - MSVC jump tables (jmp [reg*4 + table], tables after the function, sometimes with a byte index table);
# - exports named like IDA names them (C names get the leading underscore: _memcpy, __ftol, __CxxFrameHandler).
# Usage: python pe_export.py Module.dll out.json [--base 0x400000]
import json, sys, struct, argparse
import pefile, capstone
from capstone import x86_const as X

ap = argparse.ArgumentParser()
ap.add_argument('pe'); ap.add_argument('out'); ap.add_argument('--base', default=None)
args = ap.parse_args()
pe = pefile.PE(args.pe)
if args.base is not None and int(args.base, 0) != pe.OPTIONAL_HEADER.ImageBase:
    pe.relocate_image(int(args.base, 0))
    pe.OPTIONAL_HEADER.ImageBase = int(args.base, 0)
IMG = pe.get_memory_mapped_image()
BASE = pe.OPTIONAL_HEADER.ImageBase
CODE_SECTS = [s for s in pe.sections if s.Characteristics & 0x20000000]
CODE = CODE_SECTS[0]
TLO = BASE + CODE.VirtualAddress
THI = TLO + max(CODE.Misc_VirtualSize, CODE.SizeOfRawData)

def rd32(a): return struct.unpack_from('<I', IMG, a - BASE)[0]
def in_text(a): return TLO <= a < THI
def in_image(a): return BASE <= a < BASE + len(IMG) - 3

RELOCS = {}
HAS_RELOCS = hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC')
if HAS_RELOCS:
    for blk in pe.DIRECTORY_ENTRY_BASERELOC:
        for e in blk.entries:
            if e.type == 3: RELOCS[BASE + e.rva] = rd32(BASE + e.rva)

IMPORTS = {}
if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dll = d.dll.decode()
        if dll.lower().endswith('.dll'): dll = dll[:-4]
        for imp in d.imports:
            IMPORTS[imp.address] = [dll, imp.name.decode() if imp.name else '#%d' % imp.ordinal]
NAMES = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if not e.name: continue
        n = e.name.decode()
        NAMES[BASE + e.address] = n if n.startswith('?') else '_' + n
for a, (dll, n) in IMPORTS.items():
    NAMES[a] = '__imp_' + n

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
_dc = {}
def decode(a):
    if a in _dc: return _dc[a]
    o = a - BASE
    ins = next(md.disasm(bytes(IMG[o:o + 16]), a), None) if in_text(a) else None
    _dc[a] = ins
    return ins

DATA_IN_TEXT = set()
HEADS = {}
SWITCHES = {}
FUNCS = set()
problems = []

def bad(ins):
    if ins is None: return True
    b = bytes(ins.bytes)
    if ins.mnemonic in ('int3', 'hlt', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'arpl', 'bound', 'into',
                        'les', 'lds', 'retf', 'iretd', 'sti', 'cli', 'ljmp', 'lcall'): return True
    if b[:2] == b'\x00\x00': return True
    return any(a in DATA_IN_TEXT for a in range(ins.address, ins.address + ins.size))

def table_entry(a):
    """A jump table slot: the relocated value, or (no .reloc) the raw dword if it points into code."""
    if HAS_RELOCS: return RELOCS.get(a)
    if not in_image(a): return None
    v = rd32(a)
    return v if in_text(v) else None

def read_table(t):
    out, a = [], t
    while len(out) < 4096:
        v = table_entry(a)
        if v is None or not in_text(v) or (a != t and a in HEADS): break
        out.append(v); a += 4
    if not out: return None
    if in_text(t): DATA_IN_TEXT.update(range(t, a))
    return out

def code_ptrs_of(ins, a, nxt):
    """Pointers into code carried by an instruction (push offset f, mov [x], offset f): function candidates."""
    if HAS_RELOCS:
        for s in range(a + 1, nxt - 3):
            if s in RELOCS and in_text(RELOCS[s]): yield RELOCS[s]
        return
    for op in ins.operands:
        if op.type == X.X86_OP_IMM and in_text(op.imm & 0xffffffff): yield op.imm & 0xffffffff

def explore(start, newfuncs):
    st = [start]
    while st:
        a = st.pop()
        while True:
            if a in HEADS or not in_text(a): break
            ins = decode(a)
            if bad(ins):
                if a != start: problems.append('flow into bad code at %x (from func %x)' % (a, start))
                break
            HEADS[a] = ins.size
            m = ins.mnemonic
            nxt = a + ins.size
            # `mov eax, offset L; ret` in a catch funclet returns the continuation L: code inside the function that
            # holds the try block, not a function (recomp.py finds these from the FuncInfo tables)
            cont = m == 'mov' and ins.bytes[0] == 0xb8 and (decode(nxt) or ins).mnemonic == 'ret'
            if m not in ('jmp', 'call') and not m.startswith('j') and not cont:
                for v in code_ptrs_of(ins, a, nxt): newfuncs.add(('imm', v))
            if m in ('ret', 'retf'): break
            if m == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM: newfuncs.add(op.imm & 0xffffffff)
                a = nxt; continue
            if m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    ti = decode(t)
                    if t in STRONG or a == start or (ti is not None and ti.bytes[0] == 0xe9):
                        newfuncs.add(t); break      # tail call, or a jump through an incremental-link thunk
                    a = t; continue
                if op.type == X.X86_OP_MEM and op.mem.index and op.mem.scale == 4 and \
                        in_image(op.mem.disp & 0xffffffff):
                    tg = read_table(op.mem.disp & 0xffffffff)
                    if tg:
                        SWITCHES[a] = {'targets': sorted(set(tg)), 'jumps': op.mem.disp & 0xffffffff,
                                       'ncases': len(tg), 'elbase': 0}
                        st.extend(tg)
                    else:
                        problems.append('unresolved table jump at %x' % a)
                break
            if m.startswith('j') or m.startswith('loop') or m == 'jecxz':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM: st.append(op.imm & 0xffffffff)
            a = nxt

def plausible(t, strict=False):
    """A pointer into code that starts a function: the first instructions decode and reach a ret or jmp."""
    if t in DATA_IN_TEXT: return False
    a, prev, n = t, None, 0
    for _ in range(64):
        ins = decode(a)
        if bad(ins) or (ins.mnemonic == 'add' and ins.bytes[0] in (0, 2)):
            return prev == 'call' and n >= 3        # ends in a no-return call (catch block rethrowing)
        if prev == 'call' and n >= 3 and (ins.mnemonic == 'nop' or ins.op_str in ('ecx, [ecx]', 'esp, [esp]', 'edi, edi')):
            return True                             # alignment padding after a no-return call
        prev = ins.mnemonic; n += 1
        if ins.mnemonic in ('ret', 'jmp'): return True
        a += ins.size
    return not strict

# pointers into code held in the data sections
data_ptrs = set()
if HAS_RELOCS:
    data_ptrs = {v for s, v in RELOCS.items() if not in_text(s) and in_text(v)}
else:
    for s in pe.sections:
        if s in CODE_SECTS: continue
        lo = BASE + s.VirtualAddress
        hi = min(lo + max(s.Misc_VirtualSize, s.SizeOfRawData), BASE + len(IMG) - 3)
        for a in range(lo, hi, 4):
            v = rd32(a)
            if in_text(v): data_ptrs.add(v)
data_ptrs = sorted(data_ptrs)

# Function starts are "strong" (entry, exports, call targets, jmp thunk targets) or "weak" (pointers into code
# from data or immediates, and the gap sweep). Code flow of one function can run through a weak start: catch
# continuations, labels whose address is taken, code after a call that does not return. IDA keeps those inside
# the enclosing function, and so must we: the C++ EH unwind resumes a continuation only in the function that
# owns it. So weak starts that end up inside a strong function's body are dropped at the end (recomp.py still
# makes them entries when data points at them).
seeds = {BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint} if pe.OPTIONAL_HEADER.AddressOfEntryPoint else set()
seeds |= {a for a in NAMES if in_text(a)}
STRONG = set()
pending = [(a, True) for a in seeds]
while True:
    while pending:
        f, strong = pending.pop()
        if not in_text(f): continue
        if strong: STRONG.add(f)
        if f in FUNCS: continue
        FUNCS.add(f)
        if f in HEADS: continue             # reached as code inside another function: still an entry
        nf = set()
        explore(f, nf)
        for x in nf:
            if isinstance(x, tuple):
                if x[1] not in FUNCS and plausible(x[1]): pending.append((x[1], False))
            elif x not in STRONG: pending.append((x, True))
    if data_ptrs:
        for v in data_ptrs:
            if v not in FUNCS and plausible(v): pending.append((v, False))
        data_ptrs = []
        continue
    # relocated words in the code segment that are neither instructions nor tables: callback tables
    extra = set()
    if HAS_RELOCS:
        covered = set()
        for h, sz in HEADS.items(): covered.update(range(h, h + sz))
        for s, v in RELOCS.items():
            if in_text(s) and in_text(v) and s not in covered and s not in DATA_IN_TEXT and v not in FUNCS and plausible(v):
                extra.add(v)
    if extra:
        pending = [(v, False) for v in extra]
        continue
    if True:
        # linear sweep like IDA's: unexplored code after padding (int3/nop) that decodes as a function, e.g.
        # incremental-link jmp thunks and functions only reached through computed pointers. Each find is explored
        # before the sweep goes on, so code it reaches is not picked up again as another start.
        a = TLO
        while a < THI:
            if a in HEADS: a += HEADS[a]; continue
            if a in DATA_IN_TEXT or IMG[a - BASE] in (0xcc, 0x90, 0x00): a += 1; continue
            if a in FUNCS: a += 1; continue
            if not plausible(a):
                # data before the code (a table): retry at the following 16-byte aligned addresses
                nxt_head = a
                while nxt_head < THI and nxt_head not in HEADS: nxt_head += 1
                a2 = (a + 16) & ~15
                while a2 < nxt_head and not (a2 not in DATA_IN_TEXT and plausible(a2, True)): a2 += 16
                if a2 >= nxt_head: a = nxt_head; continue
                a = a2
                if a in FUNCS: a += 1; continue
            extra.add(a)
            pending = [(a, False)]
            while pending:                  # explore it (and what it reaches) right away
                f, strong = pending.pop()
                if not in_text(f): continue
                if strong: STRONG.add(f)
                if f in FUNCS: continue
                FUNCS.add(f)
                if f in HEADS: continue
                nf = set()
                explore(f, nf)
                for x in nf:
                    if isinstance(x, tuple):
                        if x[1] not in FUNCS and plausible(x[1]): pending.append((x[1], False))
                    elif x not in STRONG: pending.append((x, True))
            if a not in HEADS: a += 1
    if not extra: break

def build_body(f, stops):
    body, st = set(), [f]
    while st:
        a = st.pop()
        while a in HEADS and a not in body:
            if a != f and a in stops: break
            body.add(a)
            ins = decode(a); m = ins.mnemonic; nxt = a + ins.size
            if m in ('ret', 'retf'): break
            if m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in FUNCS and t in stops or a == f: break
                    a = t; continue
                if a in SWITCHES: st.extend(SWITCHES[a]['targets'])
                break
            if m.startswith('j') or m.startswith('loop') or m == 'jecxz':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM: st.append(op.imm & 0xffffffff)
            a = nxt
    return body

# drop weak starts that the body of a strong function (or of an earlier weak one) runs through
inside = set()
for f in sorted(STRONG & FUNCS):
    inside |= build_body(f, STRONG) - {f}
weak = sorted(FUNCS - STRONG)
for f in weak:
    if f in inside: FUNCS.discard(f)
kept = STRONG | set(f for f in weak if f in FUNCS)
inside2 = set()
for f in sorted(FUNCS - STRONG):
    if f in inside2: FUNCS.discard(f); continue
    inside2 |= build_body(f, kept) - {f}
dropped = len(weak) - len(FUNCS - STRONG)

# function bodies -> chunks
funcs_out = []
for f in sorted(FUNCS):
    body = build_body(f, FUNCS)
    chunks = []
    for h in sorted(body):
        e = h + HEADS[h]
        if chunks and chunks[-1][1] == h: chunks[-1][1] = e
        else: chunks.append([h, e])
    if not chunks: chunks = [[f, f + HEADS.get(f, 1)]]
    funcs_out.append({'start': f, 'end': max(c[1] for c in chunks), 'name': NAMES.get(f, 'sub_%X' % f),
                      'flags': 0, 'chunks': chunks, 'frsize': 0, 'argsize': 0})

segs = []
for s in pe.sections:
    nm = s.Name.rstrip(b'\0').decode(errors='replace')
    lo = BASE + s.VirtualAddress
    hi = lo + max(s.Misc_VirtualSize, s.SizeOfRawData)
    if s is CODE: nm = '.text'
    segs.append({'start': lo, 'end': hi, 'name': nm, 'perm': 5 if s in CODE_SECTS else 6})
segs.sort(key=lambda x: x['start'])
heads = sorted(HEADS)
json.dump({'funcs': funcs_out, 'switches': {str(k): v for k, v in SWITCHES.items()}, 'names': {str(k): v for k, v in NAMES.items()},
           'imports': {str(k): v for k, v in IMPORTS.items()}, 'segments': segs, 'code_heads': heads}, open(args.out, 'w'))
with open(args.out + '.problems.txt', 'w') as fp:
    fp.write('\n'.join(problems) + '\n')
cov = sum(HEADS.values())
print('%s: funcs %d (weak dropped %d) heads %d switches %d code %.1f%% problems %d' % (
    args.pe.replace('\\', '/').split('/')[-1], len(FUNCS), dropped, len(HEADS), len(SWITCHES), 100.0 * cov / (THI - TLO), len(problems)))
