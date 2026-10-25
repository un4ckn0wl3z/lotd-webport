# Remove carriage returns that ended up inside C string literals (before an escaped \n).
import sys
for p in sys.argv[1:]:
    s = open(p, 'rb').read()
    n = s.count(b'\r\\n')
    s = s.replace(b'\r\\n', b'\\n')
    open(p, 'wb').write(s)
    print(p, n)
