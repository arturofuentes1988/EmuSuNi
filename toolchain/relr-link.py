"""(01.27) PS4_RELR's link (toolchain/ps4-love-style.cmake runs the eboot's link through this, RULE_LAUNCH_LINK):
the link as CMake wrote it, then the program's own data pointers packed into its RELR table (port/ps4_relr.c).

Every R_X86_64_RELATIVE of .rela.dyn that is
  - in .data.rel.ro or .data (--no-relro: .data only),
  - 8-aligned,
  - not in crt1's libc tables (the symbols _sce*: Sony's libc reads them before the program starts)
goes into .emusuni_relr as RELR words (an even word: an address and the pointer there; an odd one: a bitmap of the 63
words after the last), its link-time value (the addend) written in its place in the image. .rela.dyn keeps the rest
(the GOT, .init_array, the TLS image, the process parameters...) for the loader, DT_RELASZ and DT_RELACOUNT with it:
create-fself copies only those into the eboot. .rela.dyn is past the loaded image (PS4_RELA_IN_IMAGE off), so nothing
moves: only bytes change, the table's room (__emusuni_relr_reserve, linked in) and the pointers' words.

The room is the last link's (<reserve file>); a table that doesn't fit links again with room for it (5% more). The
pattern of the pointers doesn't change with the room: the data after it moves by whole 16 KiB pages, all together.
PS4_RELR_KEEP_PREPACK=1 keeps the ELF as linked (<out>.prepack) for toolchain/relr-check.py.

    python3 -I relr-link.py [--no-relro] --reserve-file <file> -- <linker> <arguments>..."""
import bisect
import os
import shutil
import struct
import subprocess
import sys

R_X86_64_RELATIVE = 8
DT_NULL, DT_RELASZ, DT_RELACOUNT = 0, 8, 0x6ffffff9
MAGIC = 0x31524c4552554d45  # "EMURELR1"
HEADER = 6 * 8              # port/ps4_relr.c's struct Ps4RelrTable
PAGE = 0x4000
# crt1's tables (port/ps4_crt1.S), each with its size in the symbol table: their pointers stay with the loader.
CRT_TABLES = ("_sceLibcParam", "_sceKernelMemParam", "_sceKernelFsParam", "_sceLibcMallocReplace",
              "_sceLibcNewReplace", "_sceLibcMallocReplaceForTls")


def fail(message):
    sys.exit("relr-link.py: " + message)


class Elf:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            fail("not a 64-bit little-endian ELF")
        (self.phoff, self.shoff) = struct.unpack_from("<QQ", data, 0x20)
        (self.phentsize, self.phnum, self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
        self.phdrs = [struct.unpack_from("<IIQQQQQQ", data, self.phoff + i * self.phentsize) for i in range(self.phnum)]
        raw = [struct.unpack_from("<IIQQQQIIQQ", data, self.shoff + i * self.shentsize) for i in range(self.shnum)]
        strtab = raw[self.shstrndx][4]
        self.sections = {}  # name -> (index, type, flags, addr, offset, size, link)
        self.by_index = []
        for i, s in enumerate(raw):
            start = strtab + s[0]
            name = data[start:data.find(b"\0", start)].decode()
            entry = (i, s[1], s[2], s[3], s[4], s[5], s[6])
            self.by_index.append((name,) + entry)
            self.sections.setdefault(name, entry)

    def section(self, name):
        if name not in self.sections:
            fail("no section " + name)
        return self.sections[name]

    def header_offset(self, index):
        return self.shoff + index * self.shentsize

    def symbols(self, names):
        """{name: (value, size)} of the .symtab symbols named so."""
        _, _, _, _, offset, size, link = self.section(".symtab")
        stroff = self.by_index[link][5]
        wanted = {n.encode() for n in names}
        found = {}
        for i in range(size // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", self.data,
                                                                                          offset + i * 24)
            start = stroff + st_name
            if self.data[start:start + 1] != b"_":
                continue
            name = bytes(self.data[start:self.data.find(b"\0", start)])
            if name in wanted:
                found[name.decode()] = (st_value, st_size)
        return found


def encode(addresses):
    """RELR words for sorted, distinct, 8-aligned addresses (the encoding of ELF's DT_RELR, as lld writes it)."""
    words = []
    i, n = 0, len(addresses)
    while i < n:
        words.append(addresses[i])
        where = addresses[i] + 8
        i += 1
        while i < n:
            bits = 0
            j = i
            while j < n and addresses[j] - where < 63 * 8:
                bits |= 1 << ((addresses[j] - where) // 8)
                j += 1
            if bits == 0:
                break
            words.append(bits << 1 | 1)
            i = j
            where += 63 * 8
    return words


def decode(words):
    """The addresses RELR words stand for (port/ps4_relr.c's loop)."""
    out = []
    where = 0
    for w in words:
        if w & 1 == 0:
            out.append(w)
            where = w + 8
        else:
            bits = w >> 1
            k = 0
            while bits:
                if bits & 1:
                    out.append(where + 8 * k)
                bits >>= 1
                k += 1
            where += 63 * 8
    return out


def pack(path, relro):
    """Packs <path> in place. None when done, else the room the words need (bytes)."""
    data = bytearray(open(path, "rb").read())
    elf = Elf(data)
    table = elf.section(".emusuni_relr")
    rela = elf.section(".rela.dyn")
    dynamic = elf.section(".dynamic")
    room = (table[5] - HEADER) // 8
    if table[5] < HEADER:
        fail(".emusuni_relr is smaller than its header")
    eligible = [".data"] + ([".data.rel.ro"] if relro else [])
    ranges = []
    for name in eligible:
        _, kind, flags, addr, offset, size, _ = elf.section(name)
        if kind != 1:  # PROGBITS: the pointers' words are in the file
            fail(name + " isn't PROGBITS")
        ranges.append((addr, addr + size, offset))
    ranges.sort()
    starts = [r[0] for r in ranges]
    tables = elf.symbols(CRT_TABLES)
    missing = [n for n in CRT_TABLES if n not in tables or tables[n][1] == 0]
    if missing:
        fail("crt1's tables without a size in the symbol table: " + ", ".join(missing))
    excluded = sorted((v, v + s) for v, s in tables.values())

    def offset_of(address):
        k = bisect.bisect_right(starts, address) - 1
        if k >= 0 and ranges[k][0] <= address and address + 8 <= ranges[k][1]:
            return ranges[k][2] + address - ranges[k][0]
        return None

    kept = []
    packed = []
    count = rela[5] // 24
    for i in range(count):
        entry = bytes(data[rela[4] + i * 24:rela[4] + i * 24 + 24])
        off, info, addend = struct.unpack("<QQq", entry)
        if (info & 0xffffffff) == R_X86_64_RELATIVE and off % 8 == 0 and offset_of(off) is not None \
                and not any(lo <= off < hi for lo, hi in excluded):
            packed.append((off, addend))
        else:
            kept.append(entry)
    packed.sort()
    addresses = [a for a, _ in packed]
    if len(set(addresses)) != len(addresses):
        fail("two relocations at one address")
    words = encode(addresses)
    if decode(words) != addresses:
        fail("the RELR words don't decode to the addresses")
    if len(words) > room:
        return len(words) * 8
    # The pages of .data.rel.ro with packed pointers: writable while relocating.
    protect_start = protect_end = 0
    if relro:
        lo, hi = elf.section(".data.rel.ro")[3], elf.section(".data.rel.ro")[3] + elf.section(".data.rel.ro")[5]
        inside = [a for a in addresses if lo <= a < hi]
        if inside:
            protect_start = inside[0] // PAGE * PAGE
            protect_end = (inside[-1] + 8 + PAGE - 1) // PAGE * PAGE
            relro_ph = [p for p in elf.phdrs if p[0] == 0x6474e552]
            if not relro_ph or not (relro_ph[0][3] <= protect_start and
                                    protect_end <= (relro_ph[0][3] + relro_ph[0][6] + PAGE - 1) // PAGE * PAGE):
                fail(".data.rel.ro's pages aren't in PT_GNU_RELRO")
    # Each pointer's link-time value in its place (lld leaves zeros there for RELA).
    for address, addend in packed:
        at = offset_of(address)
        old = struct.unpack_from("<q", data, at)[0]
        if old not in (0, addend):
            fail("the word at %#x already holds %#x, not 0 or %#x" % (address, old, addend))
        struct.pack_into("<q", data, at, addend)
    # The table.
    struct.pack_into("<6Q", data, table[4], MAGIC, table[3], len(words), len(packed), protect_start, protect_end)
    struct.pack_into("<%dQ" % len(words), data, table[4] + HEADER, *words)
    # .rela.dyn: the rest, in their order (lld's: the relative ones first).
    leading = 0
    for entry in kept:
        if struct.unpack_from("<Q", entry, 8)[0] & 0xffffffff != R_X86_64_RELATIVE:
            break
        leading += 1
    data[rela[4]:rela[4] + rela[5]] = b"".join(kept) + bytes(rela[5] - 24 * len(kept))
    struct.pack_into("<Q", data, elf.header_offset(rela[0]) + 0x20, 24 * len(kept))  # sh_size
    seen = set()
    for i in range(dynamic[5] // 16):
        at = dynamic[4] + i * 16
        tag = struct.unpack_from("<q", data, at)[0]
        if tag == DT_NULL:
            break
        if tag == DT_RELASZ:
            struct.pack_into("<Q", data, at + 8, 24 * len(kept))
            seen.add(tag)
        elif tag == DT_RELACOUNT:
            struct.pack_into("<Q", data, at + 8, leading)
            seen.add(tag)
    if DT_RELASZ not in seen:
        fail("no DT_RELASZ")
    with open(path + ".relr-tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".relr-tmp", path)
    print("relr-link.py: %d pointers in %d RELR words (%.2f MB, room for %d) instead of %.2f MB of .rela.dyn; %d "
          "relocations left to the loader (%.2f MB)%s" % (len(packed), len(words), len(words) * 8 / 1e6, room,
                                                        len(packed) * 24 / 1e6, len(kept), len(kept) * 24 / 1e6,
                                                        "" if relro else "; .data only (--no-relro)"))
    return None


def main():
    args = sys.argv[1:]
    relro = True
    reserve_file = None
    while args and args[0] != "--":
        a = args.pop(0)
        if a == "--no-relro":
            relro = False
        elif a == "--reserve-file" and args:
            reserve_file = args.pop(0)
        else:
            fail("unknown argument " + a)
    if not args or not reserve_file:
        fail("usage: relr-link.py [--no-relro] --reserve-file <file> -- <linker> <arguments>...")
    command = args[1:]
    if "-o" not in command:
        fail("no -o in the link")
    out = command[command.index("-o") + 1]
    try:
        reserve = int(open(reserve_file).read().split()[0])
    except (OSError, ValueError, IndexError):
        reserve = 0
    for _ in range(3):
        result = subprocess.call(command + ["--defsym=__emusuni_relr_reserve=%d" % reserve])
        if result != 0:
            sys.exit(result)
        if os.environ.get("PS4_RELR_KEEP_PREPACK") == "1":
            shutil.copyfile(out, out + ".prepack")
        try:
            needed = pack(out, relro)
        except (SystemExit, Exception) as e:  # pylint: disable=broad-except
            # The ELF as linked (pack writes only at the end): the loader relocates everything, as before 01.27.
            print("relr-link.py: WARNING: NOT PACKED (%s): every relocation left to the loader" % e)
            return
        if needed is None:
            return
        reserve = (needed * 105 // 100 + 0xfff) // 0x1000 * 0x1000
        print("relr-link.py: the table needs %d bytes: linked again with room for %d" % (needed, reserve))
        with open(reserve_file, "w") as f:
            f.write("%d\n" % reserve)
    fail("the table still doesn't fit")


if __name__ == "__main__":
    main()
