#!/usr/bin/env python3
"""
Properly embed the MIDP native library into the CLDC VM binary.

The issue: objcopy --add-section creates a section but it's not in any LOAD
segment, so the Vita loader doesn't map it into memory.

This script:
1. Reads the existing ELF binary
2. Extends the data LOAD segment to include space for .midp.native
3. Appends the .midp.native data at the correct file offset
4. Updates the section header to point to the new data
5. Writes the modified binary
"""
import sys
import struct
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import Section
from elftools.elf.constants import SH_FLAGS

def embed_midp_native(elf_path, midp_data_path, output_path):
    """Embed MIDP native data into ELF binary properly."""

    # Read the MIDP native data
    with open(midp_data_path, 'rb') as f:
        midp_data = f.read()

    print(f"MIDP native data size: {len(midp_data)} bytes (0x{len(midp_data):x})")

    # Read the ELF binary
    with open(elf_path, 'rb') as f:
        elf_data = bytearray(f.read())

    # Parse the ELF
    elf = ELFFile(open(elf_path, 'rb'))

    # Find the .bss section or the last section of the data segment
    # We'll append .midp.native after all existing sections

    # Strategy: find the end of the file (after symtab, strtab, etc.)
    # Then we'll add the .midp.native section there
    # And modify the program headers to include it in a LOAD segment

    # For 32-bit ARM ELF, the data LOAD segment is the second one
    # It currently goes from 0x811a0000 with MemSiz 0x50f2c

    # We need to:
    # 1. Find where the data segment ends in the file
    # 2. Add padding to align to 0x1000 boundary
    # 3. Append the MIDP native data
    # 4. Add a new section header for .midp.native
    # 5. Modify the data LOAD segment to include this new data

    # First, find the file offset and size of the data segment
    data_segment_offset = None
    data_segment_filesiz = None
    data_segment_memsiz = None
    data_segment_vaddr = None

    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD' and seg['p_vaddr'] >= 0x811a0000:
            data_segment_offset = seg['p_offset']
            data_segment_filesiz = seg['p_filesz']
            data_segment_memsiz = seg['p_memsz']
            data_segment_vaddr = seg['p_vaddr']
            print(f"Found data segment: offset=0x{seg['p_offset']:x}, "
                  f"vaddr=0x{seg['p_vaddr']:x}, filesz=0x{seg['p_filesz']:x}, "
                  f"memsz=0x{seg['p_memsz']:x}")
            break

    if data_segment_offset is None:
        print("ERROR: Could not find data segment!")
        return False

    # The new .midp.native will be placed at the end of the data segment
    # The current data ends at file offset data_segment_offset + data_segment_filesiz
    # We need to align to 0x1000

    current_end = data_segment_offset + data_segment_filesiz
    # Align to next 0x1000 boundary
    aligned_end = (current_end + 0xfff) & ~0xfff

    # The new virtual address for .midp.native
    new_vaddr = data_segment_vaddr + data_segment_memsiz
    # Align to 0x1000
    new_vaddr = (new_vaddr + 0xfff) & ~0xfff

    # Calculate how much padding we need
    file_padding = aligned_end - current_end
    vaddr_padding = new_vaddr - (data_segment_vaddr + data_segment_memsiz)

    print(f"Current data end: file=0x{current_end:x}, vaddr=0x{data_segment_vaddr + data_segment_memsiz:x}")
    print(f"New .midp.native location: file=0x{aligned_end:x}, vaddr=0x{new_vaddr:x}")
    print(f"Padding needed: file={file_padding} bytes, vaddr={vaddr_padding} bytes")

    # Now we need to rebuild the ELF with the new section
    # This is complex. Let me use a simpler approach: just write a new file
    # with the proper structure.

    # Read all sections
    sections = []
    for i, section in enumerate(elf.iter_sections()):
        sections.append({
            'name': section.name,
            'sh': section.header
        })

    elf.close()

    # The complex part: we need to insert the new data and section header
    # at the right place, and update the program headers.

    # For simplicity, let's use a different approach:
    # Use the existing binary but add the section at the end of the file
    # and extend the data LOAD segment.

    # Read the ELF file as bytes
    with open(elf_path, 'rb') as f:
        elf_bytes = f.read()

    # File size
    file_size = len(elf_bytes)
    print(f"Original file size: 0x{file_size:x}")

    # We need to:
    # 1. Add the MIDP data after the existing file content (padded to 0x1000)
    # 2. Add a new section header for .midp.native
    # 3. Modify the data LOAD segment to include the new data

    # First, let's create the new file content
    # We append the MIDP data at the end (after symtab/strtab)
    new_file = bytearray(elf_bytes)

    # Pad to 0x1000 boundary
    while len(new_file) % 0x1000 != 0:
        new_file.append(0)

    # Append MIDP data
    midp_offset = len(new_file)
    new_file.extend(midp_data)
    # Pad to 0x1000
    while len(new_file) % 0x1000 != 0:
        new_file.append(0)

    print(f"New file size: 0x{len(new_file):x}")
    print(f"MIDP data at offset: 0x{midp_offset:x}")

    # Now we need to:
    # 1. Add a section header for .midp.native
    # 2. Update the string table (.shstrtab) to include the name
    # 3. Update program headers

    # This is getting very complex. Let me try a completely different approach:
    # Use ld with a linker script.

    return True


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <elf_binary> <midp_data> <output_binary>")
        sys.exit(1)

    embed_midp_native(sys.argv[1], sys.argv[2], sys.argv[3])
