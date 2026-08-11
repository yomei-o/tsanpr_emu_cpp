import struct, capstone
data = open('engine/tsanpr.dll','rb').read()
# PE parse
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4]==b'PE\x00\x00'
nsec = struct.unpack_from('<H', data, pe+6)[0]
opt = pe+24
magic = struct.unpack_from('<H', data, opt)[0]
imgbase = struct.unpack_from('<Q', data, opt+24)[0] if magic==0x20b else struct.unpack_from('<I',data,opt+28)[0]
sizeopt = struct.unpack_from('<H', data, pe+20)[0]
sect = opt+sizeopt
secs=[]
for i in range(nsec):
    o=sect+i*40
    name=data[o:o+8].rstrip(b'\x00').decode('latin1')
    vsz,va,rsz,rptr=struct.unpack_from('<IIII',data,o+8)
    secs.append((name,va,vsz,rptr,rsz))
def rva2off(rva):
    for name,va,vsz,rptr,rsz in secs:
        if va<=rva<va+max(vsz,rsz): return rptr+(rva-va)
    return None
print("imgbase=0x%x sections:"%imgbase)
for s in secs: print("  %-8s va=0x%x vsz=0x%x rptr=0x%x"%(s[0],s[1],s[2],s[3]))
rva=0x21f2002
off=rva2off(rva)
print("RVA 0x%x -> file off 0x%x"%(rva,off))
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail=False
# disasm from a bit before to see prologue too
start_rva=0x21f1fd0
so=rva2off(start_rva)
code=data[so:so+0x180]
for ins in md.disasm(code, imgbase+start_rva):
    mark=" <== ENTRY 0x21f2002" if ins.address==imgbase+rva else ""
    print("0x%x (rva %x): %-8s %s%s"%(ins.address, ins.address-imgbase, ins.mnemonic, ins.op_str, mark))
