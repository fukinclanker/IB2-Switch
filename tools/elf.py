"""Small dependency-free ELF dynamic-table reader used by local tooling."""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterable


class ElfError(ValueError):
    pass


@dataclass(frozen=True)
class ElfInfo:
    bits: int
    machine: int
    needed: tuple[str, ...]
    undefined: tuple[str, ...]
    exports: tuple[str, ...]


def _cstring(data: bytes, start: int) -> str:
    if start < 0 or start >= len(data):
        raise ElfError(f"string offset outside table: {start}")
    end = data.find(b"\0", start)
    if end < 0:
        end = len(data)
    return data[start:end].decode("utf-8", "replace")


def _vaddr_to_offset(loads: Iterable[tuple[int, int, int]], address: int) -> int:
    for vaddr, offset, filesz in loads:
        if vaddr <= address < vaddr + filesz:
            return offset + address - vaddr
    raise ElfError(f"virtual address is not file-backed: 0x{address:x}")


def parse_elf(data: bytes) -> ElfInfo:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    elf_class, encoding = data[4], data[5]
    if elf_class not in (1, 2):
        raise ElfError(f"unsupported ELF class: {elf_class}")
    if encoding != 1:
        raise ElfError("only little-endian ELF files are supported")

    bits = 32 if elf_class == 1 else 64
    prefix = "<"
    if bits == 64:
        header = struct.unpack_from(prefix + "16sHHIQQQIHHHHHH", data, 0)
        machine, phoff, phentsize, phnum = header[2], header[5], header[9], header[10]
        ph_fmt = prefix + "IIQQQQQQ"
        dyn_fmt = prefix + "qQ"
        sym_fmt = prefix + "IBBHQQ"
    else:
        header = struct.unpack_from(prefix + "16sHHIIIIIHHHHHH", data, 0)
        machine, phoff, phentsize, phnum = header[2], header[5], header[9], header[10]
        ph_fmt = prefix + "IIIIIIII"
        dyn_fmt = prefix + "iI"
        sym_fmt = prefix + "IIIBBH"

    expected_ph = struct.calcsize(ph_fmt)
    if phentsize < expected_ph:
        raise ElfError("program header entries are truncated")

    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None
    for index in range(phnum):
        pos = phoff + index * phentsize
        if pos + expected_ph > len(data):
            raise ElfError("program header table is truncated")
        values = struct.unpack_from(ph_fmt, data, pos)
        if bits == 64:
            p_type, p_offset, p_vaddr, p_filesz = values[0], values[2], values[3], values[5]
        else:
            p_type, p_offset, p_vaddr, p_filesz = values[0], values[1], values[2], values[4]
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dynamic = (p_offset, p_filesz)

    if dynamic is None:
        raise ElfError("ELF has no PT_DYNAMIC segment")

    tags: dict[int, list[int]] = {}
    dyn_size = struct.calcsize(dyn_fmt)
    dyn_offset, dyn_filesz = dynamic
    for pos in range(dyn_offset, dyn_offset + dyn_filesz, dyn_size):
        if pos + dyn_size > len(data):
            raise ElfError("dynamic table is truncated")
        tag, value = struct.unpack_from(dyn_fmt, data, pos)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)

    def one(tag: int) -> int:
        try:
            return tags[tag][0]
        except KeyError as exc:
            raise ElfError(f"missing dynamic tag {tag}") from exc

    strtab_off = _vaddr_to_offset(loads, one(5))  # DT_STRTAB
    strtab_size = one(10)  # DT_STRSZ
    if strtab_off + strtab_size > len(data):
        raise ElfError("dynamic string table is truncated")
    strings = data[strtab_off : strtab_off + strtab_size]
    needed = tuple(_cstring(strings, offset) for offset in tags.get(1, ()))

    symtab_off = _vaddr_to_offset(loads, one(6))  # DT_SYMTAB
    syment = one(11)  # DT_SYMENT
    expected_sym = struct.calcsize(sym_fmt)
    if syment < expected_sym:
        raise ElfError("dynamic symbol entries are truncated")

    # Android objects normally carry a SysV DT_HASH even if they also have GNU
    # hash. Its nchain field is an exact, bounds-safe dynamic symbol count.
    if 4 in tags:
        hash_off = _vaddr_to_offset(loads, tags[4][0])
        if hash_off + 8 > len(data):
            raise ElfError("DT_HASH header is truncated")
        _, symbol_count = struct.unpack_from(prefix + "II", data, hash_off)
    elif 0x6FFFFEF5 in tags:  # DT_GNU_HASH
        hash_off = _vaddr_to_offset(loads, tags[0x6FFFFEF5][0])
        if hash_off + 16 > len(data):
            raise ElfError("DT_GNU_HASH header is truncated")
        bucket_count, symbol_offset, bloom_size, _ = struct.unpack_from(
            prefix + "IIII", data, hash_off
        )
        word_size = 8 if bits == 64 else 4
        buckets_off = hash_off + 16 + bloom_size * word_size
        buckets_end = buckets_off + bucket_count * 4
        if buckets_end > len(data):
            raise ElfError("DT_GNU_HASH buckets are truncated")
        buckets = struct.unpack_from(prefix + f"{bucket_count}I", data, buckets_off)
        highest = max(buckets, default=0)
        if highest < symbol_offset:
            symbol_count = symbol_offset
        else:
            chain_off = buckets_end + (highest - symbol_offset) * 4
            symbol_count = highest
            while True:
                if chain_off + 4 > len(data):
                    raise ElfError("DT_GNU_HASH chain is truncated")
                value = struct.unpack_from(prefix + "I", data, chain_off)[0]
                symbol_count += 1
                chain_off += 4
                if value & 1:
                    break
    else:
        # Fall back to the distance to the string table. This is conservative
        # and works for the supplied objects, but DT_HASH is preferred.
        if strtab_off <= symtab_off:
            raise ElfError("cannot determine dynamic symbol count")
        symbol_count = (strtab_off - symtab_off) // syment

    undefined: set[str] = set()
    exports: set[str] = set()
    for index in range(symbol_count):
        pos = symtab_off + index * syment
        if pos + expected_sym > len(data):
            raise ElfError("dynamic symbol table is truncated")
        fields = struct.unpack_from(sym_fmt, data, pos)
        if bits == 64:
            name_offset, info, shndx = fields[0], fields[1], fields[3]
        else:
            name_offset, info, shndx = fields[0], fields[3], fields[5]
        binding = info >> 4
        if not name_offset or binding not in (1, 2):  # global or weak
            continue
        name = _cstring(strings, name_offset)
        if not name:
            continue
        if shndx == 0:
            undefined.add(name)
        else:
            exports.add(name)

    return ElfInfo(
        bits=bits,
        machine=machine,
        needed=tuple(sorted(set(needed))),
        undefined=tuple(sorted(undefined)),
        exports=tuple(sorted(exports)),
    )
