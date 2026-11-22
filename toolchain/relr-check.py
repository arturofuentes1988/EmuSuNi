"""(01.27) PS4_RELR's check on the PC: the program image relocated as the loader did before 01.27 against the image as
it is with PS4_RELR, byte for byte.

    PS4_RELR_KEEP_PREPACK=1 (the link through toolchain/relr-link.py)  ->  <elf>.prepack (as linked) and <elf> (packed)
    python3 -I toolchain/relr-check.py <elf>.prepack <elf>

  - Before: every relocation of the linked ELF's .rela.dyn and .rela.plt applied as the loader does (load address B;
    a symbol the program doesn't define gets a made-up address, the same in both).
  - After: the packed ELF's (what's left of .rela.dyn, .rela.plt) the same way, then port/ps4_relr.c itself (compiled
    here with PS4_RELR_TEST) over that image mapped at B, .data.rel.ro's pages read-only first (as the loader leaves
    them): it has to make them writable, and give them back.
  - The loaded segments compared, all but the table itself (.emusuni_relr) and .dynamic (DT_RELASZ, DT_RELACOUNT)."""
import importlib.util
import mmap
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("relr_link", os.path.join(HERE, "relr-link.py"))
relr_link = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relr_link)
Elf = relr_link.Elf

BASE = 0x500000000000
PT_LOAD, PT_GNU_RELRO = 1, 0x6474e552
PAGE = 0x4000

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
struct Ps4RelrTable;
const struct Ps4RelrTable* ps4_relr_test_table;
void ps4_relr_apply(void);
static uintptr_t lo, hi;
static uint32_t current = 1;
static int calls;
static int to_linux(int32_t prot) { return (prot & 1 ? PROT_READ : 0) | (prot & 2 ? PROT_WRITE | PROT_READ : 0); }
int32_t ps4_relr_test_query(void* addr, void** start, void** end, uint32_t* prot) {
  if ((uintptr_t)addr < lo || (uintptr_t)addr >= hi) { fprintf(stderr, "query outside RELRO\n"); exit(3); }
  *start = (void*)lo; *end = (void*)hi; *prot = current; return 0;
}
int32_t ps4_relr_test_mprotect(const void* addr, size_t len, int32_t prot) {
  printf("mprotect +%#lx %#zx %d\n", (unsigned long)((uintptr_t)addr - BASE_ADDR), len, prot);
  if ((uintptr_t)addr < lo || (uintptr_t)addr + len > hi || ((uintptr_t)addr & 0x3fff) || (len & 0x3fff)) {
    fprintf(stderr, "mprotect outside RELRO or not on 16 KiB pages\n"); exit(3);
  }
  calls++;
  current = (uint32_t)prot;
  return mprotect((void*)addr, len, to_linux(prot));
}
void ps4_relr_test_fail(const char* what, int32_t code) { fprintf(stderr, "FAIL %s %#x\n", what, code); exit(2); }
int main(int argc, char** argv) {
  size_t size = strtoull(argv[2], 0, 0);
  uintptr_t table = strtoull(argv[3], 0, 0);
  lo = BASE_ADDR + strtoull(argv[4], 0, 0); hi = BASE_ADDR + strtoull(argv[5], 0, 0);
  unsigned char* image = mmap((void*)BASE_ADDR, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (image != (void*)BASE_ADDR) { perror("mmap"); return 1; }
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(image, 1, size, f) != size) { perror("read"); return 1; }
  fclose(f);
  if (hi > lo && mprotect((void*)lo, hi - lo, PROT_READ) != 0) { perror("mprotect"); return 1; }
  ps4_relr_test_table = (const struct Ps4RelrTable*)(BASE_ADDR + table);
  ps4_relr_apply();
  int first = calls;
  ps4_relr_apply();  /* once only */
  printf("mprotect calls %d, then %d; protection at the end %u\n", first, calls - first, current);
  if (hi > lo) mprotect((void*)lo, hi - lo, PROT_READ | PROT_WRITE);
  f = fopen(argv[6], "wb");
  if (!f || fwrite(image, 1, size, f) != size) { perror("write"); return 1; }
  fclose(f);
  return 0;
}
"""


def loaded_segments(elf):
    """The PT_LOAD segments create-fself keeps: all but the one .rela.dyn is in (past the image)."""
    rela_addr = elf.section(".rela.dyn")[3]
    return [p for p in elf.phdrs if p[0] == PT_LOAD and not (p[3] <= rela_addr < p[3] + p[6])]


def symbol_values(elf):
    """.dynsym: each symbol's address once loaded at BASE, or a made-up one for those the program doesn't define."""
    _, _, _, _, offset, size, link = elf.section(".dynsym")
    stroff = elf.by_index[link][5]
    values = []
    for i in range(size // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", elf.data, offset + i * 24)
        name = elf.data[stroff + st_name:elf.data.find(b"\0", stroff + st_name)]
        values.append(BASE + st_value if st_shndx != 0 else 0x7f0000000000 + (zlib.crc32(name) << 12))
    return values


def image_of(elf, size):
    image = bytearray(size)
    for p in loaded_segments(elf):
        image[p[3]:p[3] + p[5]] = elf.data[p[2]:p[2] + p[5]]
    return image


def relocate(elf, image, relasz):
    """The loader's part: .rela.dyn (relasz bytes of it) and .rela.plt."""
    values = symbol_values(elf)
    counts = {}
    for name, size in ((".rela.dyn", relasz), (".rela.plt", None)):
        sec = elf.section(name)
        size = sec[5] if size is None else size
        for off, info, addend in struct.iter_unpack("<QQq", elf.data[sec[4]:sec[4] + size]):
            kind, sym = info & 0xffffffff, info >> 32
            if kind == 8:  # RELATIVE
                value = BASE + addend
            elif kind == 1:  # 64
                value = values[sym] + addend
            elif kind in (6, 7):  # GLOB_DAT, JUMP_SLOT
                value = values[sym]
            else:
                sys.exit("relr-check.py: relocation of type %d" % kind)
            if off + 8 > len(image):
                sys.exit("relr-check.py: a relocation past the file's bytes (%#x)" % off)
            struct.pack_into("<Q", image, off, value & 0xffffffffffffffff)
            counts[kind] = counts.get(kind, 0) + 1
    return counts


def dynamic_value(elf, tag):
    sec = elf.section(".dynamic")
    for i in range(sec[5] // 16):
        t, v = struct.unpack_from("<qQ", elf.data, sec[4] + i * 16)
        if t == tag:
            return v
        if t == 0:
            return None
    return None


def main():
    before_path, after_path = sys.argv[1], sys.argv[2]
    src = os.path.join(HERE, "..", "port", "ps4_relr.c")
    with open(before_path, "rb") as f1, open(after_path, "rb") as f2:
        before = Elf(mmap.mmap(f1.fileno(), 0, access=mmap.ACCESS_READ))
        after = Elf(mmap.mmap(f2.fileno(), 0, access=mmap.ACCESS_READ))
        # The same layout: sections and segments (.rela.dyn's size aside).
        for b, a in zip(before.by_index, after.by_index):
            if b[:5] != a[:5] or (b[6] != a[6] and b[0] != ".rela.dyn"):
                sys.exit("relr-check.py: the sections differ: %s / %s" % (b[0], a[0]))
        if before.phdrs != after.phdrs or len(before.by_index) != len(after.by_index):
            sys.exit("relr-check.py: the segments differ")
        size = max(p[3] + p[5] for p in loaded_segments(after))
        relro = [p for p in after.phdrs if p[0] == PT_GNU_RELRO][0]
        relro_lo, relro_hi = relro[3] // PAGE * PAGE, (relro[3] + relro[6] + PAGE - 1) // PAGE * PAGE
        table = after.section(".emusuni_relr")
        dyn = after.section(".dynamic")
        header = struct.unpack_from("<6Q", after.data, table[4])
        print("table: %d words for %d pointers; pages made writable %#x-%#x (RELRO %#x-%#x)"
              % (header[2], header[3], header[4], header[5], relro_lo, relro_hi))
        # Before: everything by the loader.
        reference = image_of(before, size)
        counts = relocate(before, reference, before.section(".rela.dyn")[5])
        print("before: the loader's %s" % ", ".join("%d of type %d" % (n, k) for k, n in sorted(counts.items())))
        # After: the loader's part, then port/ps4_relr.c.
        image = image_of(after, size)
        relasz = dynamic_value(after, 8)
        counts = relocate(after, image, relasz)
        print("after: the loader's %s; DT_RELASZ %d (was %d), DT_RELACOUNT %s (was %s)"
              % (", ".join("%d of type %d" % (n, k) for k, n in sorted(counts.items())), relasz,
                 dynamic_value(before, 8), dynamic_value(after, 0x6ffffff9), dynamic_value(before, 0x6ffffff9)))
        with tempfile.TemporaryDirectory() as tmp:
            harness = os.path.join(tmp, "harness")
            subprocess.run(["cc", "-O2", "-DPS4_RELR_TEST", "-DBASE_ADDR=%#xUL" % BASE, "-x", "c", "-",
                            src, "-o", harness], input=HARNESS.encode(), check=True)
            with open(os.path.join(tmp, "in"), "wb") as f:
                f.write(image)
            del image
            subprocess.run([harness, os.path.join(tmp, "in"), str(size), str(table[3]), str(relro_lo), str(relro_hi),
                            os.path.join(tmp, "out")], check=True)
            relocated = open(os.path.join(tmp, "out"), "rb").read()
        skip = [(table[3], table[3] + table[5]), (dyn[3], dyn[3] + dyn[5])]
        different = 0
        compared = 0
        for p in loaded_segments(after):
            lo, hi = p[3], p[3] + p[5]
            cuts = sorted([lo] + [x for s in skip for x in s if lo < x < hi] + [hi])
            for x, y in zip(cuts, cuts[1:]):
                if any(s[0] <= x and y <= s[1] for s in skip):
                    continue
                compared += y - x
                if reference[x:y] != relocated[x:y]:
                    for k in range(x, y, 8):
                        if reference[k:k + 8] != relocated[k:k + 8]:
                            if different < 10:
                                print("  differs at %#x: %s / %s" % (k, reference[k:k + 8].hex(), relocated[k:k + 8].hex()))
                            different += 1
        print("compared %.1f MB of the loaded segments: %d words different" % (compared / 1e6, different))
        sys.exit(1 if different else 0)


if __name__ == "__main__":
    main()
