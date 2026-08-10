import struct, sys
import capstone
data=open("engine/tsanpr.dll","rb").read()
e_lfanew=struct.unpack_from("<I",data,0x3C)[0]
assert data[e_lfanew:e_lfanew+4]==b"PE\0\0"
coff=e_lfanew+4
num_sec=struct.unpack_from("<H",data,coff+2)[0]
opt_size=struct.unpack_from("<H",data,coff+16)[0]
opt=coff+20
image_base=struct.unpack_from("<Q",data,opt+24)[0]
sec=opt+opt_size
secs=[]
for i in range(num_sec):
    off=sec+i*40
    name=data[off:off+8].rstrip(b"\0").decode('latin1')
    va=struct.unpack_from("<I",data,off+12)[0]
    vsz=struct.unpack_from("<I",data,off+8)[0]
    praw=struct.unpack_from("<I",data,off+20)[0]
    rsz=struct.unpack_from("<I",data,off+16)[0]
    secs.append((name,va,vsz,praw,rsz))
def rva2off(rva):
    for name,va,vsz,praw,rsz in secs:
        if va<=rva<va+max(vsz,rsz):
            return praw+(rva-va)
    return None
def disasm(rva,length):
    off=rva2off(rva)
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    md.detail=False
    code=data[off:off+length]
    for insn in md.disasm(code, rva):
        print("  RVA %08x  emu %011x  %-9s %s"%(insn.address, 0x140100000+insn.address, insn.mnemonic, insn.op_str))
target=int(sys.argv[1],16)
length=int(sys.argv[2],16) if len(sys.argv)>2 else 0x120
print("image_base=%x sections:"%image_base, [(n,hex(va)) for n,va,_,_,_ in secs][:6])
print("=== disasm RVA %x ==="%target)
disasm(target,length)
