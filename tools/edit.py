# Tiny exact-replacement helper for scripted source edits (avoids shell heredoc escaping problems).
# usage: python tools/edit.py <file> <spec.py>   where spec.py defines EDITS = [(old, new), ...] and/or CUT = [(start, end)]
import sys
path, spec = sys.argv[1], sys.argv[2]
ns = {}
exec(open(spec, encoding='utf-8').read(), ns)
s = open(path, 'rb').read().decode('utf-8', 'surrogateescape')
for old, new in ns.get('EDITS', []):
    if old not in s:
        raise SystemExit('%s: not found: %r' % (path, old[:80]))
    s = s.replace(old, new, 1)
for a, b in ns.get('CUT', []):
    i = s.index(a); j = s.index(b, i) + len(b)
    s = s[:i] + s[j:]
open(path, 'wb').write(s.encode('utf-8', 'surrogateescape'))
print('edited', path)
