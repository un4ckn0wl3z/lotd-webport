import pefile,capstone,sys
pe=pefile.PE(sys.argv[1]);img=pe.get_memory_mapped_image();base=0x400000
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
for a in sys.argv[2:]:
    s,e=[int(x,16) for x in a.split('-')]
    print('----',a)
    for i in md.disasm(img[s-base:e-base],s): print('%x  %s %s'%(i.address,i.mnemonic,i.op_str))
