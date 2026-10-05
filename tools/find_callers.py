#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find call sites of a libnickel PLT import (Thumb BL/BLX and B.W tail calls).

Needs pyelftools and capstone (pip install pyelftools capstone) and
libnickel.so.1.0.0 extracted from a firmware image:
  python tools/find_callers.py path/to/libnickel.so.1.0.0 <mangled symbol>

A symbol with a .rel.plt entry but no call sites here is probably reached only
through Qt (signal/slot by pointer) and can't be PLT-hooked. Thumb tail calls
(b.w) land on the "bx pc" trampoline 4 bytes before the ARM stub.
"""
import sys, struct, bisect
sys.path.insert(0, 'tools/python-packages')
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from capstone import *
from capstone.arm import *
path, want = sys.argv[1], sys.argv[2]
f = open(path, 'rb'); blob = f.read(); elf = ELFFile(open(path, 'rb'))
def off(a):
    for s in elf.iter_segments():
        if s['p_type']=='PT_LOAD' and s['p_vaddr']<=a<s['p_vaddr']+s['p_filesz']: return a-s['p_vaddr']+s['p_offset']
plt = elf.get_section_by_name('.plt'); relp = elf.get_section_by_name('.rel.plt'); syms = elf.get_section(relp['sh_link'])
got = None
for r in relp.iter_relocations():
    if syms.get_symbol(r['r_info_sym']).name == want: got = r['r_offset']
arm = Cs(CS_ARCH_ARM, CS_MODE_ARM); arm.detail=True
stub=None
for a in range(plt['sh_addr'], plt['sh_addr']+plt['sh_size'], 4):
    o=off(a); ins=list(arm.disasm(blob[o:o+12], a))
    regs={}
    for i in ins:
        ops=i.operands
        if i.mnemonic=='add' and ops[1].type==ARM_OP_REG:
            base = i.address+8 if ops[1].reg==ARM_REG_PC else regs.get(ops[1].reg)
            imm=ops[2].imm
            if len(ops)>3: r=ops[3].imm; imm=((imm>>r)|(imm<<(32-r)))&0xffffffff
            if base is not None: regs[ops[0].reg]=(base+imm)&0xffffffff
        elif i.mnemonic=='ldr' and len(ops)>1 and ops[1].type==ARM_OP_MEM and ops[1].mem.base in regs:
            if (regs[ops[1].mem.base]+ops[1].mem.disp)&0xffffffff==got: stub=a
    if stub: break
print('stub', hex(stub))
text=elf.get_section_by_name('.text'); ta=text['sh_addr']; to=text['sh_offset']; n=text['sh_size']
symlist=sorted((s['st_value']&~1,s.name) for s in elf.get_section_by_name('.dynsym').iter_symbols() if s['st_value'])
keys=[k for k,_ in symlist]
th=Cs(CS_ARCH_ARM, CS_MODE_THUMB)
for i in range(0, n-4, 2):
    h1,h2=struct.unpack_from('<HH', blob, to+i)
    if (h1&0xf800)!=0xf000 or ((h2&0xc000)!=0xc000 and (h2&0xd000)!=0x9000): continue
    S=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
    I1=1-(J1^S); I2=1-(J2^S)
    v=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
    if S: v-=1<<25
    pc=ta+i+4
    t = ((pc&~3)+v) if (h2&0xd000)==0xc000 else pc+v
    if t in (stub, stub-4):
        a=ta+i; j=bisect.bisect_right(keys,a)-1
        print(f'{a:#x} in {symlist[j][1]}')
        for ins in th.disasm(blob[to+i-24:to+i+4], a-24): print('   ', ins.mnemonic, ins.op_str)
