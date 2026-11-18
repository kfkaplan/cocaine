#!/usr/bin/env python3
"""Link big-endian PowerPC ELF objects into a static Mach-O PowerPC program.

Current compilers can still generate PowerPC code, but only into ELF object
files; nothing modern writes PowerPC Mach-O. This small linker bridges the gap
for cocaine's own test programs. It lays out the sections of a few ELF
objects, applies their relocations, and wraps the result in a Mach-O
executable with an LC_UNIXTHREAD entry point, the shape of a program built
with `gcc -static` on Mac OS X Tiger.

usage: elf2macho.py -o OUTPUT a.o b.o ...
"""

import struct
import sys

PAGE = 0x1000
BASE = 0x1000         # __TEXT starts right above the zero page
HEADER_ROOM = 0x200   # the Mach-O header and load commands open __TEXT

SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
SHF_WRITE, SHF_ALLOC = 1, 2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_GLOBAL, STB_WEAK = 1, 2


def die(msg):
    sys.exit("elf2macho: " + msg)


def align(value, to):
    return (value + to - 1) & ~(to - 1)


def cstr(blob, off):
    return blob[off:blob.index(b"\0", off)].decode()


class Section:
    def __init__(self, fields, blob):
        (self.name_off, self.type, self.flags, _, self.offset, self.size,
         self.link, self.info, self.align, _) = fields
        self.align = max(self.align, 1)
        self.data = bytearray(self.size if self.type == SHT_NOBITS
                              else blob[self.offset:self.offset + self.size])
        self.addr = None  # set by layout for sections that get loaded
        self.name = ""


class Obj:
    def __init__(self, path):
        self.path = path
        blob = open(path, "rb").read()
        if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 2:
            die(path + ": not a 32-bit big-endian ELF file")
        (_, machine, _, _, _, shoff, _, _, _, _, shentsize, shnum,
         shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", blob, 16)
        if machine != 20:
            die(path + ": not PowerPC")
        self.sections = [Section(struct.unpack_from(">10I", blob, shoff + i * shentsize), blob)
                         for i in range(shnum)]
        names = self.sections[shstrndx].data
        for s in self.sections:
            s.name = cstr(names, s.name_off)

        self.symbols = []   # (name, value, binding, shndx)
        self.relocs = []    # (target section, offset, symbol index, type, addend)
        for s in self.sections:
            if s.type == SHT_SYMTAB:
                strings = self.sections[s.link].data
                for off in range(0, s.size, 16):
                    name, value, _, info, _, shndx = struct.unpack_from(">IIIBBH", s.data, off)
                    self.symbols.append((cstr(strings, name), value, info >> 4, shndx))
        for s in self.sections:
            if s.type == SHT_RELA:
                target = self.sections[s.info]
                if not target.flags & SHF_ALLOC:
                    continue  # debug info and the like: not loaded, so not relocated
                for off in range(0, s.size, 12):
                    r_off, r_info, addend = struct.unpack_from(">IIi", s.data, off)
                    self.relocs.append((target, r_off, r_info >> 8, r_info & 0xFF, addend))


def layout(objs):
    """Assigns addresses: read-only sections, then data, then zero-fill."""
    text, data, bss = [], [], []
    for o in objs:
        for s in o.sections:
            if not s.flags & SHF_ALLOC:
                continue
            if s.type == SHT_NOBITS:
                bss.append(s)
            elif s.flags & SHF_WRITE:
                data.append(s)
            else:
                text.append(s)

    def place(sections, addr):
        for s in sections:
            addr = align(addr, s.align)
            s.addr = addr
            addr += s.size
        return addr

    text_end = place(text, BASE + HEADER_ROOM)
    data_start = align(text_end, PAGE)
    data_end = place(data, data_start)
    bss_end = place(bss, data_end)
    return text, data, text_end, data_start, data_end, bss_end


def link(objs):
    text, data, text_end, data_start, data_end, bss_end = layout(objs)

    # Global symbols; a strong definition beats a weak one.
    table = {}
    for o in objs:
        for name, value, binding, shndx in o.symbols:
            if shndx == SHN_UNDEF or binding not in (STB_GLOBAL, STB_WEAK):
                continue
            if shndx == SHN_COMMON:
                die("%s: common symbol %s (compile with -fno-common)" % (o.path, name))
            addr = value if shndx == SHN_ABS else o.sections[shndx].addr + value
            if name in table and table[name][1] == STB_GLOBAL:
                if binding == STB_GLOBAL:
                    die("duplicate symbol " + name)
                continue
            table[name] = (addr, binding)

    def resolve(o, index):
        name, value, binding, shndx = o.symbols[index]
        if shndx == SHN_UNDEF:
            if name in table:
                return table[name][0]
            if binding == STB_WEAK:
                return 0
            die("%s: undefined symbol %s" % (o.path, name))
        if shndx == SHN_ABS:
            return value
        return o.sections[shndx].addr + value

    def patch_insn(sec, off, mask, value):
        word = struct.unpack_from(">I", sec.data, off)[0]
        struct.pack_into(">I", sec.data, off, (word & ~mask & 0xFFFFFFFF) | (value & mask))

    for o in objs:
        for sec, off, sym, rtype, addend in o.relocs:
            s = resolve(o, sym)
            place = sec.addr + off
            if rtype == 0:                       # R_PPC_NONE
                pass
            elif rtype == 1:                     # R_PPC_ADDR32
                struct.pack_into(">I", sec.data, off, (s + addend) & 0xFFFFFFFF)
            elif rtype == 2:                     # R_PPC_ADDR24
                patch_insn(sec, off, 0x03FFFFFC, s + addend)
            elif rtype in (3, 4):                # R_PPC_ADDR16, R_PPC_ADDR16_LO
                struct.pack_into(">H", sec.data, off, (s + addend) & 0xFFFF)
            elif rtype == 5:                     # R_PPC_ADDR16_HI
                struct.pack_into(">H", sec.data, off, ((s + addend) >> 16) & 0xFFFF)
            elif rtype == 6:                     # R_PPC_ADDR16_HA
                struct.pack_into(">H", sec.data, off, ((s + addend + 0x8000) >> 16) & 0xFFFF)
            elif rtype == 7:                     # R_PPC_ADDR14
                patch_insn(sec, off, 0x0000FFFC, s + addend)
            elif rtype in (10, 23):              # R_PPC_REL24, R_PPC_LOCAL24PC
                patch_insn(sec, off, 0x03FFFFFC, s + addend - place)
            elif rtype == 18:                    # R_PPC_PLTREL24: a plain call here
                patch_insn(sec, off, 0x03FFFFFC, s - place)
            elif rtype == 11:                    # R_PPC_REL14
                patch_insn(sec, off, 0x0000FFFC, s + addend - place)
            elif rtype == 26:                    # R_PPC_REL32
                struct.pack_into(">I", sec.data, off, (s + addend - place) & 0xFFFFFFFF)
            elif rtype in (249, 250):            # R_PPC_REL16, R_PPC_REL16_LO
                struct.pack_into(">H", sec.data, off, (s + addend - place) & 0xFFFF)
            elif rtype == 251:                   # R_PPC_REL16_HI
                struct.pack_into(">H", sec.data, off, ((s + addend - place) >> 16) & 0xFFFF)
            elif rtype == 252:                   # R_PPC_REL16_HA
                struct.pack_into(">H", sec.data, off,
                                 ((s + addend - place + 0x8000) >> 16) & 0xFFFF)
            else:
                die("%s: unsupported relocation type %d in %s" % (o.path, rtype, sec.name))

    if "_start" not in table:
        die("no _start symbol")
    entry = table["_start"][0]

    def image(sections, start, end):
        out = bytearray(align(end - start, PAGE))
        for s in sections:
            out[s.addr - start:s.addr - start + s.size] = s.data
        return out

    text_img = image(text, BASE, text_end)
    data_img = image(data, data_start, data_end)
    data_vmsize = align(bss_end - data_start, PAGE)

    def segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot):
        return struct.pack(">II16sIIIIIIII", 1, 56, name.encode(), vmaddr, vmsize, fileoff,
                           filesize, maxprot, initprot, 0, 0)

    cmds = [segment("__PAGEZERO", 0, BASE, 0, 0, 0, 0),
            segment("__TEXT", BASE, len(text_img), 0, len(text_img), 7, 5)]
    if data_vmsize:
        cmds.append(segment("__DATA", data_start, data_vmsize, len(text_img), len(data_img), 7, 3))
    # LC_UNIXTHREAD with a PPC_THREAD_STATE: 40 words, the first being the pc.
    cmds.append(struct.pack(">IIII", 5, 16 + 160, 1, 40) + struct.pack(">40I", entry, *([0] * 39)))

    load = b"".join(cmds)
    # magic, cputype PowerPC, subtype ALL, MH_EXECUTE, ncmds, sizeofcmds, MH_NOUNDEFS
    header = struct.pack(">7I", 0xFEEDFACE, 18, 0, 2, len(cmds), len(load), 1)
    if len(header) + len(load) > HEADER_ROOM:
        die("load commands do not fit in the header room")
    text_img[:len(header) + len(load)] = header + load
    return bytes(text_img) + bytes(data_img)


def main(argv):
    if len(argv) < 4 or argv[1] != "-o":
        sys.exit(__doc__.strip().splitlines()[-1])
    out = link([Obj(p) for p in argv[3:]])
    with open(argv[2], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main(sys.argv)
