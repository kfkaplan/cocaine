#!/usr/bin/env python3
"""A small PowerPC disassembler, for reading Apple's PowerPC code.

Current Xcode tools can't disassemble PowerPC, and the PowerPC and Intel
halves of a library don't always agree (the input system's shared memory is
laid out differently for the two), so sometimes the PowerPC code is the only
place to look. This knows the instructions that matter for that: loads and
stores, arithmetic with constants, compares and branches. Anything else is
printed as a number.

    lipo -thin ppc Some.framework/Some -output /tmp/some.ppc
    nm /tmp/some.ppc | grep _someFunction          # its address, say 000fdb00
    python3 tools/ppc-dis.py /tmp/some.ppc fdb00 40

The address is the one nm prints, which for a thinned library is also the
place in the file; the last number is how many instructions to print.
"""
import sys, struct
D = {32:'lwz',33:'lwzu',34:'lbz',35:'lbzu',36:'stw',37:'stwu',38:'stb',39:'stbu',40:'lhz',41:'lhzu',42:'lha',43:'lhau',44:'sth',45:'sthu',46:'lmw',47:'stmw',48:'lfs',50:'lfd',52:'stfs',54:'stfd'}
def s16(x): return x - 0x10000 if x & 0x8000 else x
def dis(w, pc):
    op = w >> 26; rt = (w >> 21) & 31; ra = (w >> 16) & 31; rb = (w >> 11) & 31; imm = w & 0xFFFF
    if op in D: return '%s r%d, %d(r%d)' % (D[op], rt, s16(imm), ra)
    if op == 14: return ('li r%d, %d' % (rt, s16(imm))) if ra == 0 else 'addi r%d, r%d, %d' % (rt, ra, s16(imm))
    if op == 15: return ('lis r%d, 0x%x' % (rt, imm)) if ra == 0 else 'addis r%d, r%d, 0x%x' % (rt, ra, imm)
    if op == 7: return 'mulli r%d, r%d, %d' % (rt, ra, s16(imm))
    if op == 10: return 'cmplwi cr%d, r%d, %d' % (rt >> 2, ra, imm)
    if op == 11: return 'cmpwi cr%d, r%d, %d' % (rt >> 2, ra, s16(imm))
    if op == 24: return 'ori r%d, r%d, 0x%x' % (ra, rt, imm)
    if op == 25: return 'oris r%d, r%d, 0x%x' % (ra, rt, imm)
    if op == 28: return 'andi. r%d, r%d, 0x%x' % (ra, rt, imm)
    if op == 29: return 'andis. r%d, r%d, 0x%x' % (ra, rt, imm)
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        return '%s 0x%x' % ('bl' if w & 1 else 'b', (pc + li) & 0xFFFFFFFF)
    if op == 16:
        bd = s16(w & 0xFFFC)
        return 'bc %d,%d, 0x%x' % (rt, ra, (pc + bd) & 0xFFFFFFFF)
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16: return 'blr' if rt == 20 else 'bclr %d,%d' % (rt, ra)
        if xo == 528: return 'bctr' + ('l' if w & 1 else '')
    if op == 21:
        return 'rlwinm r%d, r%d, %d, %d, %d' % (ra, rt, rb, (w >> 6) & 31, (w >> 1) & 31)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        X = {23:'lwzx',151:'stwx',87:'lbzx',215:'stbx',279:'lhzx',407:'sthx',266:'add',40:'subf',0:'cmpw',32:'cmplw',444:'or',28:'and',339:'mfspr',467:'mtspr',20:'lwarx',150:'stwcx.',24:'slw',536:'srw',235:'mullw'}
        if xo == 444 and rt == rb: return 'mr r%d, r%d' % (ra, rt)
        if xo in X: return '%s r%d, r%d, r%d' % (X[xo], rt, ra, rb)
    return '.long 0x%08x' % w
if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16); n = int(sys.argv[3])
    for i in range(n):
        pc = start + 4 * i
        w = struct.unpack('>I', data[pc:pc + 4])[0]
        print('%08x  %s' % (pc, dis(w, pc)))
