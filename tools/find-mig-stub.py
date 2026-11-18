#!/usr/bin/env python3
"""Describe the client side of a Mach (MIG) message, from a disassembly.

Mac OS X's private servers speak undocumented protocols, but every client
stub is generated code with the same shape: it stores the message number,
calls mach_msg, then checks the reply's number and size. This finds the stub
that sends a given message number and prints what reply it will accept, which
is what cocaine's stand-in for the server has to produce.

usage: find-mig-stub.py DISASSEMBLY MESSAGE_NUMBER
       (make the disassembly with: otool -tV binary > binary.dis)

Reply sizes: 36 is the plain error reply (header, NDR record, return code).
A size checked together with "1 descriptor" is a reply carrying a port or
out-of-line memory.
"""
import re
import sys

path, number = sys.argv[1], int(sys.argv[2], 0)
lines = open(path, errors="replace").read().split("\n")
needle = "$0x%x, " % number
reply = "$0x%x, " % (number + 100)
for i, line in enumerate(lines):
    if needle not in line or "movl" not in line:
        continue
    name = next((l[:-1] for l in reversed(lines[:i]) if l.endswith(":") and not l[:1].isdigit()), "?")
    sizes, seen_reply = [], False
    for l in lines[i:i + 140]:
        if l.endswith(":") and not l[:1].isdigit() and seen_reply:
            break
        if reply in l:
            seen_reply = True
        m = re.search(r"cmpl\s+\$(0x[0-9a-f]+), %e[a-d]x", l)
        if seen_reply and m and int(m.group(1), 16) < 0x2000 and reply not in l:
            sizes.append(int(m.group(1), 16))
        if "cmpl\t$0x1, -" in l and seen_reply:
            sizes.append("1 descriptor")
    print("%s: sends %d, accepts reply %d with sizes/checks %s" % (name, number, number + 100, sizes))
