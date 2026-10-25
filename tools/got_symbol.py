"""Resolve PC-relative GOT loads in libRSBR_arm32.so to symbol names.

Usage: python tools/got_symbol.py <lib.so> <add_pc_insn_addr> <pc_literal_addr> <got_offset_literal_addr> [...]
For the common sequence
    ldr r0, [pc, #a]      ; r0 = literal at <pc_literal_addr>
    ldr r1, [pc, #b]      ; r1 = literal at <got_offset_literal_addr>
    add r0, pc, r0        ; at <add_pc_insn_addr>  -> base = r0 + insn + 8
    ldr rX, [r1, r0]      ; GOT slot = base + r1
prints the symbol each GOT slot is relocated to.
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


def main():
    path = sys.argv[1]
    add_pc, lit_base = int(sys.argv[2], 16), int(sys.argv[3], 16)
    got_lits = [int(a, 16) for a in sys.argv[4:]]
    with open(path, "rb") as f:
        elf = ELFFile(f)

        def word(vaddr):
            for seg in elf.iter_segments():
                if seg["p_type"] == "PT_LOAD" and seg["p_vaddr"] <= vaddr < seg["p_vaddr"] + seg["p_filesz"]:
                    f.seek(seg["p_offset"] + vaddr - seg["p_vaddr"])
                    return struct.unpack("<I", f.read(4))[0]
            raise ValueError(hex(vaddr))

        slots = {}
        dynsym = elf.get_section_by_name(".dynsym")
        for sec in elf.iter_sections():
            if isinstance(sec, RelocationSection):
                for r in sec.iter_relocations():
                    if r["r_info_sym"]:
                        slots[r["r_offset"]] = dynsym.get_symbol(r["r_info_sym"]).name
        base = (word(lit_base) + add_pc + 8) & 0xFFFFFFFF
        for lit in got_lits:
            slot = (base + word(lit)) & 0xFFFFFFFF
            print("%#x -> GOT %#x -> %s" % (lit, slot, slots.get(slot, "(no symbol; value %#x)" % word(slot))))


if __name__ == "__main__":
    main()
