#!/usr/bin/env python3
"""Patch the SCE velf entry point back to the crt entry (_start).

The vita-elf-create converter picks a wrong e_entry (an unrelated
function) after our link-order changes; neither -Wl,-e nor its -m flag
changes it. Vita3K boots from e_entry, so the app returned immediately.
This rewrites e_entry from the input ELF's real entry (relocated to the
module base 0x81000000).
"""
import sys, struct

velf_path, elf_path = sys.argv[1], sys.argv[2]
elf = open(elf_path, 'rb').read()
entry = struct.unpack_from('<I', elf, 0x18)[0] & ~1          # strip Thumb
rel = entry - 0x81000000
v = bytearray(open(velf_path, 'rb').read())
struct.pack_into('<I', v, 0x18, rel)
open(velf_path, 'wb').write(v)
print(f"velf entry patched to {rel:#x} (ELF entry {entry:#x})")
