import struct, capstone
data=open('engine/tsanpr.dll','rb').read()
pe=struct.unpack_from('<I',data,0x3c)[0]; nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=pe+24
imgbase=struct.unpack_from('<Q',data,opt+24)[0]; sizeopt=struct.unpack_from('<H',data,pe+20)[0]; sect=opt+sizeopt
secs=[]
for i in range(nsec):
    o=sect+i*40; vsz,va,rsz,rptr=struct.unpack_from('<IIII',data,o+8); secs.append((va,vsz,rptr,rsz))
def rva2off(rva):
    for va,vsz,rptr,rsz in secs:
        if va<=rva<va+max(vsz,rsz): return rptr+(rva-va)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def dis(rva,length):
    off=rva2off(rva); 
    for ins in md.disasm(data[off:off+length], imgbase+rva):
        print("  %x: %-7s %s"%(ins.address-imgbase, ins.mnemonic, ins.op_str))
print("=== prologue arg mapping 0x21f1f80..0x21f2012 ===")
dis(0x21f1f80,0x92)
print("=== ...continue 0x21f1fc7..0x21f2012 ===")
dis(0x21f1fc7,0x4b)
print("=== common exit 0x21f2cd1 ===")
dis(0x21f2cd1,0x60)
print("=== M<3 dispatch 0x21f2831 ===")
dis(0x21f2831,0x30)
print("=== M==1? 0x21f2aff ===")
dis(0x21f2aff,0x30)
print("=== M>=4 path store+outer tail 0x21f2420..0x21f2494 ===")
dis(0x21f2420,0x74)
print("=== M>=4 store fn 0x21f3ce0? find via call. Let's see 0x21f2079 area store target ===")
dis(0x21f245e,0x40)
print("=== M>=4 store fn 0x21f3c70 ===")
dis(0x21f3c70,0xf0)
print("=== M>=4 inner loop full 0x21f2079..0x21f2467 : A reads + advances ===")
dis(0x21f2079,0x3f0)
print("=== M>=4 outer body head 0x21f202a..0x21f2079 (what resets/advances per panel) ===")
dis(0x21f202a,0x4f)
print("=== ALL branches in M>=4 path 0x21f2012..0x21f2494 ===")
import capstone as cap
off=rva2off(0x21f2012); code=data[off:off+(0x21f2494-0x21f2012)]
for ins in md.disasm(code, imgbase+0x21f2012):
    m=ins.mnemonic
    if m[0]=='j' or m.startswith('call') or m.startswith('loop') or m=='ret':
        print("  %x: %-7s %s"%(ins.address-imgbase,m,ins.op_str))
print("=== 0x21f210e (instr after the shufps at exit) ===")
dis(0x21f210e,0x10)
print("=== 0x14a1a1f (transfer target) ===")
dis(0x14a1a1f,0x30)
