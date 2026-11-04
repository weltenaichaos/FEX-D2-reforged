#!/usr/bin/env python3
"""Prints the 64-bit value stored at a data symbol of a 64-bit little-endian ELF file
(e.g. ntdll.so's d2r_ev_life_ns, the event reuse window's default), as a decimal number.
  elf-symbol-u64.py <elf file> <symbol name>
Exits 1 when the file isn't such an ELF, has no such symbol in .symtab, or the symbol's
section has no file data."""
import struct
import sys


def main():
    path, name = sys.argv[1], sys.argv[2].encode()
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, _ = struct.unpack_from("<HHH", data, 0x3a)
    if not shoff:
        sys.exit("no section headers")

    def header(i):
        # sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link
        return struct.unpack_from("<IIQQQQI", data, shoff + i * shentsize)

    for i in range(shnum):
        _, sh_type, _, _, offset, size, link = header(i)
        if sh_type != 2:  # SHT_SYMTAB
            continue
        stroff = header(link)[4]
        for pos in range(offset, offset + size, 24):
            st_name, _, _, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, pos)
            end = data.index(b"\0", stroff + st_name)
            if data[stroff + st_name:end] != name or st_size < 8 or not 0 < st_shndx < shnum:
                continue
            _, s_type, _, s_addr, s_offset, _, _ = header(st_shndx)
            if s_type == 8:  # SHT_NOBITS
                sys.exit("symbol has no file data")
            value, = struct.unpack_from("<Q", data, s_offset + st_value - s_addr)
            print(value)
            return
    sys.exit("no symbol " + name.decode())


main()
