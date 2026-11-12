"""Settings -> How to Help's QR code (assets/xmb/help-qr.png, 00.90): EmuSuNi's GitHub page, which a phone opens (the
PS4 opens no link). A QR code of the address in byte mode, error correction level M, the smallest version it fits (4:
33 x 33 modules), the mask with the lowest penalty - ISO/IEC 18004, as Project Nayuki's QR Code generator (MIT) does it -,
drawn black on white with the 4-module quiet zone, 5 pixels a module (205 x 205: the menu draws it at its own size), as
an RGBA PNG. The Python standard library only. Run it again only to change the address:
    python3 -I scripts/make-help-qr.py assets/xmb/help-qr.png
(01.04) Another address's, the same way: the group on Telegram (How to Help, Send Log's code):
    python3 -I scripts/make-help-qr.py assets/xmb/telegram-qr.png https://t.me/+A53DraC3it5mZTcx"""
import struct
import sys
import zlib

URL = "https://emusuni.com"  # (00.95: the website, emusuni.com - before, the GitHub page)
SCALE, BORDER = 5, 4
# (00.95) Version 4 at least: 33 modules and the quiet zone, (33 + 8) x 5 = 205 pixels, the size the menu draws it at (a
# shorter address would fit a smaller version, and be drawn blurred).
LEAST_VERSION = 4

# Error correction level M (versions 1 to 9: byte mode's count is 8 bits there).
ECL_FORMAT = 0  # M's format bits
ECC_PER_BLOCK = [None, 10, 16, 26, 18, 24, 16, 18, 22, 22]
NUM_BLOCKS = [None, 1, 1, 1, 2, 2, 4, 4, 4, 5]


def raw_modules(ver):
    result = (16 * ver + 128) * ver + 64
    if ver >= 2:
        n = ver // 7 + 2
        result -= (25 * n - 10) * n - 55
        if ver >= 7:
            result -= 36
    return result


def data_codewords(ver):
    return raw_modules(ver) // 8 - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver]


def gf_mul(x, y):
    z = 0
    for i in reversed(range(8)):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z


def rs_divisor(degree):
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = gf_mul(result[j], root)
            if j + 1 < degree:
                result[j] ^= result[j + 1]
        root = gf_mul(root, 0x02)
    return result


def rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for b in data:
        factor = b ^ result.pop(0)
        result.append(0)
        for i, coef in enumerate(divisor):
            result[i] ^= gf_mul(coef, factor)
    return result


def codewords(text, least=1):
    data = text.encode("utf-8")
    ver = next(v for v in range(least, 10) if 4 + 8 + 8 * len(data) <= data_codewords(v) * 8)
    bits = []

    def put(value, n):
        bits.extend((value >> i) & 1 for i in reversed(range(n)))

    put(0b0100, 4)
    put(len(data), 8)
    for b in data:
        put(b, 8)
    capacity = data_codewords(ver) * 8
    put(0, min(4, capacity - len(bits)))
    put(0, -len(bits) % 8)
    pad = 0xEC
    while len(bits) < capacity:
        put(pad, 8)
        pad ^= 0xEC ^ 0x11
    words = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    # The blocks, each with its error correction, interleaved.
    blocks_n, ecc_len = NUM_BLOCKS[ver], ECC_PER_BLOCK[ver]
    raw = raw_modules(ver) // 8
    short_n = blocks_n - raw % blocks_n
    short_len = raw // blocks_n
    divisor = rs_divisor(ecc_len)
    blocks, k = [], 0
    for i in range(blocks_n):
        dat = words[k:k + short_len - ecc_len + (0 if i < short_n else 1)]
        k += len(dat)
        ecc = rs_remainder(dat, divisor)
        if i < short_n:
            dat.append(0)
        blocks.append(dat + ecc)
    result = []
    for i in range(len(blocks[0])):
        for j, block in enumerate(blocks):
            if i != short_len - ecc_len or j >= short_n:
                result.append(block[i])
    return ver, result


class Qr:
    def __init__(self, ver):
        self.ver = ver
        self.size = ver * 4 + 17
        self.dark = [[False] * self.size for _ in range(self.size)]
        self.func = [[False] * self.size for _ in range(self.size)]

    def set_func(self, x, y, dark):
        self.dark[y][x] = dark
        self.func[y][x] = True

    def draw_functions(self):
        s = self.size
        for i in range(s):
            self.set_func(6, i, i % 2 == 0)
            self.set_func(i, 6, i % 2 == 0)
        for cx, cy in ((3, 3), (s - 4, 3), (3, s - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < s and 0 <= y < s:
                        self.set_func(x, y, max(abs(dx), abs(dy)) not in (2, 4))
        pos = self.alignment_positions()
        n = len(pos)
        for i in range(n):
            for j in range(n):
                if (i, j) in ((0, 0), (0, n - 1), (n - 1, 0)):
                    continue
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        self.set_func(pos[i] + dx, pos[j] + dy, max(abs(dx), abs(dy)) != 1)
        self.draw_format(0)

    def alignment_positions(self):
        if self.ver == 1:
            return []
        n = self.ver // 7 + 2
        step = (self.ver * 8 + n * 3 + 5) // (n * 4 - 4) * 2
        result = [self.size - 7 - i * step for i in range(n - 1)] + [6]
        return list(reversed(result))

    def draw_format(self, mask):
        data = ECL_FORMAT << 3 | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = (data << 10 | rem) ^ 0x5412
        bit = lambda i: (bits >> i) & 1 != 0
        s = self.size
        for i in range(6):
            self.set_func(8, i, bit(i))
        self.set_func(8, 7, bit(6))
        self.set_func(8, 8, bit(7))
        self.set_func(7, 8, bit(8))
        for i in range(9, 15):
            self.set_func(14 - i, 8, bit(i))
        for i in range(8):
            self.set_func(s - 1 - i, 8, bit(i))
        for i in range(8, 15):
            self.set_func(8, s - 15 + i, bit(i))
        self.set_func(8, s - 8, True)

    def draw_codewords(self, words):
        s, i = self.size, 0
        right = s - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(s):
                for j in range(2):
                    x = right - j
                    upward = ((right + 1) & 2) == 0
                    y = s - 1 - vert if upward else vert
                    if not self.func[y][x] and i < len(words) * 8:
                        self.dark[y][x] = (words[i >> 3] >> (7 - (i & 7))) & 1 != 0
                        i += 1
            right -= 2

    def apply_mask(self, mask):
        tests = [
            lambda x, y: (x + y) % 2 == 0,
            lambda x, y: y % 2 == 0,
            lambda x, y: x % 3 == 0,
            lambda x, y: (x + y) % 3 == 0,
            lambda x, y: (x // 3 + y // 2) % 2 == 0,
            lambda x, y: x * y % 2 + x * y % 3 == 0,
            lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
            lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0,
        ]
        for y in range(self.size):
            for x in range(self.size):
                if not self.func[y][x] and tests[mask](x, y):
                    self.dark[y][x] = not self.dark[y][x]

    def penalty(self):
        s, m, score = self.size, self.dark, 0
        lines = [row for row in m] + [[m[y][x] for y in range(s)] for x in range(s)]
        finder = [True, False, True, True, True, False, True]
        for line in lines:
            run, prev = 0, None
            for v in line:
                if v == prev:
                    run += 1
                else:
                    if run >= 5:
                        score += 3 + run - 5
                    run, prev = 1, v
            if run >= 5:
                score += 3 + run - 5
            padded = [False] * 4 + line + [False] * 4
            for i in range(len(padded) - 6):
                if padded[i:i + 7] == finder:
                    if padded[max(0, i - 4):i] == [False] * 4 or padded[i + 7:i + 11] == [False] * 4:
                        score += 40
        for y in range(s - 1):
            for x in range(s - 1):
                if m[y][x] == m[y][x + 1] == m[y + 1][x] == m[y + 1][x + 1]:
                    score += 3
        dark = sum(v for row in m for v in row)
        total = s * s
        k = (abs(dark * 20 - total * 10) + total - 1) // total - 1
        return score + k * 10


def make(text, least=1):
    ver, words = codewords(text, least)
    best, best_score = None, None
    for mask in range(8):
        qr = Qr(ver)
        qr.draw_functions()
        qr.draw_codewords(words)
        qr.apply_mask(mask)
        qr.draw_format(mask)
        score = qr.penalty()
        if best_score is None or score < best_score:
            best, best_score = qr, score
    return best


def png(qr, path):
    n = (qr.size + 2 * BORDER) * SCALE
    rows = []
    for py in range(n):
        y = py // SCALE - BORDER
        row = bytearray([0])
        for px in range(n):
            x = px // SCALE - BORDER
            on = 0 <= x < qr.size and 0 <= y < qr.size and qr.dark[y][x]
            row += bytes((0, 0, 0, 255)) if on else bytes((255, 255, 255, 255))
        rows.append(bytes(row))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(b"".join(rows), 9)))
        f.write(chunk(b"IEND", b""))
    return n


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/xmb/help-qr.png"
    code = make(sys.argv[2] if len(sys.argv) > 2 else URL, LEAST_VERSION)
    n = png(code, out)
    print("%s: version %d (%d x %d modules), %d x %d pixels" % (out, code.ver, code.size, code.size, n, n))
