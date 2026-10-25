"""List the PLT calls (with names) made by a range of the disassembly (rsbr32.asm + pltnames.txt).

Usage: python tools/plt_calls.py <rsbr32.asm> <pltnames.txt> <start_hex> <end_hex>
"""
import re
import sys

asm, plt, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
names = {}
for line in open(plt, encoding="utf-8", errors="replace"):
    parts = line.split()
    if len(parts) == 2:
        names[int(parts[0], 16)] = parts[1]
pat = re.compile(r"^\s+([0-9a-f]+):\s+\S*\s*(bl|blx|b)(?:[a-z]{2})?\s+0x([0-9a-f]+)")
for line in open(asm, encoding="utf-8", errors="replace"):
    m = pat.match(line)
    if not m:
        continue
    addr = int(m.group(1), 16)
    if lo <= addr < hi:
        target = int(m.group(3), 16)
        if target in names:
            print("%x %s %s" % (addr, m.group(2), names[target]))
