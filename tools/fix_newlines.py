# Repair C string literals broken across lines (a shell heredoc turned "\n" into a real newline): a line whose
# string literal is unterminated is joined with the next one through an escaped "\n".
# Usage: python fix_newlines.py file.c ...
import sys

def unterminated(line):
    q = False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '\\' and q: i += 2; continue
        if c == '"': q = not q
        if c == '/' and not q and line[i:i + 2] == '//': break
        i += 1
    return q

for path in sys.argv[1:]:
    lines = open(path, newline='').read().split('\n')
    out, i, fixed = [], 0, 0
    while i < len(lines):
        l = lines[i]
        while unterminated(l) and i + 1 < len(lines):
            i += 1
            l = l + '\\n' + lines[i]
            fixed += 1
        out.append(l)
        i += 1
    open(path, 'w', newline='').write('\n'.join(out))
    print(path, 'joined', fixed)
