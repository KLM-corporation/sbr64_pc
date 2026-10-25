"""ELF32 ARM dynamic binary inspection oracle."""

import os
import struct
from typing import Dict, List, Optional, Tuple

from tests.e2e.common.game_data import LIB_RSBR_PATH

PT_LOAD = 1
PT_DYNAMIC = 2
PT_ARM_EXIDX = 0x70000001

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_STRSZ = 10
DT_SYMENT = 11

EM_ARM = 0x28
ELFCLASS32 = 1
ELFDATA2LSB = 1
ET_DYN = 3


class Elf32Reader:
    """Minimal opaque-box ELF32 parser for validating ARM32 guest binaries."""

    def __init__(self, file_path: str = LIB_RSBR_PATH):
        self.file_path = file_path
        self.data: bytes = b""
        if os.path.exists(file_path):
            with open(file_path, "rb") as f:
                self.data = f.read()

        self.e_type = 0
        self.e_machine = 0
        self.e_version = 0
        self.e_entry = 0
        self.e_phoff = 0
        self.e_shoff = 0
        self.e_flags = 0
        self.e_ehsize = 0
        self.e_phentsize = 0
        self.e_phnum = 0
        self.e_shentsize = 0
        self.e_shnum = 0
        self.e_shstrndx = 0
        self.program_headers: List[Dict] = []
        self.section_headers: List[Dict] = []
        self.needed_libs: List[str] = []
        self.dyn_symbols: List[str] = []

        if self.data:
            self._parse()

    def is_valid(self) -> bool:
        return (
            len(self.data) >= 52
            and self.data[:4] == b"\x7fELF"
            and self.data[4] == ELFCLASS32
            and self.data[5] == ELFDATA2LSB
            and self.e_machine == EM_ARM
        )

    def _parse(self):
        if len(self.data) < 52:
            return
        (
            magic,
            ei_class,
            ei_data,
            ei_ver,
            ei_osabi,
            ei_abiver,
            pad,
            self.e_type,
            self.e_machine,
            self.e_version,
            self.e_entry,
            self.e_phoff,
            self.e_shoff,
            self.e_flags,
            self.e_ehsize,
            self.e_phentsize,
            self.e_phnum,
            self.e_shentsize,
            self.e_shnum,
            self.e_shstrndx,
        ) = struct.unpack("<4sBBBBB7sHHIIIIIHHHHHH", self.data[:52])

        # Parse program headers
        for i in range(self.e_phnum):
            offset = self.e_phoff + i * self.e_phentsize
            if offset + 32 <= len(self.data):
                p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack(
                    "<IIIIIIII", self.data[offset : offset + 32]
                )
                self.program_headers.append(
                    {
                        "type": p_type,
                        "offset": p_offset,
                        "vaddr": p_vaddr,
                        "paddr": p_paddr,
                        "filesz": p_filesz,
                        "memsz": p_memsz,
                        "flags": p_flags,
                        "align": p_align,
                    }
                )

        # Parse dynamic segment for DT_NEEDED and DT_STRTAB
        dyn_ph = next((ph for ph in self.program_headers if ph["type"] == PT_DYNAMIC), None)
        if dyn_ph:
            dyn_data = self.data[dyn_ph["offset"] : dyn_ph["offset"] + dyn_ph["filesz"]]
            tags = []
            for j in range(0, len(dyn_data), 8):
                if j + 8 <= len(dyn_data):
                    d_tag, d_val = struct.unpack("<II", dyn_data[j : j + 8])
                    if d_tag == DT_NULL:
                        break
                    tags.append((d_tag, d_val))

            # Locate string table offset
            strtab_vaddr = next((v for t, v in tags if t == DT_STRTAB), None)
            if strtab_vaddr is not None:
                # Find which PT_LOAD segment contains strtab_vaddr
                strtab_file_offset = None
                for ph in self.program_headers:
                    if ph["type"] == PT_LOAD and ph["vaddr"] <= strtab_vaddr < ph["vaddr"] + ph["filesz"]:
                        strtab_file_offset = ph["offset"] + (strtab_vaddr - ph["vaddr"])
                        break

                if strtab_file_offset is not None:
                    # Extract DT_NEEDED entries
                    for t, v in tags:
                        if t == DT_NEEDED:
                            s_off = strtab_file_offset + v
                            end_off = self.data.find(b"\x00", s_off)
                            if end_off != -1:
                                lib_name = self.data[s_off:end_off].decode("utf-8", errors="replace")
                                self.needed_libs.append(lib_name)
