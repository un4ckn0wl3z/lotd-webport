# Disassemble Direct3D 9 shader bytecode (SM1-3) dumped with PORT_DUMPSHADERS, to see what the translator must cover.
import sys, struct, collections
OPS = {0:'nop',1:'mov',2:'add',3:'sub',4:'mad',5:'mul',6:'rcp',7:'rsq',8:'dp3',9:'dp4',10:'min',11:'max',12:'slt',13:'sge',
       14:'exp',15:'log',16:'lit',17:'dst',18:'lrp',19:'frc',20:'m4x4',21:'m4x3',22:'m3x4',23:'m3x3',24:'m3x2',25:'call',
       26:'callnz',27:'loop',28:'ret',29:'endloop',30:'label',31:'dcl',32:'pow',33:'crs',34:'sgn',35:'abs',36:'nrm',
       37:'sincos',38:'rep',39:'endrep',40:'if',41:'ifc',42:'else',43:'endif',44:'break',45:'breakc',46:'mova',47:'defb',
       48:'defi',64:'texcoord',65:'texkill',66:'tex',67:'texbem',68:'texbeml',69:'texreg2ar',70:'texreg2gb',71:'texm3x2pad',
       72:'texm3x2tex',73:'texm3x3pad',74:'texm3x3tex',76:'texm3x3spec',77:'texm3x3vspec',78:'expp',79:'logp',80:'cnd',
       81:'def',82:'texreg2rgb',83:'texdp3tex',84:'texm3x2depth',85:'texdp3',86:'texm3x3',87:'texdepth',88:'cmp',89:'bem',
       90:'dp2add',91:'dsx',92:'dsy',93:'texldd',94:'setp',95:'texldl',96:'breakp'}
NPARAM1 = {0:0,1:2,2:3,3:3,4:4,5:3,6:2,7:2,8:3,9:3,10:3,11:3,12:3,13:3,14:2,15:2,16:2,17:3,18:4,19:2,20:3,21:3,22:3,23:3,
           24:3,31:2,64:1,65:1,66:1,67:2,68:2,69:2,70:2,71:2,72:2,73:2,74:2,76:3,77:2,78:2,79:2,80:4,81:5,82:2,83:2,84:2,
           85:2,86:2,87:1,88:4,89:3,90:4,32:3,33:3,35:2,36:2,37:2}
RT = ['r','v','c','a/t','rast','attr','oT','ci','oC','oDepth','s','c2','c3','c4','b','aL','h','misc','l','p']
def rtype(t): return ((t >> 28) & 7) | ((t >> 8) & 0x18)
def dst(t):
    m = (t >> 16) & 15
    mods = (t >> 20) & 15; sh = (t >> 24) & 15
    return '%s%d%s%s%s' % (RT[rtype(t)], t & 0x7ff, '' if m == 15 else '.' + ''.join('xyzw'[i] for i in range(4) if m >> i & 1),
                           '_sat' if mods & 1 else '', ('_sh%d' % sh) if sh else '')
def src(t):
    sw = (t >> 16) & 255; mod = (t >> 24) & 15
    s = ''.join('xyzw'[(sw >> (2 * i)) & 3] for i in range(4))
    return '%s%s%d%s%s' % (('mod%d:' % mod) if mod else '', RT[rtype(t)], t & 0x7ff, '' if s == 'xyzw' else '.' + s, '[rel]' if t & 0x2000 else '')
def dis(path, hist):
    d = open(path, 'rb').read(); tok = struct.unpack('<%dI' % (len(d) // 4), d)
    ver = tok[0]; major = (ver >> 8) & 255; ps = (ver >> 16) == 0xffff
    print('== %s %s_%d_%d' % (path, 'ps' if ps else 'vs', major, ver & 255))
    i = 1
    while i < len(tok):
        t = tok[i]; op = t & 0xffff
        if op == 0xffff: break
        if op == 0xfffe: i += 1 + ((t >> 16) & 0x7fff); continue
        n = (t >> 24) & 15 if major >= 2 else NPARAM1.get(op, 0)
        if op == 81 or op == 48: n = 5
        if op == 47: n = 2
        p = tok[i + 1:i + 1 + n]
        name = OPS.get(op, 'op%d' % op)
        hist[name] += 1
        if op == 31: txt = 'dcl(%08x) %s' % (p[0], dst(p[1]))
        elif op == 81: txt = 'def %s %s' % (dst(p[0]), ' '.join('%g' % struct.unpack('<f', struct.pack('<I', x))[0] for x in p[1:]))
        elif n: txt = name + ' ' + ', '.join([dst(p[0])] + [src(x) for x in p[1:]])
        else: txt = name
        print('  %s%s' % ('+' if t & 0x40000000 else ' ', txt))
        i += 1 + n
hist = collections.Counter()
for p in sys.argv[1:]: dis(p, hist)
print(dict(hist))
