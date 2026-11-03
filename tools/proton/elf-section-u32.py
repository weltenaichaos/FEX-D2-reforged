#!/usr/bin/env python3
"""Prints the 32-bit values stored in one section of a 64-bit little-endian ELF file
(e.g. ntdll.so's d2r_vm_sites, the line of every place that takes the memory lock),
one per line in file order.
  elf-section-u32.py <elf file> <section name>
Exits 1 when the file isn't such an ELF or has no section of that name."""
import struct
import sys


def main():
    path, name = sys.argv[1], sys.argv[2].encode()
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    if not shoff or shstrndx >= shnum:
        sys.exit("no section headers")

    def header(i):
        # sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size
        return struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)

    strtab_off = header(shstrndx)[4]
    for i in range(shnum):
        sh_name, sh_type, _, _, offset, size = header(i)
        end = data.index(b"\0", strtab_off + sh_name)
        if data[strtab_off + sh_name:end] != name:
            continue
        if sh_type == 8:  # SHT_NOBITS
            sys.exit("section has no file data")
        for value, in struct.iter_unpack("<I", data[offset:offset + size - size % 4]):
            print(value)
        return
    sys.exit("no section " + name.decode())


main()
