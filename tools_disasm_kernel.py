import struct, capstone
data=open('engine/tsanpr.dll','rb').read()
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=pe+24
imgbase=struct.unpack_from('<Q',data,opt+24)[0]
sizeopt=struct.unpack_from('<H',data,pe+20)[0]; sect=opt+sizeopt
secs=[]
for i in range(nsec):
    o=sect+i*40; va,vsz=struct.unpack_from('<II',data,o+12-4+0)[0:2] if False else (0,0)
    name=data[o:o+8].rstrip(b'\x00').decode('latin1')
    vsz,va,rsz,rptr=struct.unpack_from('<IIII',data,o+8)
    secs.append((va,vsz,rptr,rsz))
def rva2off(rva):
    for va,vsz,rptr,rsz in secs:
        if va<=rva<va+max(vsz,rsz): return rptr+(rva-va)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def dis(rva,length,stop=None):
    off=rva2off(rva); code=data[off:off+length]
    for ins in md.disasm(code, imgbase+rva):
        print("  rva %x: %-7s %s"%(ins.address-imgbase, ins.mnemonic, ins.op_str))
        if stop and ins.address-imgbase>=stop: break
print("=== M==3 path @0x21f2494 ===")
dis(0x21f2494,0x140)

print("=== store/exit @0x21f27dd ===")
dis(0x21f27dd,0x90)
print("=== prologue: find function start (disasm 0x21f1e80..) ===")
dis(0x21f1e80,0x150)

print("=== store fn @0x21f3bb0 (M=3) ===")
dis(0x21f3bb0,0xd0)

print("=== M=3 full k-loop 0x21f24f5..0x21f2804 ===")
dis(0x21f24f5,0x310)
