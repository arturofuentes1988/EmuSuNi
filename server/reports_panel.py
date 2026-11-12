"""The owner's panel (00.92): the log reports and the private statistics, in any browser, on the reports' own HTTPS
(server/emusuni-reports.py serve: the same port 443, Let's Encrypt's certificate for the server's IP made for the
reports' key - server/reports_acme.py - so the app's pin keeps working).

  - The reports: their list (date, version, country, the console and game the log says, size, status) with filters and
    a search; each one's files in a viewer (pages, a search, the error lines marked, only those); downloads; its
    status (new / reviewed / solved), a note and its GitHub issue; deleting it.
  - The statistics: emusuni-netd's stats.db (SQLite, read only: the same counts its "report" command gives, with no
    minimum of installs - it is the owner's), online now, by day, versions, countries, languages, consoles, games; and
    GitHub's downloads of each release (its public API, kept an hour).

Its door - the panel is on the internet:
  - A password (scrypt) and a TOTP code (RFC 6238: Google Authenticator and the like), set by the owner on the server
    only: sudo emusuni-reports admin-setup (asks the password unseen, shows the TOTP's QR in the terminal). Never in the
    repository nor a chat. Kept in <reports>/panel/admin.json (the service's user only).
  - 5 wrong tries from one address in 15 minutes lock it out 15 minutes; 30 wrong tries in all in an hour lock every
    login 15 minutes. The address is counted as a keyed hash in memory only (the key new each day), never kept or
    written - as for the reports.
  - The session: a random cookie (__Host-, HttpOnly, Secure, SameSite=Strict), 12 hours at most, 60 minutes idle;
    in memory (a restart logs out). Every form that changes something carries the session's CSRF token, and the
    browser's Origin must be the panel's ("null" only with Sec-Fetch-Site same-origin).
  - Headers: a CSP with no script at all (the panel has none), no frames, no sniffing, a referrer only to the panel
    itself (with "no-referrer" a browser sends its forms with Origin "null"), nothing cached.
In Spanish; English with the link at the bottom.
"""
import base64
import calendar
import getpass
import gzip
import hashlib
import hmac
import html
import io
import json
import math
import os
import re
import secrets
import shutil
import sqlite3
import struct
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request

ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
NAMES = ("emusuni.log", "emusuni-1.log", "crash.log")
STATUSES = ("new", "reviewed", "solved")
SESSION_SECONDS = 12 * 3600
IDLE_SECONDS = 60 * 60
TRIES, TRIES_SECONDS, LOCK_SECONDS = 5, 15 * 60, 15 * 60
ALL_TRIES, ALL_TRIES_SECONDS = 30, 3600
SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 15, 8, 1
MAX_FORM = 64 * 1024
PAGE_LINES = 1000
LIST_PAGE = 50
PAGE_SIZES = (25, 50, 100)  # (01.04) a long table's rows a page (Panel.pager)
GITHUB_SECONDS = 3600
SKETCH_ALL, SKETCH_LABEL, SKETCH_GAME = 14, 11, 8
LABEL_KINDS = ("country", "version", "language", "state", "console", "game", "community")  # ((01.04) community modes)
# (01.07) The quick phrases' English (src/NetplayProtocol.h's ChatPhrase: their numbers never change - a new one at the
# end), for the chat's page: a line stores its phrase's number.
PHRASES = ("", "Hello!", "Ready?", "Ready!", "Good game!", "One moment, please", "Another game?", "You choose",
           "Thanks!", "Nice!", "There's lag", "I have to go", "See you!")
CHAT_BLOCK_DAYS = (1, 7, 0)  # (01.07) a block's days (0: always - an address's hash: as long as its salt, 30 days at most)
ERROR = re.compile(r"\b(?:error|errors|failed|failure|crash(?:ed|es)?|signal \d+|abort(?:ed)?|exception|fatal|panic|"
                   r"desync|can't|couldn't|cannot|not found|refused|timed out)\b", re.I)
# (The lines of numbers every 10 s say "0 errors" and the like: not errors.)
QUIET = re.compile(r"^(?:audio|pacing|speed|memory[^:]*|gpu filter|radv/ps4|ra: ping): ")


def is_error(line):
    return ERROR.search(line) is not None and not QUIET.match(line)
# The console of the core a log names (src/Cores.cpp's names), else the folder the game was in.
CORES = {
    "Snes9x": "SNES", "FCEUmm": "NES", "Genesis Plus GX": "Genesis / SMS / GG", "Beetle PCE Fast": "PC Engine",
    "FinalBurn Neo": "Arcade", "Mupen64Plus-Next": "N64", "SwanStation": "PS1", "Beetle PSX": "PS1",
    "Beetle PSX HW": "PS1", "Flycast": "Dreamcast / NAOMI", "PPSSPP": "PSP", "Azahar": "3DS", "melonDS": "DS",
    "melonDS DS": "DS", "mGBA": "GBA", "Gambatte": "Game Boy", "SameBoy": "Game Boy", "Stella": "Atari 2600",
    "ProSystem": "Atari 7800", "Virtual Jaguar": "Jaguar", "YabaSanshiro": "Saturn", "Beetle Saturn": "Saturn",
    "PicoDrive": "32X / Sega CD", "Beetle WonderSwan": "WonderSwan", "Beetle NeoPop": "Neo Geo Pocket",
    "Dolphin": "GameCube / Wii",
}
GAME_LINE = re.compile(r"^game: (.+?) \((/.+)\)\s*$")
CORE_LINE = re.compile(r"^game: ([^,()]+), [0-9.]+ FPS")


def esc(text):
    return html.escape(str(text), quote=True)


def stamp(t):
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(t)) if t else "-"


def kb(n):
    return "%.0f KB" % (n / 1024.0) if n < 1024 * 1024 else "%.1f MB" % (n / 1048576.0)


def flag(cc):
    """(01.04) A country's flag (its two letters as Unicode's regional indicators: no picture) and its code; "??" and
    the like: the code alone."""
    cc = (cc or "").strip().upper()
    if len(cc) == 2 and cc.isalpha() and cc.isascii():
        return "%s%s %s" % (chr(0x1F1E6 + ord(cc[0]) - 65), chr(0x1F1E6 + ord(cc[1]) - 65), cc)
    return cc or "??"


def flags(countries):
    """A list of countries ("AR,CL") with their flags."""
    return " ".join(flag(c) for c in (countries or "").split(",") if c) or "-"


# ---- TOTP (RFC 6238: HMAC-SHA1, 6 digits, 30 s) ----
def totp(secret_b32, counter):
    key = base64.b32decode(secret_b32 + "=" * (-len(secret_b32) % 8))
    digest = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
    at = digest[-1] & 0x0F
    value = struct.unpack(">I", digest[at:at + 4])[0] & 0x7FFFFFFF
    return "%06d" % (value % 1000000)


def totp_match(secret_b32, code, now=None, last=-1):
    """The time step the code is of (now, one before or after), -1 when none - or one already used (last)."""
    now = time.time() if now is None else now
    code = re.sub(r"\s", "", code or "")
    if not re.fullmatch(r"\d{6}", code):
        return -1
    step = int(now // 30)
    for counter in (step, step - 1, step + 1):
        if counter > last and hmac.compare_digest(totp(secret_b32, counter), code):
            return counter
    return -1


def password_hash(password, salt, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P):
    return hashlib.scrypt(password.encode("utf-8"), salt=salt, n=n, r=r, p=p, maxmem=128 * 1024 * 1024, dklen=32)


# ---- A QR code in the terminal (the TOTP's: byte mode, level M, versions 1 to 6 - scripts/make-help-qr.py's) ----
QR_ECC = [None, 10, 16, 26, 18, 24, 16]
QR_BLOCKS = [None, 1, 1, 1, 2, 2, 4]


def qr_raw_modules(ver):
    result = (16 * ver + 128) * ver + 64
    if ver >= 2:
        n = ver // 7 + 2
        result -= (25 * n - 10) * n - 55
    return result


def qr_data_words(ver):
    return qr_raw_modules(ver) // 8 - QR_ECC[ver] * QR_BLOCKS[ver]


def qr_gf_mul(x, y):
    z = 0
    for i in reversed(range(8)):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z


def qr_words(text):
    data = text.encode("utf-8")
    ver = next(v for v in range(1, 7) if 4 + 8 + 8 * len(data) <= qr_data_words(v) * 8)
    bits = []

    def put(value, n):
        bits.extend((value >> i) & 1 for i in reversed(range(n)))

    put(0b0100, 4)
    put(len(data), 8)
    for b in data:
        put(b, 8)
    capacity = qr_data_words(ver) * 8
    put(0, min(4, capacity - len(bits)))
    put(0, -len(bits) % 8)
    pad = 0xEC
    while len(bits) < capacity:
        put(pad, 8)
        pad ^= 0xEC ^ 0x11
    words = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    degree = QR_ECC[ver]
    divisor = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            divisor[j] = qr_gf_mul(divisor[j], root)
            if j + 1 < degree:
                divisor[j] ^= divisor[j + 1]
        root = qr_gf_mul(root, 0x02)
    raw = qr_raw_modules(ver) // 8
    blocks_n = QR_BLOCKS[ver]
    short_n, short_len = blocks_n - raw % blocks_n, raw // blocks_n
    blocks, k = [], 0
    for i in range(blocks_n):
        dat = words[k:k + short_len - degree + (0 if i < short_n else 1)]
        k += len(dat)
        rem = [0] * degree
        for b in dat:
            factor = b ^ rem.pop(0)
            rem.append(0)
            for j, coef in enumerate(divisor):
                rem[j] ^= qr_gf_mul(coef, factor)
        if i < short_n:
            dat = dat + [0]
        blocks.append(dat + rem)
    out = []
    for i in range(len(blocks[0])):
        for j, block in enumerate(blocks):
            if i != short_len - degree or j >= short_n:
                out.append(block[i])
    return ver, out


def qr_matrix(text):
    ver, words = qr_words(text)
    size = ver * 4 + 17
    best = None
    for mask in range(8):
        dark = [[False] * size for _ in range(size)]
        func = [[False] * size for _ in range(size)]

        def setf(x, y, on):
            dark[y][x] = on
            func[y][x] = True

        for i in range(size):
            setf(6, i, i % 2 == 0)
            setf(i, 6, i % 2 == 0)
        for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < size and 0 <= y < size:
                        setf(x, y, max(abs(dx), abs(dy)) not in (2, 4))
        if ver > 1:
            n = ver // 7 + 2
            step = (ver * 8 + n * 3 + 5) // (n * 4 - 4) * 2
            pos = list(reversed([size - 7 - i * step for i in range(n - 1)] + [6]))
            for i in range(n):
                for j in range(n):
                    if (i, j) in ((0, 0), (0, n - 1), (n - 1, 0)):
                        continue
                    for dy in range(-2, 3):
                        for dx in range(-2, 3):
                            setf(pos[i] + dx, pos[j] + dy, max(abs(dx), abs(dy)) != 1)

        def fmt(m):
            data = m  # (level M: 00)
            rem = data
            for _ in range(10):
                rem = (rem << 1) ^ ((rem >> 9) * 0x537)
            bits = (data << 10 | rem) ^ 0x5412
            bit = lambda i: (bits >> i) & 1 != 0
            for i in range(6):
                setf(8, i, bit(i))
            setf(8, 7, bit(6))
            setf(8, 8, bit(7))
            setf(7, 8, bit(8))
            for i in range(9, 15):
                setf(14 - i, 8, bit(i))
            for i in range(8):
                setf(size - 1 - i, 8, bit(i))
            for i in range(8, 15):
                setf(8, size - 15 + i, bit(i))
            setf(8, size - 8, True)

        fmt(0)
        i, right = 0, size - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(size):
                for j in range(2):
                    x = right - j
                    y = size - 1 - vert if ((right + 1) & 2) == 0 else vert
                    if not func[y][x] and i < len(words) * 8:
                        dark[y][x] = (words[i >> 3] >> (7 - (i & 7))) & 1 != 0
                        i += 1
            right -= 2
        tests = [lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
                 lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
                 lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
                 lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0]
        for y in range(size):
            for x in range(size):
                if not func[y][x] and tests[mask](x, y):
                    dark[y][x] = not dark[y][x]
        fmt(mask)
        score = 0
        finder = [True, False, True, True, True, False, True]
        for line in [row for row in dark] + [[dark[y][x] for y in range(size)] for x in range(size)]:
            run, prev = 0, None
            for v in line:
                if v == prev:
                    run += 1
                else:
                    score += 3 + run - 5 if run >= 5 else 0
                    run, prev = 1, v
            score += 3 + run - 5 if run >= 5 else 0
            padded = [False] * 4 + line + [False] * 4
            for i in range(len(padded) - 6):
                if padded[i:i + 7] == finder and (padded[max(0, i - 4):i] == [False] * 4 or
                                                  padded[i + 7:i + 11] == [False] * 4):
                    score += 40
        for y in range(size - 1):
            for x in range(size - 1):
                if dark[y][x] == dark[y][x + 1] == dark[y + 1][x] == dark[y + 1][x + 1]:
                    score += 3
        total = size * size
        score += ((abs(sum(v for row in dark for v in row) * 20 - total * 10) + total - 1) // total - 1) * 10
        if best is None or score < best[0]:
            best = (score, dark)
    return best[1]


def qr_terminal(text):
    """The QR code as lines of half blocks: light modules drawn (a terminal's dark background is the dark ones)."""
    dark = qr_matrix(text)
    size, border = len(dark), 2
    light = lambda x, y: not (0 <= x < size and 0 <= y < size and dark[y][x])
    lines = []
    for y in range(-border, size + border, 2):
        row = ""
        for x in range(-border, size + border):
            top, bottom = light(x, y), light(x, y + 1)
            row += "█" if top and bottom else "▀" if top else "▄" if bottom else " "
        lines.append(row)
    return lines


# ---- The owner's password and TOTP ----
def admin_path(state_dir):
    return os.path.join(state_dir, "admin.json")


def admin_setup(state_dir, say=print, ask=input, secret_ask=getpass.getpass):
    """sudo emusuni-reports admin-setup: the panel's password (asked twice, unseen) and a new TOTP (its QR shown, a code
    of it typed to check)."""
    os.makedirs(state_dir, mode=0o700, exist_ok=True)
    path = admin_path(state_dir)
    if os.path.exists(path):
        if ask("El panel ya tiene contraseña. ¿Cambiar la contraseña y el código TOTP? [s/N] ").strip().lower() not in (
                "s", "si", "sí", "y", "yes"):
            say("Nada cambió.")
            return 1
    while True:
        password = secret_ask("Contraseña nueva del panel (12 caracteres o más; no se ve al escribir): ")
        if len(password) < 12:
            say("Muy corta: 12 caracteres o más.")
            continue
        if secret_ask("Otra vez: ") != password:
            say("No son iguales.")
            continue
        break
    secret = base64.b32encode(secrets.token_bytes(20)).decode("ascii").rstrip("=")
    uri = "otpauth://totp/EmuSuNi?secret=%s&issuer=EmuSuNi" % secret
    say("")
    say("Escanea este código con Google Authenticator (o la app de códigos que uses):")
    say("")
    for line in qr_terminal(uri):
        say("  " + line)
    say("")
    say("O escribe la clave a mano: %s" % " ".join(secret[i:i + 4] for i in range(0, len(secret), 4)))
    say("")
    for _ in range(3):
        if totp_match(secret, ask("El código de 6 cifras que muestra la app ahora: ")) >= 0:
            break
        say("Ese código no coincide (¿la hora del celular está bien?).")
    else:
        say("Nada cambió: vuelve a intentarlo.")
        return 1
    salt = secrets.token_bytes(16)
    record = {"v": 1, "salt": base64.b64encode(salt).decode(), "n": SCRYPT_N, "r": SCRYPT_R, "p": SCRYPT_P,
              "hash": base64.b64encode(password_hash(password, salt)).decode(), "totp": secret,
              "created": int(time.time())}
    old = os.umask(0o077)
    try:
        with open(path + ".part", "w", encoding="utf-8") as f:
            json.dump(record, f)
        os.replace(path + ".part", path)
    finally:
        os.umask(old)
    say("Listo: el panel pide esa contraseña y el código de la app. Las sesiones abiertas se cerraron.")
    return 0


# ---- Wrong tries, by address (a keyed hash in memory, never the address) and in all ----
class Throttle:
    def __init__(self):
        self.lock = threading.Lock()
        self.day, self.key = None, secrets.token_bytes(32)
        self.tries = {}  # who -> [times of wrong tries]
        self.locked = {}  # who -> until
        self.all = []
        self.all_locked = 0.0

    def who(self, address):
        day = time.strftime("%Y-%m-%d", time.gmtime())
        if day != self.day:
            self.day, self.key = day, secrets.token_bytes(32)
            self.tries, self.locked = {}, {}
        return hmac.new(self.key, address.encode(), hashlib.sha256).digest()[:16]

    def wait(self, address, now=None):
        """Seconds this address must wait before it may try (0: it may)."""
        now = time.time() if now is None else now
        with self.lock:
            who = self.who(address)
            return max(0.0, self.locked.get(who, 0.0) - now, self.all_locked - now)

    def failed(self, address, now=None):
        """A wrong try counted; True when it locked the address (or every login)."""
        now = time.time() if now is None else now
        with self.lock:
            who = self.who(address)
            recent = [t for t in self.tries.get(who, []) if now - t < TRIES_SECONDS] + [now]
            self.tries[who] = recent
            self.all = [t for t in self.all if now - t < ALL_TRIES_SECONDS] + [now]
            locked = False
            if len(recent) >= TRIES:
                self.locked[who] = now + LOCK_SECONDS
                self.tries[who] = []
                locked = True
            if len(self.all) >= ALL_TRIES:
                self.all_locked = now + LOCK_SECONDS
                self.all = []
                locked = True
            return locked

    def passed(self, address):
        with self.lock:
            self.tries.pop(self.who(address), None)


# ---- HyperLogLog, as emusuni-netd keeps them (server/emusuni-netd-stats.h: Hll) ----
class Hll:
    def __init__(self, p):
        self.reg = bytearray(1 << p)

    def merge_text(self, s):
        if not s:
            return
        n = len(self.reg)
        if s[0] == "d" and len(s) == n + 1:
            for i in range(n):
                v = min(64, max(0, ord(s[i + 1]) - 48))
                if v > self.reg[i]:
                    self.reg[i] = v
        elif s[0] == "s" and (len(s) - 1) % 5 == 0:
            for at in range(1, len(s), 5):
                i = int(s[at:at + 4], 16)
                v = min(64, max(0, ord(s[at + 4]) - 48))
                if i < n and v > self.reg[i]:
                    self.reg[i] = v

    def estimate(self):
        m = float(len(self.reg))
        total = sum(2.0 ** -r for r in self.reg)
        zeros = self.reg.count(0)
        e = (0.7213 / (1.0 + 1.079 / m)) * m * m / total
        if e <= 2.5 * m and zeros > 0:
            e = m * math.log(m / zeros)
        return int(math.floor(e + 0.5))


class Stats:
    """emusuni-netd's stats.db, read only (the same numbers as its report --json, with no minimum)."""

    def __init__(self, stats_dir, titles_path):
        self.path = os.path.join(stats_dir, "stats.db")
        self.titles_path = titles_path
        self.titles, self.titles_mtime = {}, None

    def load_titles(self):
        try:
            mtime = os.stat(self.titles_path).st_mtime
        except OSError:
            return
        if mtime == self.titles_mtime:
            return
        titles = {}
        try:
            with open(self.titles_path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    label, _, title = line.rstrip("\n").partition("\t")
                    if label and title:
                        titles[label] = title
        except OSError:
            return
        self.titles, self.titles_mtime = titles, mtime

    def report(self, days, now=None):
        now = time.time() if now is None else now
        if not os.path.exists(self.path):
            return None
        db = sqlite3.connect("file:%s?mode=ro" % urllib.parse.quote(self.path), uri=True, timeout=5)
        try:
            return self.read(db, days, now)
        finally:
            db.close()

    def read(self, db, days, now):
        days = max(1, min(days, 3660))
        span = max(days, 30)
        dates = [time.strftime("%Y-%m-%d", time.gmtime(now - i * 86400)) for i in range(span)]
        rows = {r[0]: r for r in db.execute(
            "SELECT date, active, fresh, heartbeats, update_checks, uncounted, peak, sketch FROM day WHERE date >= ?",
            (dates[-1],))}

        def unique(count):
            h = Hll(SKETCH_ALL)
            for d in dates[:count]:
                if d in rows:
                    h.merge_text(rows[d][7])
            return h.estimate()

        out = {"days": days, "generated": now}
        out["active"] = {"today": rows[dates[0]][1] if dates[0] in rows else 0,
                         "yesterday": rows[dates[1]][1] if dates[1] in rows else 0,
                         "7": unique(7), "30": unique(30),
                         "period": (rows[dates[0]][1] if dates[0] in rows else 0) if days == 1 else unique(days)}
        period = dates[:days]
        out["new"] = sum(rows[d][2] for d in period if d in rows)
        out["heartbeats"] = sum(rows[d][3] for d in period if d in rows)
        out["checks"] = sum(rows[d][4] for d in period if d in rows)
        out["uncounted"] = sum(rows[d][5] for d in period if d in rows)
        out["daily"] = [{"date": d, "active": rows[d][1] if d in rows else 0, "new": rows[d][2] if d in rows else 0,
                         "peak": rows[d][6] if d in rows else 0} for d in reversed(period)]
        labels = {}
        for kind, value, installs, beats, sketch in db.execute(
                "SELECT kind, value, installs, beats, sketch FROM day_label WHERE date >= ? AND date <= ?",
                (period[-1], period[0])):
            if kind not in labels:
                labels[kind] = {}
            if value not in labels[kind]:
                labels[kind][value] = [Hll(SKETCH_GAME if kind == "game" else SKETCH_LABEL), 0, 0]
            entry = labels[kind][value]
            entry[0].merge_text(sketch)
            entry[1] += beats
            entry[2] += installs
        self.load_titles()
        out["by"] = {}
        for kind in LABEL_KINDS:
            values = []
            for value, (sketch, beats, exact) in labels.get(kind, {}).items():
                values.append({"value": value, "installs": exact if days == 1 else sketch.estimate(),
                               "hours": beats * 5.0 / 60.0, "title": self.titles.get(value) if kind == "game" else None})
            values.sort(key=lambda v: (-v["installs"], -v["hours"], v["value"]))
            out["by"][kind] = values
        out["updates"] = [{"from": f, "to": t, "installs": n} for f, t, n in db.execute(
            "SELECT from_version, to_version, SUM(installs) FROM day_update WHERE date >= ? AND date <= ? "
            "GROUP BY from_version, to_version ORDER BY 3 DESC", (period[-1], period[0]))]
        live = {"time": 0, "online": 0, "by": {}}
        row = db.execute("SELECT value FROM meta WHERE key = 'live'").fetchone()
        for line in (row[0] if row else "").splitlines():
            words = line.split(" ")
            if words[0] == "time" and len(words) == 2 and words[1].isdigit():
                live["time"] = int(words[1])
            elif words[0] == "online" and len(words) == 2 and words[1].isdigit():
                live["online"] = int(words[1])
            elif words[0] == "by" and len(words) == 4 and words[3].isdigit():
                live["by"].setdefault(words[1], []).append((words[2], int(words[3])))
        for values in live["by"].values():
            values.sort(key=lambda v: -v[1])
        live["current"] = live["time"] > 0 and now - live["time"] <= 180
        out["live"] = live
        downloads = {}
        for date, tag, total in db.execute("SELECT date, tag, total FROM downloads ORDER BY date"):
            downloads.setdefault(date, {})[tag] = total
        out["downloads"] = downloads
        # (00.95) The versions published on GitHub (record-downloads): the public page counts only those; here every
        # version, the test ones marked.
        row = db.execute("SELECT value FROM meta WHERE key = 'published'").fetchone()
        out["published"] = set((row[0] if row and row[0] else "").split())
        return out


def room_title(titles, label):
    """(01.04) A room's game title from titles.txt: its label ("snes.c-7655bb55"), a link cable room's under the Game
    Boys' own consoles ("gblink.c-..." as gb., gbc.; "gbalink.c-..." as gba.); None when it isn't known."""
    if not label:
        return None
    console, _, rest = label.partition(".")
    tries = {"gblink": ("gb", "gbc"), "gbalink": ("gba",)}.get(console, (console,))
    for c in tries:
        if "%s.%s" % (c, rest) in titles:
            return titles["%s.%s" % (c, rest)]
    return None


ROOM_ORDERS = {"created": "created", "played": "(matches > 0 AND match_seconds >= 30)", "match": "match_seconds",
               "peak": "peak", "joins": "joins", "lasted": "(closed - created)"}
GAME_ORDERS = {"minutes": "6", "matches": "5", "rooms": "4", "game": "2"}


def rooms_report(stats, days, now=None, latest=100, page_of=None, orders=None):
    """(01.04) emusuni-netd's room_log (stats.db, read only): the period's totals, each day's, the latest rooms and the
    games most played online - in SQL (the micro VM: aggregates, and the lists a page at a time with LIMIT/OFFSET on the
    index of created). page_of(name, total) -> (offset, limit): the page of a list ("r_": the rooms, "g_": the games;
    none: the first `latest` rooms, the first 50 games). None: no stats.db, or one of before 01.04 (no room_log)."""
    now = time.time() if now is None else now
    if not os.path.exists(stats.path):
        return None
    days = max(1, min(days, 3660))
    dates = [time.strftime("%Y-%m-%d", time.gmtime(now - i * 86400)) for i in range(days)]
    since = calendar.timegm(time.strptime(dates[-1], "%Y-%m-%d"))  # (the period's first day, from 00:00 UTC)
    page_of = page_of or (lambda name, total: (0, latest if name == "r_" else 50))
    orders = orders or {}
    r_key, r_desc = orders.get("r_", ("created", True))
    g_key, g_desc = orders.get("g_", ("minutes", True))
    r_order = "%s %s, created DESC" % (ROOM_ORDERS.get(r_key, "created"), "DESC" if r_desc else "ASC")
    g_order = "%s %s, 6 DESC, console" % (GAME_ORDERS.get(g_key, "6"), "DESC" if g_desc else "ASC")
    db = sqlite3.connect("file:%s?mode=ro" % urllib.parse.quote(stats.path), uri=True, timeout=5)
    try:
        columns = {row[1] for row in db.execute("PRAGMA table_info(room_log)")}
        if not columns:
            return None
        # (A room_log of before the columns more: theirs read as 0, or -1 - not told - for the games in common.)
        col = lambda name, missing="0": "COALESCE(%s, %s)" % (name, missing) if name in columns else missing
        played = "(matches > 0 AND match_seconds >= 30)"
        refused = " + ".join(col(c) for c in ("refused_full", "refused_version", "refused_started", "refused_kicked"))
        common = col("common", "-1")
        total = db.execute(
            "SELECT COUNT(*), COALESCE(SUM(matches > 0), 0), COALESCE(SUM(%s), 0), COALESCE(SUM(match_seconds), 0),"
            " COALESCE(SUM(joins), 0), COALESCE(SUM(%s), 0), COALESCE(SUM(%s), 0),"
            " COALESCE(SUM(kind = 'together' AND %s >= 0), 0), COALESCE(SUM(kind = 'together' AND %s = 0), 0)"
            " FROM room_log WHERE created >= ?" % (played, col("guests_left"), refused, common, common),
            (since,)).fetchone()
        daily = {d: {"date": d, "rooms": 0, "matched": 0, "played": 0, "minutes": 0.0, "public": 0, "private": 0,
                     "together": 0, "link": 0} for d in dates}
        for row in db.execute(
                "SELECT date(created, 'unixepoch'), COUNT(*), SUM(matches > 0), SUM(%s), COALESCE(SUM(match_seconds), 0),"
                " SUM(kind = 'public'), SUM(kind = 'private'), SUM(kind = 'together'), SUM(kind = 'link')"
                " FROM room_log WHERE created >= ? GROUP BY 1" % played, (since,)):
            if row[0] in daily:
                daily[row[0]].update(rooms=row[1], matched=row[2], played=row[3], minutes=row[4] / 60.0, public=row[5],
                                     private=row[6], together=row[7], link=row[8])
        offset, limit = page_of("r_", total[0])
        cursor = db.execute("SELECT * FROM room_log WHERE created >= ? ORDER BY %s LIMIT ? OFFSET ?" % r_order,
                            (since, limit, offset))
        names = [d[0] for d in cursor.description]
        rows = cursor.fetchall()
        games_total = db.execute("SELECT COUNT(*) FROM (SELECT 1 FROM room_log WHERE created >= ? AND matches > 0"
                                 " GROUP BY console, game)", (since,)).fetchone()[0]
        g_offset, g_limit = page_of("g_", games_total)
        games = db.execute("SELECT console, game, MAX(%s), COUNT(*), SUM(matches), COALESCE(SUM(match_seconds), 0)"
                           " FROM room_log WHERE created >= ? AND matches > 0 GROUP BY console, game"
                           " ORDER BY %s LIMIT ? OFFSET ?" % (col("title", "''"), g_order),
                           (since, g_limit, g_offset)).fetchall()
        try:
            refusals = db.execute("SELECT request, why, SUM(n) FROM refusal_day WHERE date >= ? AND date <= ? "
                                  "GROUP BY request, why ORDER BY 3 DESC", (dates[-1], dates[0])).fetchall()
        except sqlite3.OperationalError:
            refusals = []
    except sqlite3.OperationalError:
        return None
    finally:
        db.close()
    stats.load_titles()
    rooms = []
    for row in rows:
        r = dict(zip(names, row))
        for name in ("guests_left", "dropped", "refused_full", "refused_version", "refused_started", "refused_kicked",
                     "host_refused", "host_no_answer", "matches", "match_seconds", "joins", "peak", "relayed_kb",
                     "watchers", "late_joins"):
            r[name] = r.get(name) or 0
        r["common"] = r.get("common") if r.get("common") is not None else -1
        r["refused"] = r["refused_full"] + r["refused_version"] + r["refused_started"] + r["refused_kicked"]
        # (01.04) Played: a match the host started that went 30 s or more (a shorter one: maybe they didn't connect).
        r["played"] = bool(r["matches"]) and r["match_seconds"] >= 30
        r["title"] = room_title(stats.titles, r["game"]) or (r.get("title") or None)
        rooms.append(r)
    today = time.strftime("%Y-%m-%d", time.gmtime(now))
    return {"latest": rooms, "count": total[0], "matched": total[1], "played": total[2], "today": daily.get(today),
            "minutes": total[3] / 60.0, "joins": total[4], "left": total[5], "refused": total[6], "refusals": refusals,
            "together_known": total[7], "together_none": total[8], "daily": [daily[d] for d in reversed(dates)],
            "games_total": games_total,
            "games": [{"console": c, "game": g, "title": room_title(stats.titles, g) or (t or None), "rooms": n,
                       "matches": m, "minutes": s / 60.0} for c, g, t, n, m, s in games]}


class VisitsRead:
    """(01.04) emusuni.com's visits (emusuni-reports.py's Visits: <dir>/visits/visits.db), read only: by day, page and
    country, and each day's unique visitors (counted that day: over several days they are a sum of days)."""

    def __init__(self, path):
        self.path = path

    def report(self, days, now=None):
        now = time.time() if now is None else now
        if not os.path.exists(self.path):
            return None
        days = max(1, min(days, 3660))
        dates = [time.strftime("%Y-%m-%d", time.gmtime(now - i * 86400)) for i in range(days)]
        first, last = dates[-1], dates[0]
        db = sqlite3.connect("file:%s?mode=ro" % urllib.parse.quote(self.path), uri=True, timeout=5)
        try:
            views = {d: 0 for d in dates}
            visitors = {d: 0 for d in dates}
            pages, countries = {}, {}
            for date, page, country, n in db.execute(
                    "SELECT date, page, country, views FROM visit_day WHERE date >= ? AND date <= ?", (first, last)):
                views[date] = views.get(date, 0) + n
                pages[page] = pages.get(page, 0) + n
                countries.setdefault(country, [0, 0])[0] += n
            for date, country, n in db.execute(
                    "SELECT date, country, visitors FROM visit_unique WHERE date >= ? AND date <= ?", (first, last)):
                visitors[date] = visitors.get(date, 0) + n
                countries.setdefault(country, [0, 0])[1] += n
            total = db.execute("SELECT COALESCE(SUM(views), 0) FROM visit_day").fetchone()[0]
            since = db.execute("SELECT MIN(date) FROM visit_day").fetchone()[0]
        finally:
            db.close()
        return {"views": sum(views[d] for d in dates), "visitors": sum(visitors[d] for d in dates), "total": total,
                "since": since, "daily": [{"date": d, "active": views[d], "peak": visitors[d]} for d in reversed(dates)],
                "pages": sorted(pages.items(), key=lambda kv: (-kv[1], kv[0])),
                "countries": sorted(((c, v[0], v[1]) for c, v in countries.items()), key=lambda r: (-r[1], r[0]))}


class GitHub:
    """Each release's downloads, from GitHub's public API (no token), kept an hour (and on disk across restarts)."""

    def __init__(self, repo, state_dir, say):
        self.repo, self.path, self.say = repo, os.path.join(state_dir, "github.json"), say
        self.lock = threading.Lock()
        self.data = None
        try:
            with open(self.path, encoding="utf-8") as f:
                self.data = json.load(f)
        except (OSError, ValueError):
            pass

    def releases(self):
        with self.lock:
            if self.data and time.time() - self.data.get("time", 0) < GITHUB_SECONDS:
                return self.data
            try:
                url = "https://api.github.com/repos/%s/releases?per_page=100" % self.repo
                req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                           "User-Agent": "emusuni-reports-panel"})
                with urllib.request.urlopen(req, timeout=10) as r:
                    raw = json.loads(r.read().decode("utf-8"))
                releases = []
                for rel in raw:
                    assets = rel.get("assets", [])
                    releases.append({"tag": rel.get("tag_name", "?"), "date": (rel.get("published_at") or "")[:10],
                                     "pkg": sum(a.get("download_count", 0) for a in assets
                                                if a.get("name", "").endswith(".pkg")),
                                     "all": sum(a.get("download_count", 0) for a in assets)})
                self.data = {"time": int(time.time()), "releases": releases}
                with open(self.path + ".part", "w", encoding="utf-8") as f:
                    json.dump(self.data, f)
                os.replace(self.path + ".part", self.path)
            except (OSError, ValueError) as e:
                self.say("panel: GitHub's releases not read now (%s)" % type(e).__name__)
                if self.data:
                    self.data["stale"] = True
            return self.data


# ---- The panel ----
CSS = """
:root{--bg:#0f1117;--card:#181b24;--line:#2a2f3d;--text:#e8eaf0;--dim:#9aa0b0;--accent:#b49cff;--good:#5bd38a;
--warn:#ffc870;--bad:#ff8a80;--mark:#5a4a00;--zebra:rgba(255,255,255,.035);--hover:rgba(180,156,255,.09)}
@media (prefers-color-scheme:light){:root{--bg:#f4f5f8;--card:#fff;--line:#d9dce4;--text:#1c1f26;--dim:#5d6372;
--accent:#5b3fd1;--good:#1e7f46;--warn:#9a5c00;--bad:#b3261e;--mark:#ffe58a;--zebra:rgba(0,0,0,.028);
--hover:rgba(91,63,209,.07)}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
a{color:var(--accent)}
header{display:flex;flex-wrap:wrap;gap:6px 18px;align-items:center;padding:12px 16px;border-bottom:1px solid var(--line);
background:var(--card)}
header .brand{font-weight:700;margin-right:auto}
header a{text-decoration:none}
header form{margin:0}
main{max-width:1180px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 14px}
h2{font-size:17px;margin:22px 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin:0 0 14px}
.muted{color:var(--dim)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.num{font-size:26px;font-weight:700}
input,select,textarea,button{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);
border-radius:8px;padding:8px 10px;max-width:100%}
textarea{width:100%;min-height:90px}
button,.button{background:var(--accent);color:#fff;border:0;cursor:pointer;text-decoration:none;display:inline-block;
padding:8px 14px;border-radius:8px}
button.plain{background:transparent;color:var(--accent);border:1px solid var(--line)}
button.danger{background:var(--bad)}
form.filters{display:flex;flex-wrap:wrap;gap:8px;align-items:end;margin:0 0 12px}
form.filters label{display:flex;flex-direction:column;font-size:13px;color:var(--dim)}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:8px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{font-size:12.5px;color:var(--dim);font-weight:600;text-transform:uppercase;letter-spacing:.03em;white-space:nowrap}
td.r,th.r{text-align:right}
td.r{font-variant-numeric:tabular-nums;white-space:nowrap}
.nw{white-space:nowrap}
.wide{min-width:14em}
.tablewrap{overflow:auto;max-height:75vh;-webkit-overflow-scrolling:touch;margin:0 -14px;padding:0 14px}
.tablewrap thead th{position:sticky;top:0;background:var(--card);z-index:1;box-shadow:0 1px 0 var(--line)}
tbody tr:nth-child(even) td{background:var(--zebra)}
tbody tr:hover td{background:var(--hover)}
th a.sort{color:inherit;text-decoration:none}
th a.sort:hover{color:var(--text)}
th a.sort.on{color:var(--accent)}
.badge{display:inline-block;padding:1px 9px;border-radius:999px;font-size:12.5px;font-weight:600;white-space:nowrap}
.badge.yes{background:rgba(91,211,138,.16);color:var(--good)}
.badge.no{background:rgba(154,160,176,.14);color:var(--dim)}
.badge.short{background:rgba(255,200,112,.15);color:var(--warn)}
.ago{display:block;color:var(--dim);font-size:12.5px}
.tag{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;border:1px solid var(--line)}
.tag.new{color:var(--warn);border-color:var(--warn)}
.tag.reviewed{color:var(--accent);border-color:var(--accent)}
.tag.solved{color:var(--good);border-color:var(--good)}
.tag.crash{color:var(--bad);border-color:var(--bad)}
.msg{padding:10px 12px;border-radius:8px;margin:0 0 12px;border:1px solid var(--line)}
.msg.bad{border-color:var(--bad);color:var(--bad)}
.msg.good{border-color:var(--good);color:var(--good)}
pre.log{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 0;overflow-x:auto;
font:12.5px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;margin:0}
pre.log a.n{display:inline-block;width:5.5em;padding-right:10px;text-align:right;color:var(--dim);text-decoration:none;
user-select:none}
pre.log span.l{display:block;padding:0 10px 0 0;white-space:pre}
pre.log span.e{background:rgba(255,80,70,.16)}
pre.log span.l:target{background:rgba(180,156,255,.25)}
mark{background:var(--mark);color:inherit}
.pages{display:flex;flex-wrap:wrap;gap:6px;margin:10px 0}
.pages a,.pages span{padding:4px 10px;border:1px solid var(--line);border-radius:8px;text-decoration:none}
.pages span{background:var(--accent);color:#fff;border-color:var(--accent)}
.pager{display:flex;flex-wrap:wrap;align-items:center;gap:8px 14px;margin:10px 0}
.pager .sizes,.pager .pages{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.pager a,.pager b{padding:3px 9px;border:1px solid var(--line);border-radius:8px;text-decoration:none}
.pager b{background:var(--accent);color:#fff;border-color:var(--accent)}
.pager .gap{color:var(--dim)}
details.sec{margin:0 0 14px}
details.sec>summary{cursor:pointer;font-size:1.15em;font-weight:600;margin:18px 0 10px;list-style-position:outside}
details.sec:not([open])>summary{color:var(--dim)}
.login{max-width:380px;margin:8vh auto}
.login label{display:block;margin:10px 0 4px}
.login input{width:100%}
.chart{width:100%;height:auto}
.bar{fill:var(--accent)}
.bar2{fill:var(--good)}
.axis{fill:var(--dim);font-size:17px}
footer{max-width:1180px;margin:0 auto;padding:12px 16px 30px;color:var(--dim);font-size:13px}
@media (max-width:720px){
table.cards thead{display:none}
table.cards tr{display:block;border:1px solid var(--line);border-radius:10px;margin:0 0 10px;padding:6px 8px;
background:var(--card)}
table.cards td{display:flex;justify-content:space-between;gap:10px;border:0;padding:4px 2px;text-align:right}
table.cards td::before{content:attr(data-label);color:var(--dim);text-align:left}
table.cards tbody tr:nth-child(even) td,table.cards tbody tr:hover td{background:transparent}
.tablewrap{max-height:none}
table:not(.cards){min-width:100%;width:max-content}
header{padding:10px 12px}main{padding:12px}
pre.log a.n{width:4em}
}
"""


class Panel:
    def __init__(self, store, data_dir, public_host, stats_dir, titles, repo, cert_status, say, canonical=None):
        # (00.95) public_host: its addresses, "panel.emusuni.com,132.145.164.206" (or one); canonical: the one to send
        # the browser to now (its name once the name's certificate is valid; None: any). cert_status(host): that
        # address's certificate.
        self.hosts = [h.strip().lower() for h in public_host.split(",") if h.strip()]
        self.public_host = self.hosts[0] if self.hosts else ""
        self.store, self.say, self.cert_status = store, say, cert_status
        self.canonical = canonical or (lambda: None)
        self.state_dir = os.path.join(data_dir, "panel")
        os.makedirs(self.state_dir, mode=0o700, exist_ok=True)
        self.lock = threading.Lock()
        self.sessions = {}
        self.admin, self.admin_mtime = None, None
        self.last_counter = -1
        self.throttle = Throttle()
        self.stats = Stats(stats_dir, titles)
        self.visits = VisitsRead(os.path.join(data_dir, "visits", "visits.db"))  # (01.04)
        self.github = GitHub(repo, self.state_dir, say)
        # (01.07) The world chat's blocks: a file emusuni-netd reads (--chat-blocks), outside the panel's own folder
        # (its user's only): codes and addresses' hashes - nothing an address can be found from without netd's salt.
        self.chat_blocks = os.environ.get("EMUSUNI_CHAT_BLOCKS") or os.path.join(data_dir, "chat", "blocks.tsv")
        self.form_key = secrets.token_bytes(32)
        self.hashing = threading.BoundedSemaphore(2)  # (scrypt takes 32 MB: two at once, the service has 256)
        self.css = CSS.encode("utf-8")
        self.css_tag = hashlib.sha256(self.css).hexdigest()[:12]
        self.local = threading.local()  # (each request's own: a thread each - its language, CSRF token, page)

    @property
    def lang(self):
        return getattr(self.local, "lang", "es")

    @lang.setter
    def lang(self, value):
        self.local.lang = value

    @property
    def csrf(self):
        return getattr(self.local, "csrf", "")

    @csrf.setter
    def csrf(self, value):
        self.local.csrf = value

    @property
    def here(self):
        return getattr(self.local, "here", "/")

    @here.setter
    def here(self, value):
        self.local.here = value

    # The owner's record (admin.json), read again when it changes - every session then ends.
    def load_admin(self):
        path = admin_path(self.state_dir)
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            self.admin, self.admin_mtime = None, None
            return None
        if mtime != self.admin_mtime:
            try:
                with open(path, encoding="utf-8") as f:
                    admin = json.load(f)
            except (OSError, ValueError):
                return None
            with self.lock:
                if self.admin_mtime is not None:
                    self.sessions.clear()
                self.admin, self.admin_mtime, self.last_counter = admin, mtime, -1
        return self.admin

    # ---- The request ----
    def handle(self, h, method):
        """A request to the panel (anything but the app's POST /report)."""
        url = urllib.parse.urlsplit(h.path)
        path = url.path
        query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
        q = lambda name, default="": (query.get(name) or [default])[0]
        cookies = {}
        for part in h.headers.get("Cookie", "").split(";"):
            name, _, value = part.strip().partition("=")
            if name:
                cookies[name] = value
        self.lang = "en" if cookies.get("lang") == "en" else "es"
        self.csrf, self.here = "", path if path.startswith("/") else "/"
        # (00.95) The address asked for (the Host): one of ours, else the first. Once the name has its certificate, a
        # page asked at the IP goes to the name (a change there is refused: its form came from the IP's page).
        asked = (h.headers.get("Host") or "").strip().lower()
        if asked.endswith(":443"):
            asked = asked[:-4]
        self.local.host = asked if asked in self.hosts else self.public_host
        canonical = self.canonical()
        if canonical and self.local.host != canonical:
            if method != "GET":
                # (Its body read first: a connection closed with it unread is cut before the answer.)
                try:
                    length = int(h.headers.get("Content-Length", "0"))
                except ValueError:
                    length = 0
                if 0 < length <= MAX_FORM:
                    h.rfile.read(length)
            if method == "GET":
                safe = path if re.fullmatch(r"/[A-Za-z0-9/_.-]*", path) else "/"
                query = url.query if re.fullmatch(r"[A-Za-z0-9=&%_.+-]*", url.query) else ""
                return self.send(h, 301, b"", headers={"Location": "https://%s%s%s" % (
                    canonical, safe, "?" + query if query else "")})
            return self.send(h, 403, self.page(self.t("Usa la dirección nueva", "Use the new address"),
                                               '<p><a href="https://%s/login">https://%s/</a></p>' % (
                                                   esc(canonical), esc(canonical)), nav=False))
        form = {}
        if method == "POST":
            try:
                length = int(h.headers.get("Content-Length", "-1"))
            except ValueError:
                length = -1
            if length < 0 or length > MAX_FORM:
                return self.send(h, 413, self.page(self.t("Demasiado grande", "Too big"), ""))
            raw = h.rfile.read(length)
            if len(raw) != length:
                return None
            form = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode("utf-8", "replace"),
                                                                keep_blank_values=True).items()}
            # (Another site's form: its Origin isn't ours. "null": a page of ours loaded with "no-referrer" - the
            # browser's Sec-Fetch-Site must then say it's ours.)
            origin = h.headers.get("Origin")
            fetch = h.headers.get("Sec-Fetch-Site")
            if origin == "null" and fetch == "same-origin":
                origin = None
            if (origin and origin != "https://" + self.local.host) or (fetch and fetch not in ("same-origin", "none")):
                return self.send(h, 403, self.page(self.t("No permitido", "Not allowed"),
                                                   '<p><a href="/login">%s</a></p>' %
                                                   self.t("Volver a entrar", "Log in again"), nav=False))
        if path == "/static/panel.css":
            return self.send(h, 200, self.css, "text/css; charset=utf-8", cache=True)
        if path == "/favicon.ico":  # (asked by the browser itself: none, not the login)
            return self.send(h, 404, b"", "text/plain; charset=utf-8", cache=True)
        if path == "/lang":
            to = "en" if q("to") == "en" else "es"
            back = q("back", "/")
            if not back.startswith("/") or back.startswith("//") or "\\" in back:
                back = "/"
            return self.send(h, 303, b"", headers={"Location": back},
                             cookies=["lang=%s; Path=/; Secure; SameSite=Lax; Max-Age=31536000" % to])
        if path == "/login":
            return self.login(h, method, form)
        session = self.session(cookies.get("__Host-emusuni"))
        if not session:
            if method == "GET":
                return self.send(h, 303, b"", headers={"Location": "/login"})
            return self.send(h, 403, self.page(self.t("La sesión terminó", "The session ended"),
                                               '<p><a href="/login">%s</a></p>' % self.t("Entrar", "Log in")))
        if method == "POST" and not hmac.compare_digest(form.get("csrf", ""), session["csrf"]):
            return self.send(h, 403, self.page(self.t("No permitido", "Not allowed"),
                                               "<p>%s</p>" % self.t("Formulario vencido: vuelve a cargar la página.",
                                                                    "An old form: load the page again.")))
        self.csrf = session["csrf"]
        if path == "/logout" and method == "POST":
            with self.lock:
                self.sessions.pop(session["id"], None)
            self.say("panel: logged out")
            return self.send(h, 303, b"", headers={"Location": "/login"},
                             cookies=["__Host-emusuni=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0"])
        if path == "/":
            return self.send(h, 303, b"", headers={"Location": "/reports"})
        if path == "/reports":
            return self.reports(h, q)
        if path == "/stats":
            return self.stats_page(h, q)
        if path == "/visits":
            return self.visits_page(h, q)
        if path == "/rooms":
            return self.rooms_page(h, q)
        if path == "/chat" and method == "GET":  # (01.07)
            return self.chat_page(h, q)
        if path in ("/chat/block", "/chat/unblock") and method == "POST":
            return self.chat_block(h, form, path == "/chat/block")
        m = re.fullmatch(r"/r/([%s]{6})(?:/(f|raw)/([a-z0-9.-]+)|/(all\.tar\.gz|delete))?" % ALPHABET, path)
        if m and m.group(1) in self.store.codes():
            code = m.group(1)
            if m.group(2) == "f" and m.group(3) in NAMES and method == "GET":
                return self.viewer(h, code, m.group(3), q)
            if m.group(2) == "raw" and m.group(3) in NAMES and method == "GET":
                return self.download(h, code, m.group(3))
            if m.group(4) == "all.tar.gz" and method == "GET":
                return self.download_all(h, code)
            if m.group(4) == "delete" and method == "POST":
                return self.delete(h, code, form)
            if not m.group(2) and not m.group(4):
                return self.report(h, code, method, form, q)
        return self.send(h, 404, self.page(self.t("No está", "Not here"), ""))

    def t(self, es, en):
        return en if self.lang == "en" else es

    # ---- (01.04, the owner: "cuando son muchos datos las tablas son muy largas") Pages of a table, its section ----
    def pager(self, h, q, total, name=""):
        """A long table a page at a time: (offset, limit, its links). The page's address keeps every other parameter
        (the filters, the period, the other tables' pages); name keeps several tables of one page apart ("r_": its
        r_p, r_n). PAGE_SIZES rows a page, the first by default; no links while it all fits."""
        try:
            size = int(q(name + "n", str(PAGE_SIZES[0])))
        except ValueError:
            size = PAGE_SIZES[0]
        size = size if size in PAGE_SIZES else PAGE_SIZES[0]
        pages = max(1, (total + size - 1) // size)
        try:
            page = int(q(name + "p", "1"))
        except ValueError:
            page = 1
        page = max(1, min(page, pages))
        offset = (page - 1) * size
        if total <= PAGE_SIZES[0]:
            return offset, size, ""
        url = urllib.parse.urlsplit(h.path)
        args = [(k, v) for k, v in urllib.parse.parse_qsl(url.query, keep_blank_values=True)
                if k not in (name + "p", name + "n")]
        anchor = "t-" + (name.rstrip("_") or "list")

        def link(p, n=size):
            more = [(name + "p", str(p))] + ([(name + "n", str(n))] if n != PAGE_SIZES[0] else [])
            return esc("%s?%s#%s" % (url.path, urllib.parse.urlencode(args + more), anchor))

        out = '<div class="pager" id="%s"><span class="muted">%s</span><span class="sizes">' % (
            anchor, self.t("Mostrando %d-%d de %d", "Showing %d-%d of %d") % (offset + 1, min(total, offset + size), total))
        out += " ".join("<b>%d</b>" % n if n == size else '<a href="%s">%d</a>' % (link(1, n), n) for n in PAGE_SIZES)
        out += '</span><span class="pages">'
        out += '<a href="%s">&laquo; %s</a>' % (link(page - 1), self.t("Anterior", "Previous")) if page > 1 else ""
        shown, last = sorted({1, pages} | set(range(max(1, page - 2), min(pages, page + 2) + 1))), 0
        for p in shown:
            if p - last > 1:
                out += '<span class="gap">&hellip;</span>'
            out += "<b>%d</b>" % p if p == page else '<a href="%s">%d</a>' % (link(p), p)
            last = p
        out += '<a href="%s">%s &raquo;</a>' % (link(page + 1), self.t("Siguiente", "Next")) if page < pages else ""
        return offset, size, out + "</span></div>"

    def section(self, title, inner, q, name="", open_default=False):
        """A table in a section that opens and closes (no script: <details>) - open by default, or once it is paged."""
        is_open = open_default or bool(name and (q(name + "p") or q(name + "n")))
        return '<details class="sec"%s><summary>%s</summary>%s</details>' % (" open" if is_open else "", title, inner)

    def ago(self, t, now=None):
        """(01.04) How long ago, in words ("hace 5 min")."""
        if not t:
            return ""
        seconds = max(0, int((time.time() if now is None else now) - t))
        if seconds < 60:
            return self.t("hace un momento", "just now")
        if seconds < 3600:
            return self.t("hace %d min", "%d min ago") % (seconds // 60)
        if seconds < 86400:
            return self.t("hace %d h", "%d h ago") % (seconds // 3600)
        days = seconds // 86400
        return self.t("hace %d día", "%d day ago") % days if days == 1 else self.t("hace %d días", "%d days ago") % days

    def when(self, t):
        """A time as UTC and how long ago, for a table's cell."""
        return '<span class="nw">%s</span><span class="ago">%s</span>' % (esc(stamp(t)), esc(self.ago(t))) if t else "-"

    def sorting(self, q, name, keys, default, default_desc=True):
        """(01.04) A table's order from the page's address: (key, descending) - name's s (the key, one of keys) and d
        ("a": up, "d": down)."""
        key = q(name + "s", default)
        key = key if key in keys else default
        direction = q(name + "d", "")
        desc = default_desc if direction not in ("a", "d") or key != q(name + "s", default) else direction == "d"
        return key, desc

    def th(self, h, q, name, label, key, current, cls=""):
        """A sortable column's header: a link that orders by it (again: the other way), the page back to 1, every
        other parameter kept."""
        url = urllib.parse.urlsplit(h.path)
        args = [(k, v) for k, v in urllib.parse.parse_qsl(url.query, keep_blank_values=True)
                if k not in (name + "s", name + "d", name + "p")]
        on = current[0] == key
        desc = not current[1] if on else True
        href = "%s?%s#t-%s" % (url.path, urllib.parse.urlencode(args + [(name + "s", key), (name + "d", "d" if desc
                                                                                                 else "a")]),
                               name.rstrip("_") or "list")
        mark = (" &#9660;" if current[1] else " &#9650;") if on else ""
        return '<th%s><a class="sort%s" href="%s">%s%s</a></th>' % (' class="%s"' % cls if cls else "",
                                                                   " on" if on else "", esc(href), esc(label), mark)

    def summary(self, cards):
        """The page's key totals on top: (label, value) cards."""
        out = '<div class="grid summary">'
        for label, value in cards:
            out += '<div class="card"><div class="muted">%s</div><div class="num">%s</div></div>' % (esc(label), esc(value))
        return out + "</div>"

    # ---- The answer ----
    def send(self, h, status, body, content_type="text/html; charset=utf-8", headers=None, cookies=(), cache=False):
        if isinstance(body, str):
            body = body.encode("utf-8")
        h.send_response(status)
        h.send_header("Content-Type", content_type)
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Content-Security-Policy", "default-src 'none'; style-src 'self'; img-src 'self' data:; "
                                                 "form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        h.send_header("X-Frame-Options", "DENY")
        h.send_header("X-Content-Type-Options", "nosniff")
        h.send_header("Referrer-Policy", "same-origin")
        h.send_header("Cross-Origin-Opener-Policy", "same-origin")
        h.send_header("Cross-Origin-Resource-Policy", "same-origin")
        h.send_header("Permissions-Policy", "camera=(), microphone=(), geolocation=(), interest-cohort=()")
        h.send_header("Strict-Transport-Security", "max-age=31536000")
        h.send_header("Cache-Control", "public, max-age=86400" if cache else "no-store")
        for name, value in (headers or {}).items():
            h.send_header(name, value)
        for cookie in cookies:
            h.send_header("Set-Cookie", cookie)
        h.send_header("Connection", "close")
        h.end_headers()
        h.wfile.write(body)
        h.close_connection = True
        return None

    def page(self, title, body, nav=True):
        links = ""
        if nav:
            links = ('<a href="/reports">%s</a><a href="/stats">%s</a><a href="/rooms">%s</a><a href="/chat">%s</a>'
                     '<a href="/visits">%s</a>'
                     '<form method="post" action="/logout"><input type="hidden" name="csrf" value="%s">'
                     '<button class="plain" type="submit">%s</button></form>' %
                     (self.t("Reportes", "Reports"), self.t("Estadísticas", "Statistics"), self.t("Salas", "Rooms"),
                      "Chat", self.t("Visitas", "Visits"), esc(self.csrf), self.t("Salir", "Log out")))
        expiry, problem = self.cert_status(getattr(self.local, "host", None))
        problems = {"none": "aún no pedido", "expired": "vencido", "for another key": "de otra clave",
                    "not valid yet": "aún no válido", "not readable": "ilegible", "no certificate in it": "vacío"}
        cert = (self.t("Certificado de Let's Encrypt hasta el %s UTC", "Let's Encrypt's certificate until %s UTC") %
                stamp(expiry)) if not problem else self.t("Certificado propio (Let's Encrypt: %s)",
                                                           "Own certificate (Let's Encrypt: %s)") % esc(
            self.t(problems.get(problem, problem), problem))
        other = "en" if self.lang == "es" else "es"
        return ('<!doctype html><html lang="%s"><head><meta charset="utf-8">'
                '<meta name="viewport" content="width=device-width,initial-scale=1">'
                '<meta name="robots" content="noindex,nofollow"><title>%s - EmuSuNi</title>'
                '<link rel="stylesheet" href="/static/panel.css?%s"></head><body>'
                '<header><span class="brand">EmuSuNi</span>%s</header><main>%s</main>'
                '<footer>%s &middot; <a href="/lang?to=%s&amp;back=%s">%s</a></footer></body></html>' %
                (self.lang, esc(title), self.css_tag, links,
                 body.replace("<table", '<div class="tablewrap"><table').replace("</table>", "</table></div>"), cert, other,
                 esc(urllib.parse.quote(self.here)), "English" if other == "en" else "Español"))

    # ---- The door ----
    def session(self, sid):
        if not sid:
            return None
        key = hashlib.sha256(sid.encode()).hexdigest()
        now = time.time()
        self.load_admin()
        with self.lock:
            s = self.sessions.get(key)
            if not s:
                return None
            if now - s["created"] > SESSION_SECONDS or now - s["seen"] > IDLE_SECONDS:
                del self.sessions[key]
                return None
            s["seen"] = now
            return s

    def login(self, h, method, form):
        self.csrf, self.here = "", "/login"
        address = h.client_address[0]
        admin = self.load_admin()
        message = ""
        cookie = ""
        for part in h.headers.get("Cookie", "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == "__Host-emusuni-login":
                cookie = value
        if method == "POST":
            expected = hmac.new(self.form_key, cookie.encode(), hashlib.sha256).hexdigest() if cookie else ""
            wait = self.throttle.wait(address)
            if not expected or not hmac.compare_digest(form.get("t", ""), expected):
                message = self.t("El formulario venció: vuelve a intentarlo.", "The form expired: try again.")
            elif wait > 0:
                message = self.t("Demasiados intentos: espera %d minutos.", "Too many tries: wait %d minutes.") % \
                          math.ceil(wait / 60.0)
            elif not admin:
                message = self.t("El panel aún no tiene contraseña.", "The panel has no password yet.")
            elif not self.hashing.acquire(timeout=10):
                message = self.t("El servidor está ocupado: vuelve a intentarlo.", "The server is busy: try again.")
            else:
                try:
                    good = hmac.compare_digest(
                        password_hash(form.get("password", "")[:1024], base64.b64decode(admin["salt"]), admin["n"],
                                      admin["r"], admin["p"]), base64.b64decode(admin["hash"]))
                finally:
                    self.hashing.release()
                counter = totp_match(admin["totp"], form.get("code", ""), last=self.last_counter) if good else -1
                if good and counter >= 0:
                    self.throttle.passed(address)
                    sid = secrets.token_urlsafe(32)
                    now = time.time()
                    with self.lock:
                        self.last_counter = counter
                        key = hashlib.sha256(sid.encode()).hexdigest()
                        self.sessions[key] = {"id": key, "created": now, "seen": now,
                                              "csrf": secrets.token_urlsafe(24)}
                    self.say("panel: logged in")
                    return self.send(h, 303, b"", headers={"Location": "/reports"},
                                     cookies=["__Host-emusuni=%s; Path=/; Secure; HttpOnly; SameSite=Strict; "
                                              "Max-Age=%d" % (sid, SESSION_SECONDS),
                                              "__Host-emusuni-login=; Path=/; Secure; HttpOnly; SameSite=Strict; "
                                              "Max-Age=0"])
                locked = self.throttle.failed(address)
                self.say("panel: a wrong login%s" % ("; locked out for 15 minutes" if locked else ""))
                message = self.t("Contraseña o código incorrectos.", "Wrong password or code.")
                if locked:
                    message = self.t("Demasiados intentos: espera 15 minutos.", "Too many tries: wait 15 minutes.")
        # (The login cookie's token kept: another /login - another tab, the browser's own /favicon.ico - must not end
        # the form already shown.)
        token = cookie if re.fullmatch(r"[A-Za-z0-9_-]{32}", cookie) else secrets.token_urlsafe(24)
        field = hmac.new(self.form_key, token.encode(), hashlib.sha256).hexdigest()
        body = '<div class="login card"><h1>%s</h1>' % self.t("Panel de EmuSuNi", "EmuSuNi's panel")
        if message:
            body += '<p class="msg bad">%s</p>' % esc(message)
        if not admin:
            body += "<p>%s</p><pre>sudo emusuni-reports admin-setup</pre>" % self.t(
                "Aún no tiene contraseña. En el servidor:", "It has no password yet. On the server:")
        body += ('<form method="post" action="/login" autocomplete="off"><input type="hidden" name="t" value="%s">'
                 '<label for="p">%s</label><input id="p" name="password" type="password" required '
                 'autocomplete="current-password"><label for="c">%s</label><input id="c" name="code" '
                 'inputmode="numeric" pattern="[0-9 ]{6,7}" maxlength="7" required autocomplete="one-time-code">'
                 '<p><button type="submit">%s</button></p></form></div>' %
                 (field, self.t("Contraseña", "Password"), self.t("Código de la app (6 cifras)", "App code (6 digits)"),
                  self.t("Entrar", "Log in")))
        return self.send(h, 200, self.page(self.t("Entrar", "Log in"), body, nav=False),
                         cookies=["__Host-emusuni-login=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=900" %
                                  token])

    # ---- A report's own panel data and what its log says ----
    def panel_data(self, code):
        try:
            with open(os.path.join(self.store.folder, code, "panel.json"), encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            data = {}
        if data.get("status") not in STATUSES:
            data["status"] = "new"
        return data

    def save_panel_data(self, code, data):
        path = os.path.join(self.store.folder, code, "panel.json")
        with open(path + ".part", "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(path + ".part", path)

    def read_file(self, code, name):
        try:
            with gzip.open(os.path.join(self.store.folder, code, name + ".gz"), "rt", encoding="utf-8",
                           errors="replace") as f:
                return f.read()
        except OSError:
            return None

    def info(self, code):
        """The console, core and game the log names last (its "game:" lines), its error lines (info.json: once)."""
        path = os.path.join(self.store.folder, code, "info.json")
        try:
            with open(path, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            pass
        info = {"game": "", "core": "", "console": "", "errors": 0}
        text = self.read_file(code, "emusuni.log") or ""
        folder = ""
        for line in text.splitlines():
            m = GAME_LINE.match(line)
            if m:
                info["game"] = m.group(1)
                parts = m.group(2).split("/")
                folder = parts[-2] if len(parts) >= 2 else ""
                continue
            m = CORE_LINE.match(line)
            if m:
                info["core"] = m.group(1).strip()
            if is_error(line):
                info["errors"] += 1
        info["console"] = CORES.get(info["core"], folder)
        try:
            with open(path + ".part", "w", encoding="utf-8") as f:
                json.dump(info, f)
            os.replace(path + ".part", path)
        except OSError:
            pass
        return info

    def rows(self):
        rows = []
        for code in self.store.codes():
            meta = self.store.meta(code)
            if not meta:
                continue
            row = dict(meta)
            row.update(self.info(code))
            row["panel"] = self.panel_data(code)
            row["size"] = sum(meta.get("files", {}).values())
            row["crash"] = "crash.log" in meta.get("files", {})
            rows.append(row)
        rows.sort(key=lambda r: -r.get("time", 0))
        return rows

    def status_name(self, status):
        return {"new": self.t("Nuevo", "New"), "reviewed": self.t("Revisado", "Reviewed"),
                "solved": self.t("Resuelto", "Solved")}.get(status, status)

    # ---- The list ----
    def reports(self, h, q):
        self.here = h.path
        rows = self.rows()
        text, status, version, country, console, crash = (q("q").strip(), q("status"), q("version"), q("country"),
                                                          q("console"), q("crash"))
        options = lambda key: sorted({r.get(key) or "" for r in rows} - {""})
        shown = []
        for r in rows:
            if status and r["panel"]["status"] != status:
                continue
            if version and r.get("version") != version:
                continue
            if country and r.get("country") != country:
                continue
            if console and r.get("console") != console:
                continue
            if crash == "yes" and not r["crash"] or crash == "no" and r["crash"]:
                continue
            if text:
                hay = " ".join([r.get("code", ""), r.get("game", ""), r.get("core", ""), r["panel"].get("note", ""),
                                r["panel"].get("issue", "")]).lower()
                if text.lower() not in hay:
                    continue
            shown.append(r)
        # (01.04) Its order (a column's header), then a page of it.
        order = self.sorting(q, "", ("time", "version", "country", "console", "game", "size", "status"), "time")
        keys = {"time": lambda r: r.get("time", 0), "version": lambda r: r.get("version") or "",
                "country": lambda r: r.get("country") or "", "console": lambda r: r.get("console") or "",
                "game": lambda r: (r.get("game") or "").lower(), "size": lambda r: r["size"],
                "status": lambda r: STATUSES.index(r["panel"]["status"]) if r["panel"]["status"] in STATUSES else 9}
        shown.sort(key=keys[order[0]], reverse=order[1])
        offset, limit, pager = self.pager(h, q, len(shown))  # (01.04: 25/50/100 a page)

        def select(name, values, current, labels=None):
            out = '<select name="%s"><option value="">%s</option>' % (name, self.t("Todos", "All"))
            for v in values:
                out += '<option value="%s"%s>%s</option>' % (esc(v), " selected" if v == current else "",
                                                            esc(labels.get(v, v) if labels else v))
            return out + "</select>"

        statuses = {s: self.status_name(s) for s in STATUSES}
        body = "<h1>%s</h1>" % self.t("Reportes de registros", "Log reports")
        body += ('<form class="filters" method="get" action="/reports"><label>%s<input name="q" value="%s" '
                 'placeholder="%s"></label><label>%s%s</label><label>%s%s</label><label>%s%s</label><label>%s%s</label>'
                 '<label>%s<select name="crash"><option value="">%s</option><option value="yes"%s>%s</option>'
                 '<option value="no"%s>%s</option></select></label><button type="submit">%s</button>'
                 '<a href="/reports">%s</a></form>' %
                 (self.t("Buscar", "Search"), esc(text), self.t("código, juego, nota", "code, game, note"),
                  self.t("Estado", "Status"), select("status", list(STATUSES), status, statuses),
                  self.t("Versión", "Version"), select("version", options("version"), version),
                  self.t("País", "Country"), select("country", options("country"), country),
                  self.t("Consola", "Console"), select("console", options("console"), console),
                  self.t("Cierre", "Crash"), self.t("Todos", "All"), " selected" if crash == "yes" else "",
                  self.t("Con crash.log", "With crash.log"), " selected" if crash == "no" else "",
                  self.t("Sin crash.log", "Without crash.log"), self.t("Filtrar", "Filter"),
                  self.t("Quitar filtros", "Clear")))
        new = sum(1 for r in rows if r["panel"]["status"] == "new")
        # (01.04) The key totals of what the filters show, on top.

        def top(key, n=3):
            counts = {}
            for r in shown:
                counts[r.get(key) or "?"] = counts.get(r.get(key) or "?", 0) + 1
            return " · ".join("%s %d" % (k, v) for k, v in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:n])                 or "-"

        body += self.summary([
            (self.t("Reportes", "Reports"), "%d / %d" % (len(shown), len(rows)) if len(shown) != len(rows) else len(rows)),
            (self.t("Nuevos", "New"), sum(1 for r in shown if r["panel"]["status"] == "new")),
            (self.t("Con crash.log", "With crash.log"), sum(1 for r in shown if r["crash"])),
            (self.t("Por versión", "By version"), top("version")),
            (self.t("Por país", "By country"), top("country"))])
        body += '<p class="muted">%s</p>' % (self.t("%d de %d reportes; %d nuevos.", "%d of %d reports; %d new.") %
                                            (len(shown), len(rows), new))
        body += pager
        if shown:
            body += '<div class="card"><table class="cards"><thead><tr><th>%s</th>%s</tr></thead><tbody>' % (
                self.t("Código", "Code"), "".join(self.th(h, q, "", label, key, order, cls) for label, key, cls in (
                    (self.t("Fecha (UTC)", "Date (UTC)"), "time", ""), (self.t("Versión", "Version"), "version", ""),
                    (self.t("País", "Country"), "country", ""), (self.t("Consola", "Console"), "console", ""),
                    (self.t("Juego", "Game"), "game", ""), (self.t("Tamaño", "Size"), "size", "r"),
                    (self.t("Estado", "Status"), "status", ""))))
            for r in shown[offset:offset + limit]:
                st = r["panel"]["status"]
                body += ('<tr><td data-label="%s"><a href="/r/%s">%s</a></td><td data-label="%s">%s</td>'
                         '<td data-label="%s">%s</td><td data-label="%s">%s</td><td data-label="%s">%s</td>'
                         '<td data-label="%s">%s</td><td class="r" data-label="%s">%s</td><td data-label="%s">'
                         '<span class="tag %s">%s</span>%s</td></tr>' %
                         (self.t("Código", "Code"), r["code"], r["code"], self.t("Fecha", "Date"),
                          self.when(r.get("time")), self.t("Versión", "Version"), esc(r.get("version", "?")),
                          self.t("País", "Country"), esc(flag(r.get("country", "??"))), self.t("Consola", "Console"),
                          esc(r.get("console") or "-"),
                          self.t("Juego", "Game"), esc(r.get("game") or "-"), self.t("Tamaño", "Size"),
                          kb(r["size"]), self.t("Estado", "Status"), st, esc(self.status_name(st)),
                          ' <span class="tag crash">crash.log</span>' if r["crash"] else ""))
            body += "</tbody></table></div>"
        else:
            body += '<p class="card">%s</p>' % self.t("Ningún reporte con estos filtros.", "No report with these filters.")
        body += pager.replace('id="t-list"', "")  # (the links again under it; the anchor once)
        return self.send(h, 200, self.page(self.t("Reportes", "Reports"), body))

    # ---- One report ----
    def report(self, h, code, method, form, q):
        self.here = "/r/" + code
        data = self.panel_data(code)
        saved = ""
        if method == "POST":
            status = form.get("status", data["status"])
            data["status"] = status if status in STATUSES else data["status"]
            data["note"] = form.get("note", "")[:4000]
            issue = form.get("issue", "").strip()
            if re.fullmatch(r"#?\d{1,7}", issue):
                issue = issue.lstrip("#")
            elif issue and not re.fullmatch(r"https://github\.com/[\w.-]+/[\w.-]+/(?:issues|pull|discussions)/\d+",
                                            issue):
                issue = ""
            data["issue"] = issue
            data["changed"] = int(time.time())
            self.save_panel_data(code, data)
            self.say("panel: report %s %s" % (code, data["status"]))
            return self.send(h, 303, b"", headers={"Location": "/r/%s?saved=1" % code})
        if q("saved"):
            saved = '<p class="msg good">%s</p>' % self.t("Guardado.", "Saved.")
        meta = self.store.meta(code) or {}
        info = self.info(code)
        issue = data.get("issue", "")
        issue_link = ""
        if issue:
            href = issue if issue.startswith("https://") else "https://github.com/arturofuentes1988/EmuSuNi/issues/" + issue
            issue_link = '<a href="%s" rel="noopener noreferrer" target="_blank">%s</a>' % (esc(href), esc(
                "#" + issue if not issue.startswith("https://") else issue))
        body = "<h1>%s %s</h1>%s" % (self.t("Reporte", "Report"), code, saved)
        body += ('<div class="card grid"><div><div class="muted">%s</div>%s UTC</div><div><div class="muted">%s</div>%s'
                 '</div><div><div class="muted">%s</div>%s</div><div><div class="muted">%s</div>%s</div><div>'
                 '<div class="muted">%s</div>%s</div><div><div class="muted">%s</div>%s</div></div>' %
                 (self.t("Fecha", "Date"), stamp(meta.get("time")), self.t("Versión", "Version"),
                  esc(meta.get("version", "?")), self.t("País", "Country"), esc(meta.get("country", "??")),
                  self.t("Consola", "Console"), esc(info.get("console") or "-"), self.t("Juego", "Game"),
                  esc(info.get("game") or "-"), self.t("Líneas con error", "Error lines"), info.get("errors", 0)))
        body += '<h2>%s</h2><div class="card"><table><tbody>' % self.t("Archivos", "Files")
        for name, size in sorted(meta.get("files", {}).items(), key=lambda kv: NAMES.index(kv[0])
                                 if kv[0] in NAMES else 9):
            body += ('<tr><td><a href="/r/%s/f/%s">%s</a></td><td class="r">%s</td><td class="r">'
                     '<a href="/r/%s/f/%s?errors=1">%s</a> &middot; <a href="/r/%s/raw/%s">%s</a></td></tr>' %
                     (code, name, name, kb(size), code, name, self.t("solo errores", "errors only"), code, name,
                      self.t("descargar", "download")))
        body += ('</tbody></table><p><a class="button" href="/r/%s/all.tar.gz">%s</a></p></div>' %
                 (code, self.t("Descargar todo (.tar.gz)", "Download all (.tar.gz)")))
        body += ('<h2>%s</h2><form class="card" method="post" action="/r/%s"><input type="hidden" name="csrf" '
                 'value="%s"><p><label>%s <select name="status">' %
                 (self.t("Seguimiento", "Follow-up"), code, esc(self.csrf), self.t("Estado", "Status")))
        for s in STATUSES:
            body += '<option value="%s"%s>%s</option>' % (s, " selected" if s == data["status"] else "",
                                                          esc(self.status_name(s)))
        body += ('</select></label></p><p><label>%s<br><input name="issue" value="%s" placeholder="45 %s https://'
                 'github.com/.../issues/45" size="40"></label> %s</p><p><label>%s<br><textarea name="note" '
                 'maxlength="4000">%s</textarea></label></p><p><button type="submit">%s</button></p></form>' %
                 (self.t("Issue de GitHub", "GitHub issue"), esc(issue), self.t("o", "or"), issue_link,
                  self.t("Notas", "Notes"), esc(data.get("note", "")), self.t("Guardar", "Save")))
        body += ('<h2>%s</h2><form class="card" method="post" action="/r/%s/delete"><input type="hidden" name="csrf" '
                 'value="%s"><p><label><input type="checkbox" name="confirm" value="yes" required> %s</label></p>'
                 '<p><button class="danger" type="submit">%s</button></p></form>' %
                 (self.t("Borrar", "Delete"), code, esc(self.csrf),
                  self.t("Sí, borrar este reporte para siempre", "Yes, delete this report for good"),
                  self.t("Borrar el reporte", "Delete the report")))
        return self.send(h, 200, self.page(self.t("Reporte", "Report") + " " + code, body))

    def delete(self, h, code, form):
        if form.get("confirm") != "yes":
            return self.send(h, 303, b"", headers={"Location": "/r/" + code})
        with self.store.lock:
            shutil.rmtree(os.path.join(self.store.folder, code), ignore_errors=True)
        self.say("panel: report %s deleted" % code)
        return self.send(h, 303, b"", headers={"Location": "/reports"})

    # ---- A file ----
    def viewer(self, h, code, name, q):
        self.here = "/r/%s/f/%s" % (code, name)
        text = self.read_file(code, name)
        if text is None:
            return self.send(h, 404, self.page(self.t("No está", "Not here"), ""))
        lines = text.splitlines()
        needle = q("q").strip()
        errors_only = q("errors") == "1"
        pattern = re.compile(re.escape(needle), re.I) if needle else None
        chosen = [i for i, line in enumerate(lines)
                  if (not errors_only or is_error(line)) and (not pattern or pattern.search(line))]
        filtered = errors_only or bool(pattern)
        try:
            page = max(1, int(q("page", "1")))
        except ValueError:
            page = 1
        pages = max(1, (len(chosen) + PAGE_LINES - 1) // PAGE_LINES)
        page = min(page, pages)

        def mark(line):
            if not pattern:
                return esc(line)
            out, at = "", 0
            for m in pattern.finditer(line):
                out += esc(line[at:m.start()]) + "<mark>" + esc(m.group(0)) + "</mark>"
                at = m.end()
            return out + esc(line[at:])

        body = '<p><a href="/r/%s">&larr; %s %s</a></p><h1>%s</h1>' % (code, self.t("Reporte", "Report"), code, name)
        body += ('<form class="filters" method="get" action="/r/%s/f/%s"><label>%s<input name="q" value="%s"></label>'
                 '<label><span>%s</span><select name="errors"><option value="">%s</option><option value="1"%s>%s'
                 '</option></select></label><button type="submit">%s</button><a href="/r/%s/f/%s">%s</a>'
                 '<a href="/r/%s/raw/%s">%s</a></form>' %
                 (code, name, self.t("Buscar en el archivo", "Search the file"), esc(needle),
                  self.t("Líneas", "Lines"), self.t("Todas", "All"), " selected" if errors_only else "",
                  self.t("Solo las de error", "Errors only"), self.t("Buscar", "Search"), code, name,
                  self.t("Todo el archivo", "The whole file"), code, name, self.t("Descargar", "Download")))
        body += '<p class="muted">%s</p>' % (
            (self.t("%d de %d líneas.", "%d of %d lines.") % (len(chosen), len(lines))) if filtered else
            (self.t("%d líneas.", "%d lines.") % len(lines)))
        body += '<pre class="log">'
        for i in chosen[(page - 1) * PAGE_LINES:page * PAGE_LINES]:
            line = lines[i]
            target = "/r/%s/f/%s?page=%d#L%d" % (code, name, i // PAGE_LINES + 1, i + 1)
            body += '<span class="l%s" id="L%d"><a class="n" href="%s">%d</a>%s</span>' % (
                " e" if is_error(line) else "", i + 1, esc(target), i + 1, mark(line))
        body += "</pre>"
        if pages > 1:
            body += '<div class="pages">'
            for p in range(1, pages + 1):
                if p == page:
                    body += "<span>%d</span>" % p
                else:
                    args = {"page": str(p)}
                    if needle:
                        args["q"] = needle
                    if errors_only:
                        args["errors"] = "1"
                    body += '<a href="/r/%s/f/%s?%s">%d</a>' % (code, name, esc(urllib.parse.urlencode(args)), p)
            body += "</div>"
        return self.send(h, 200, self.page("%s %s" % (code, name), body))

    def download(self, h, code, name):
        text = self.read_file(code, name)
        if text is None:
            return self.send(h, 404, self.page(self.t("No está", "Not here"), ""))
        return self.send(h, 200, text, "text/plain; charset=utf-8",
                         headers={"Content-Disposition": 'attachment; filename="%s-%s"' % (code, name)})

    def download_all(self, h, code):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as tar:
            for name in NAMES:
                text = self.read_file(code, name)
                if text is None:
                    continue
                raw = text.encode("utf-8")
                entry = tarfile.TarInfo("%s/%s" % (code, name))
                entry.size, entry.mtime = len(raw), int(time.time())
                tar.addfile(entry, io.BytesIO(raw))
            meta = json.dumps(self.store.meta(code) or {}, indent=1).encode()
            entry = tarfile.TarInfo("%s/meta.json" % code)
            entry.size, entry.mtime = len(meta), int(time.time())
            tar.addfile(entry, io.BytesIO(meta))
        return self.send(h, 200, data.getvalue(), "application/gzip",
                         headers={"Content-Disposition": 'attachment; filename="%s.tar.gz"' % code})

    # ---- The statistics ----
    def stats_page(self, h, q):
        self.here = h.path
        try:
            days = int(q("days", "30"))
        except ValueError:
            days = 30
        days = days if days in (1, 7, 30, 90, 365) else 30
        try:
            s = self.stats.report(days)
        except sqlite3.Error as e:
            s = None
            self.say("panel: stats.db not read (%s)" % type(e).__name__)
        body = "<h1>%s</h1>" % self.t("Estadísticas (privadas)", "Statistics (private)")
        body += '<div class="pages">'
        for d in (1, 7, 30, 90, 365):
            label = self.t("Hoy", "Today") if d == 1 else self.t("%d días", "%d days") % d
            body += "<span>%s</span>" % label if d == days else '<a href="/stats?days=%d">%s</a>' % (d, label)
        body += "</div>"
        if not s:
            body += '<p class="card">%s</p>' % self.t("No se pudo leer stats.db.", "stats.db couldn't be read.")
        else:
            live = s["live"]
            # (00.95) What the public page counts.
            body += '<p class="muted">%s: %s</p>' % (
                self.t("Aquí, todas las versiones. La web pública cuenta solo las publicadas en GitHub",
                       "Here, every version. The public page counts only those published on GitHub"),
                esc(", ".join(sorted(s.get("published", set()))) or self.t("ninguna todavía", "none yet")))
            body += '<div class="grid">'
            cards = [(self.t("En línea ahora", "Online now"), live["online"] if live["current"] else "?"),
                     (self.t("Activas hoy", "Active today"), s["active"]["today"]),
                     (self.t("Ayer", "Yesterday"), s["active"]["yesterday"]),
                     (self.t("Últimos 7 días", "Last 7 days"), s["active"]["7"]),
                     (self.t("Últimos 30 días", "Last 30 days"), s["active"]["30"]),
                     (self.t("Instalaciones nuevas", "New installs"), s["new"]),
                     (self.t("Consultas de actualización", "Update checks"), s["checks"])]
            for label, value in cards:
                body += '<div class="card"><div class="muted">%s</div><div class="num">%s</div></div>' % (
                    esc(label), esc(value))
            body += "</div>"
            if live["by"]:
                body += '<div class="card"><b>%s</b> (%s UTC): ' % (self.t("Ahora", "Now"), stamp(live["time"]))
                body += " &middot; ".join("%s: %s" % (esc(kind), ", ".join("%s %d" % (esc(v), n) for v, n in values[:8]))
                                          for kind, values in sorted(live["by"].items()))
                body += "</div>"
            body += "<h2>%s</h2><div class=\"card\">%s</div>" % (self.t("Por día", "By day"), self.chart(s["daily"]))
            names = {"country": self.t("Países", "Countries"), "version": self.t("Versiones", "Versions"),
                     "language": self.t("Idiomas de la PS4", "PS4 languages"),
                     "state": self.t("Qué hacen", "What they do"), "console": self.t("Consolas", "Consoles"),
                     "game": self.t("Juegos (por horas)", "Games (by hours)"),
                     "community": self.t("Modos en línea de la comunidad (desde la 01.04)",
                                         "Community online modes (since 01.04)")}
            # (01.04) The period's most, on top; then each table a page at a time, in a section that opens (the
            # versions open; another once it is paged).
            by = {kind: s["by"].get(kind, []) for kind in names}
            by["game"] = sorted(by["game"], key=lambda v: -v["hours"])
            firsts = []
            for kind in ("version", "country", "console", "game"):
                if by[kind]:
                    v = by[kind][0]
                    firsts.append((self.t("Más: ", "Top: ") + names[kind].split(" (")[0].lower(),
                                   (v["title"] or v["value"]) if kind == "game" else v["value"]))
            if firsts:
                body += self.summary(firsts)
            for kind in ("version", "country", "console", "game", "state", "language", "community"):
                hours = kind in ("console", "state", "game", "community")
                name = kind + "_"
                order = self.sorting(q, name, ("value", "installs", "hours") if hours else ("value", "installs"),
                                     "hours" if kind == "game" else "installs", default_desc=True)
                values = sorted(by[kind], key=lambda v: (v[order[0]] if order[0] != "value" else str(v["value"]).lower(),
                                                         v["installs"]), reverse=order[1])
                offset, limit, pager = self.pager(h, q, len(values), name)
                # (On a phone only the games' table - 4 columns - becomes cards.)
                inner = pager + '<div class="card"><table%s><thead><tr>%s%s%s%s</tr></thead><tbody>' % (
                    ' class="cards"' if kind == "game" else "", self.th(h, q, name, self.t("Valor", "Value"), "value", order),
                    "<th>%s</th>" % self.t("Título", "Title") if kind == "game" else "",
                    self.th(h, q, name, self.t("Instalaciones", "Installs"), "installs", order, "r"),
                    self.th(h, q, name, self.t("Horas", "Hours"), "hours", order, "r") if hours else "")
                for v in values[offset:offset + limit]:
                    # (00.95) A version not published on GitHub: a test one (not on the public page).
                    test = kind == "version" and v["value"] not in s.get("published", set())
                    inner += '<tr><td data-label="%s">%s</td>%s<td class="r" data-label="%s">%d</td>%s</tr>' % (
                        names[kind], esc(flag(v["value"]) if kind == "country" else v["value"]) + (
                            ' <span class="muted">(%s)</span>' %
                                                        self.t("prueba", "test") if test else ""),
                        '<td data-label="%s">%s</td>' % (self.t("Título", "Title"), esc(v["title"] or "-"))
                        if kind == "game" else "", self.t("Instalaciones", "Installs"), v["installs"],
                        '<td class="r" data-label="%s">%.1f</td>' % (self.t("Horas", "Hours"), v["hours"])
                        if hours else "")
                if not values:
                    inner += "<tr><td>-</td></tr>"
                inner += "</tbody></table></div>"
                body += self.section("%s (%d)" % (names[kind], len(values)), inner, q, kind + "_",
                                     open_default=kind == "version")
            if s["updates"]:
                offset, limit, pager = self.pager(h, q, len(s["updates"]), "update_")
                inner = pager + '<div class="card"><table><tbody>'
                for u in s["updates"][offset:offset + limit]:
                    inner += '<tr><td>%s &rarr; %s</td><td class="r">%d</td></tr>' % (esc(u["from"]), esc(u["to"]),
                                                                                    u["installs"])
                inner += "</tbody></table></div>"
                body += self.section("%s (%d)" % (self.t("Actualizaciones instaladas", "Updates installed"),
                                                  len(s["updates"])), inner, q, "update_")
        gh = self.github.releases()
        body += "<h2>%s</h2>" % self.t("Descargas en GitHub", "GitHub downloads")
        if gh and gh.get("releases"):
            body += '<div class="card"><p class="muted">%s %s UTC%s</p><table class="cards"><thead><tr><th>%s</th>' \
                    '<th>%s</th><th class="r">.pkg</th><th class="r">%s</th></tr></thead><tbody>' % (
                        self.t("Leídas de GitHub el", "Read from GitHub on"), stamp(gh.get("time")),
                        self.t(" (sin conexión ahora: las de antes)", " (no connection now: the ones before)")
                        if gh.get("stale") else "", self.t("Versión", "Version"), self.t("Publicada", "Published"),
                        self.t("Todos los archivos", "All files"))
            for rel in gh["releases"]:
                body += '<tr><td data-label="%s">%s</td><td data-label="%s">%s</td><td class="r" data-label=".pkg">' \
                        '%d</td><td class="r" data-label="%s">%d</td></tr>' % (
                            self.t("Versión", "Version"), esc(rel["tag"]), self.t("Publicada", "Published"),
                            esc(rel["date"]), rel["pkg"], self.t("Todos", "All"), rel["all"])
            body += "</tbody></table></div>"
        else:
            body += '<p class="card">%s</p>' % self.t("GitHub no respondió.", "GitHub didn't answer.")
        body += '<p class="muted">%s</p>' % self.t(
            "Países: IP Geolocation by DB-IP. Las instalaciones de varios días son estimaciones (HyperLogLog).",
            "Countries: IP Geolocation by DB-IP. Installs over several days are estimates (HyperLogLog).")
        return self.send(h, 200, self.page(self.t("Estadísticas", "Statistics"), body))

    def visits_page(self, h, q):
        """(01.04) emusuni.com's visits: the period's views and visitors, by day, page and country."""
        self.here = h.path
        try:
            days = int(q("days", "30"))
        except ValueError:
            days = 30
        days = days if days in (1, 7, 30, 90, 365) else 30
        try:
            v = self.visits.report(days)
        except sqlite3.Error as e:
            v = None
            self.say("panel: visits.db not read (%s)" % type(e).__name__)
        body = "<h1>%s</h1>" % self.t("Visitas de emusuni.com", "emusuni.com's visits")
        body += '<div class="pages">'
        for d in (1, 7, 30, 90, 365):
            label = self.t("Hoy", "Today") if d == 1 else self.t("%d días", "%d days") % d
            body += "<span>%s</span>" % label if d == days else '<a href="/visits?days=%d">%s</a>' % (d, label)
        body += "</div>"
        if not v:
            body += '<p class="card">%s</p>' % self.t("Todavía no hay visitas contadas.", "No visits counted yet.")
            return self.send(h, 200, self.page(self.t("Visitas", "Visits"), body))
        names = {"home": self.t("Portada", "Home"), "stats": self.t("Estadísticas", "Statistics"),
                 "compat": self.t("Compatibilidad", "Compatibility")}
        # (01.04) The key totals on top: the views, the visitors, the page and the country most seen.
        body += self.summary([
            (self.t("Vistas", "Page views"), v["views"]),
            (self.t("Visitantes (suma de cada día)", "Visitors (each day's, added)"), v["visitors"]),
            (self.t("Página más vista", "Top page"), names.get(v["pages"][0][0], v["pages"][0][0]) if v["pages"] else "-"),
            (self.t("País con más vistas", "Top country"), flag(v["countries"][0][0]) if v["countries"] else "-"),
            (self.t("Vistas desde el %s", "Views since %s") % (v["since"] or "-"), v["total"])])
        body += "<h2>%s</h2><div class=\"card\">%s</div>" % (self.t("Por día", "By day"), self.chart(
            v["daily"], self.t("Barras: vistas; verde: visitantes del día.", "Bars: page views; green: the day's visitors.")))
        inner = '<div class="card"><table><thead><tr><th>%s</th><th class="r">%s</th></tr></thead><tbody>' % (
            self.t("Página", "Page"), self.t("Vistas", "Views"))
        for page, n in v["pages"]:
            inner += '<tr><td>%s</td><td class="r">%d</td></tr>' % (esc(names.get(page, page)), n)
        if not v["pages"]:
            inner += "<tr><td>-</td></tr>"
        body += self.section(self.t("Por página", "By page"), inner + "</tbody></table></div>", q, open_default=True)
        order = self.sorting(q, "c_", ("country", "views", "visitors"), "views")
        countries = sorted(v["countries"], key=lambda c: c[{"country": 0, "views": 1, "visitors": 2}[order[0]]],
                           reverse=order[1])
        offset, limit, pager = self.pager(h, q, len(countries), "c_")
        inner = pager + '<div class="card"><table><thead><tr>%s%s%s</tr></thead><tbody>' % (
            self.th(h, q, "c_", self.t("País", "Country"), "country", order),
            self.th(h, q, "c_", self.t("Vistas", "Views"), "views", order, "r"),
            self.th(h, q, "c_", self.t("Visitantes", "Visitors"), "visitors", order, "r"))
        for country, n, people in countries[offset:offset + limit]:
            inner += '<tr><td>%s</td><td class="r">%d</td><td class="r">%d</td></tr>' % (esc(flag(country)), n, people)
        if not v["countries"]:
            inner += "<tr><td>-</td></tr>"
        body += self.section("%s (%d)" % (self.t("Por país", "By country"), len(v["countries"])),
                             inner + "</tbody></table></div>", q, "c_", open_default=True)
        body += '<p class="muted">%s</p>' % self.t(
            "Solo el día, la página y el país (IP Geolocation by DB-IP); nunca la dirección. Un visitante es único en "
            "su día (un hash con sal del día, en memoria); en varios días, los visitantes se suman.",
            "Only the day, the page and the country (IP Geolocation by DB-IP); never the address. A visitor is unique "
            "in its day (a hash with the day's salt, in memory); over several days the visitors are added up.")
        return self.send(h, 200, self.page(self.t("Visitas", "Visits"), body))

    # ---- (01.07, the owner: "30 días, sin IP") The world chat's (and, next, the rooms') lines ----
    def read_chat_blocks(self):
        """The blocks file's lines: (kind, value, salt, until, made) - kind "code" or "ip"."""
        out = []
        try:
            with open(self.chat_blocks, encoding="utf-8") as f:
                for line in f:
                    words = line.split()
                    if not words or words[0].startswith("#"):
                        continue
                    try:
                        if words[0] == "code" and len(words) >= 3:
                            out.append(("code", words[1], -1, int(words[2]), int(words[3]) if len(words) > 3 else 0))
                        elif words[0] == "ip" and len(words) >= 4:
                            out.append(("ip", words[1], int(words[2]), int(words[3]),
                                        int(words[4]) if len(words) > 4 else 0))
                    except ValueError:
                        continue
        except OSError:
            pass
        return out

    def write_chat_blocks(self, blocks):
        """Written whole (a new file renamed over the old one: netd never reads half of it), the expired left out."""
        now = int(time.time())
        folder = os.path.dirname(self.chat_blocks)
        os.makedirs(folder, mode=0o755, exist_ok=True)
        lines = ["# emusuni-netd --chat-blocks (the owner's panel): code CODE UNTIL MADE / ip HASH SALT UNTIL MADE"]
        for kind, value, salt, until, made in blocks:
            if until and until <= now:
                continue
            lines.append("code %s %d %d" % (value, until, made) if kind == "code" else
                         "ip %s %d %d %d" % (value, salt, until, made))
        tmp = self.chat_blocks + ".new"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os.chmod(tmp, 0o644)
        os.replace(tmp, self.chat_blocks)

    def chat_block(self, h, form, block):
        """Block (1 day, 7 days, always) or unblock a player's code or an address's hash; back to the page asked from."""
        kind, value = form.get("kind", ""), form.get("value", "")
        back = form.get("back", "/chat")
        if not back.startswith("/chat") or "\\" in back or back.startswith("//"):
            back = "/chat"
        ok = (kind == "code" and re.fullmatch("[%s]{8}" % ALPHABET, value)) or \
             (kind == "ip" and re.fullmatch("[0-9a-f]{16}", value))
        try:
            salt = int(form.get("salt", "-1")) if kind == "ip" else -1
            days = int(form.get("days", "1"))
        except ValueError:
            ok = False
        if not ok or (block and days not in CHAT_BLOCK_DAYS):
            return self.send(h, 400, self.page(self.t("No se pudo", "Couldn't"), ""))
        blocks = [b for b in self.read_chat_blocks() if not (b[0] == kind and b[1] == value and (kind == "code" or
                                                                                               b[2] == salt))]
        now = int(time.time())
        if block:
            blocks.append((kind, value, salt, now + days * 86400 if days else 0, now))
        try:
            self.write_chat_blocks(blocks)
        except OSError as e:
            self.say("panel: the chat's blocks not written (%s)" % type(e).__name__)
            return self.send(h, 500, self.page(self.t("No se pudo escribir", "Couldn't write"), ""))
        self.say("panel: the world chat: a %s %s (%s)" % ("code" if kind == "code" else "hash",
                                                          "blocked" if block else "unblocked",
                                                          ("%d days" % days if days else "always") if block else "-"))
        return self.send(h, 303, b"", headers={"Location": back})

    def chat_page(self, h, q):
        """The chat's lines (stats.db's chat_log, read only - 30 days): searched by nickname, player's code, room (or
        "world"), day; the reported ones first; a line's 20 around it (context); each player's code and address's hash
        blocked from the world chat here (1 day, 7 days, always - a hash as long as its salt) - netd reads the blocks
        file (--chat-blocks). No address anywhere: its keyed hash only."""
        self.here = h.path
        nick, code, place, day = q("nick").strip(), q("code").strip().upper(), q("place").strip(), q("day").strip()
        reported = q("reported") == "1"
        try:
            around = int(q("around", "0"))
        except ValueError:
            around = 0
        where, args = [], []
        if nick:
            where.append("nick LIKE ?")
            args.append("%" + nick.replace("%", "") + "%")
        if code:
            where.append("code = ?")
            args.append(code)
        if place:
            where.append("place = ?")
            args.append(place if place == "world" else place.upper())
        if re.fullmatch(r"\d{4}-\d{2}-\d{2}", day):
            start = calendar.timegm(time.strptime(day, "%Y-%m-%d"))
            where.append("at >= ? AND at < ?")
            args += [start, start + 86400]
        if reported:
            where.append("reported > 0")
        body = "<h1>%s</h1>" % self.t("Chat (mundial y de salas)", "Chat (world and rooms)")
        body += '<p class="muted">%s</p>' % self.t(
            "Las líneas de los últimos 30 días, para moderar; después se borran solas. Nunca la IP: solo su hash con una "
            "sal que el servidor cambia cada 30 días. La voz no se graba.",
            "The last 30 days' lines, for moderation; then deleted by themselves. Never the address: only its hash with a "
            "salt the server changes every 30 days. Voice isn't recorded.")
        body += ('<form method="get" action="/chat" class="card filters">'
                 '<input name="nick" placeholder="%s" value="%s"> <input name="code" placeholder="%s" value="%s" '
                 'size="9"> <input name="place" placeholder="%s" value="%s" size="9"> '
                 '<input name="day" placeholder="AAAA-MM-DD" value="%s" size="11"> '
                 '<label><input type="checkbox" name="reported" value="1"%s> %s</label> '
                 '<button type="submit">%s</button> <a href="/chat">%s</a></form>' % (
                     self.t("Apodo", "Nickname"), esc(nick), self.t("Código", "Code"), esc(code),
                     self.t("Sala o world", "Room or world"), esc(place), esc(day), " checked" if reported else "",
                     self.t("Solo reportadas", "Reported only"), self.t("Buscar", "Search"), self.t("Todo", "All")))
        try:
            db = sqlite3.connect("file:%s?mode=ro" % urllib.parse.quote(self.stats.path), uri=True, timeout=5)
        except sqlite3.Error:
            db = None
        rows, total = [], 0
        try:
            if db is None:
                raise sqlite3.OperationalError("no stats.db")
            columns = "id, at, place, nick, code, country, version, phrase, param, text, ip_hash, salt, reported"
            if around > 0:
                # (A reported line's context: the 10 before and after it in its place.)
                center = db.execute("SELECT place FROM chat_log WHERE id = ?", (around,)).fetchone()
                if center:
                    before = db.execute("SELECT %s FROM chat_log WHERE place = ? AND id <= ? ORDER BY id DESC LIMIT 11"
                                        % columns, (center[0], around)).fetchall()
                    after = db.execute("SELECT %s FROM chat_log WHERE place = ? AND id > ? ORDER BY id LIMIT 10"
                                       % columns, (center[0], around)).fetchall()
                    rows = list(reversed(before)) + after
                total = len(rows)
            else:
                clause = (" WHERE " + " AND ".join(where)) if where else ""
                total = db.execute("SELECT COUNT(*) FROM chat_log" + clause, args).fetchone()[0]
                offset, limit, pager = self.pager(h, q, total, "c_")
                rows = db.execute("SELECT %s FROM chat_log%s ORDER BY reported > 0 DESC, id DESC LIMIT ? OFFSET ?" %
                                  (columns, clause), args + [limit, offset]).fetchall()
                body += pager
        except sqlite3.Error as e:
            body += '<p class="card">%s</p>' % self.t(
                "Todavía no hay chat guardado (llega con el servidor de la 01.07).",
                "No chat kept yet (it comes with 01.07's server).")
            self.say("panel: chat_log not read (%s)" % type(e).__name__)
            rows = None
        finally:
            if db is not None:
                db.close()
        blocks = self.read_chat_blocks()
        blocked_codes = {b[1] for b in blocks if b[0] == "code"}
        blocked_hashes = {(b[1], b[2]) for b in blocks if b[0] == "ip"}

        def block_form(kind, value, salt):
            out = ""
            for days in CHAT_BLOCK_DAYS:
                label = self.t("siempre", "always") if days == 0 else self.t("%d día(s)", "%d day(s)") % days
                out += ('<form method="post" action="/chat/block" class="inline"><input type="hidden" name="csrf" '
                        'value="%s"><input type="hidden" name="kind" value="%s"><input type="hidden" name="value" '
                        'value="%s"><input type="hidden" name="salt" value="%d"><input type="hidden" name="days" '
                        'value="%d"><input type="hidden" name="back" value="%s"><button class="plain" type="submit">'
                        '%s</button></form>' % (esc(self.csrf), kind, esc(value), salt, days, esc(h.path), label))
            return out

        if rows is not None:
            if around > 0:
                body += '<p><a href="/chat">&laquo; %s</a> &middot; %s</p>' % (
                    self.t("Volver", "Back"), self.t("Las 20 líneas alrededor de la reportada (marcada).",
                                                     "The 20 lines around the reported one (marked)."))
            body += '<div class="card"><table class="cards"><thead><tr>%s</tr></thead><tbody>' % "".join(
                "<th>%s</th>" % esc(c) for c in (
                    self.t("Hora (UTC)", "Time (UTC)"), self.t("Dónde", "Where"), self.t("Apodo", "Nickname"),
                    self.t("Código", "Code"), self.t("País", "Country"), self.t("Versión", "Version"),
                    self.t("Mensaje", "Message"), self.t("Reportes", "Reports"),
                    self.t("Bloquear código", "Block the code"), self.t("Bloquear IP (hash)", "Block the address (hash)")))
            for (rid, at, where_, nick_, code_, country, version, phrase, param, text, ip_hash, salt, rep) in rows:
                message = text if not phrase else "[%s] %s%s" % (
                    self.t("frase %d", "phrase %d") % phrase, PHRASES[phrase] if phrase < len(PHRASES) else "?",
                    " (%s)" % param if param else "")
                mark = ' class="hl"' if rid == around else ""
                context = ' <a href="/chat?around=%d">%s</a>' % (rid, self.t("contexto", "context")) if rep else ""
                code_cell = ('<a href="/chat?code=%s">%s</a>' % (esc(code_), esc(code_))) + (
                    " &#x1F6AB;" if code_ in blocked_codes else "")
                body += ("<tr%s><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
                         "<td>%s%s</td><td>%s</td><td>%s</td></tr>" % (
                             mark, stamp(at), esc(where_), esc(nick_), code_cell, flag(country) if country else "-",
                             esc(version), esc(message), rep or "", context, block_form("code", code_, -1),
                             (block_form("ip", ip_hash, salt) + (" &#x1F6AB;" if (ip_hash, salt) in blocked_hashes
                                                                 else "")) if ip_hash else "-"))
            body += "</tbody></table></div>"
            if not rows:
                body += '<p class="muted">%s</p>' % self.t("Ninguna línea.", "No line.")
        body += "<h2>%s</h2>" % self.t("Bloqueados del chat mundial", "Blocked from the world chat")
        if not blocks:
            body += '<p class="muted">%s</p>' % self.t("Nadie.", "No one.")
        else:
            body += '<div class="card"><table><thead><tr><th>%s</th><th>%s</th><th>%s</th><th></th></tr></thead><tbody>' % (
                self.t("Qué", "What"), self.t("Hasta (UTC)", "Until (UTC)"), self.t("Desde (UTC)", "Since (UTC)"))
            for kind, value, salt, until, made in blocks:
                what = (self.t("código ", "code ") + esc(value)) if kind == "code" else (
                    self.t("hash de IP ", "address hash ") + esc(value))
                body += ('<tr><td>%s</td><td>%s</td><td>%s</td><td><form method="post" action="/chat/unblock" '
                         'class="inline"><input type="hidden" name="csrf" value="%s"><input type="hidden" name="kind" '
                         'value="%s"><input type="hidden" name="value" value="%s"><input type="hidden" name="salt" '
                         'value="%d"><input type="hidden" name="back" value="%s"><button class="plain" type="submit">'
                         '%s</button></form></td></tr>' % (
                             what, stamp(until) if until else self.t("siempre", "always"), stamp(made),
                             esc(self.csrf), kind, esc(value), salt, esc(h.path), self.t("Desbloquear", "Unblock")))
            body += "</tbody></table></div>"
        body += '<p class="muted">%s</p>' % (self.t("Archivo de bloqueos para netd (--chat-blocks): ",
                                                   "The blocks file for netd (--chat-blocks): ") + esc(self.chat_blocks))
        return self.send(h, 200, self.page("Chat", body))

    def rooms_page(self, h, q):
        """(01.04) Online play's rooms (emusuni-netd's room_log): did they play - each room's matches, how long, who came,
        left or was refused and why, why it closed -, today's and the period's totals, the refusals, the games played.
        (The owner, after the first real history: "no se entiende si alguien jugó, qué y cuánto".)"""
        self.here = h.path
        try:
            days = int(q("days", "7"))
        except ValueError:
            days = 7
        days = days if days in (1, 7, 30, 90, 365) else 7
        pagers = {}

        def page_of(name, total):  # (01.04: the latest rooms and the games, a page at a time in SQL)
            offset, limit, pagers[name] = self.pager(h, q, total, name)
            return offset, limit

        orders = {"r_": self.sorting(q, "r_", tuple(ROOM_ORDERS), "created"),
                  "g_": self.sorting(q, "g_", tuple(GAME_ORDERS), "minutes")}
        try:
            r = rooms_report(self.stats, days, page_of=page_of, orders=orders)
        except sqlite3.Error as e:
            r = None
            self.say("panel: room_log not read (%s)" % type(e).__name__)
        body = "<h1>%s</h1>" % self.t("Salas en línea", "Online rooms")
        body += '<div class="pages">'
        for d in (1, 7, 30, 90, 365):
            label = self.t("Hoy", "Today") if d == 1 else self.t("%d días", "%d days") % d
            body += "<span>%s</span>" % label if d == days else '<a href="/rooms?days=%d">%s</a>' % (d, label)
        body += "</div>"
        if r is None:
            body += '<p class="card">%s</p>' % self.t("Todavía no hay historial de salas (llega con el servidor de la "
                                                      "01.04).", "No room history yet (it comes with 01.04's server).")
            return self.send(h, 200, self.page(self.t("Salas", "Rooms"), body))

        def clock(seconds):
            seconds = int(seconds or 0)
            return "%d:%02d:%02d" % (seconds // 3600, seconds // 60 % 60, seconds % 60) if seconds >= 3600 else \
                "%d:%02d" % (seconds // 60, seconds % 60)

        def share(part, whole):
            return "%d%%" % round(100.0 * part / whole) if whole else "-"

        today = r["today"] or {"rooms": 0, "played": 0, "minutes": 0.0}
        body += "<h2>%s</h2><div class=\"grid\">" % self.t("Hoy (UTC)", "Today (UTC)")
        for label, value in ((self.t("Salas", "Rooms"), today["rooms"]),
                             (self.t("Con partida", "With a match"), today["played"]),
                             (self.t("Tiempo de partida", "Match time"), clock(today["minutes"] * 60)),
                             (self.t("Con partida, %", "With a match, %"), share(today["played"], today["rooms"]))):
            body += '<div class="card"><div class="muted">%s</div><div class="num">%s</div></div>' % (
                esc(label), esc(value))
        body += "</div><h2>%s</h2><div class=\"grid\">" % (self.t("Hoy", "Today") if days == 1 else
                                                        self.t("Últimos %d días", "Last %d days") % days)
        cards = [(self.t("Salas", "Rooms"), r["count"]), (self.t("Con partida", "With a match"), r["played"]),
                 (self.t("Con partida, %", "With a match, %"), share(r["played"], r["count"])),
                 (self.t("Tiempo de partida", "Match time"), clock(r["minutes"] * 60)),
                 (self.t("Entraron / se fueron", "Joined / left"), "%d / %d" % (r["joins"], r["left"])),
                 (self.t("Entradas rechazadas", "Joins refused"), r["refused"])]
        if r["together_known"]:
            cards.append((self.t("Elegir juntos sin juegos en común", "Choose Together, no game in common"),
                          "%d / %d" % (r["together_none"], r["together_known"])))
        for label, value in cards:
            body += '<div class="card"><div class="muted">%s</div><div class="num">%s</div></div>' % (
                esc(label), esc(value))
        body += "</div>"
        body += "<h2>%s</h2><div class=\"card\">%s</div>" % (self.t("Por día", "By day"), self.chart(
            [{"date": d["date"], "active": d["rooms"], "peak": d["played"]} for d in r["daily"]],
            self.t("Barras: salas creadas; verde: salas con partida.", "Bars: rooms made; green: rooms with a match.")))
        kinds = {"public": self.t("pública", "public"), "private": self.t("privada", "private"),
                 "together": self.t("elegir juntos", "choose together"), "link": self.t("cable link", "link cable")}
        reasons = {"host-left": self.t("el anfitrión salió", "the host left"),
                   "host-silent": self.t("se perdió la conexión del anfitrión", "the host's connection was lost"),
                   "too-old": self.t("pasaron 12 horas", "12 hours went by"),
                   "server-stopped": self.t("se reinició el servidor", "the server restarted")}
        why_names = {"full": self.t("llena", "full"), "version": self.t("otra versión", "another version"),
                     "started": self.t("ya empezada", "already started"), "kicked": self.t("expulsado", "removed"),
                     "no-room": self.t("código que no existe", "no such code"),
                     "too-many": self.t("demasiados intentos", "too many tries"),
                     "server-full": self.t("servidor lleno", "server full"),
                     "bad-request": self.t("paquete ilegible", "unreadable packet"),
                     "bad-cookie": self.t("cookie vieja", "old cookie"),
                     "no-watch": self.t("no deja mirar", "no spectators let in")}
        columns = (self.t("Creada (UTC)", "Made (UTC)"), self.t("¿Jugaron?", "Played?"), self.t("Juego", "Game"),
                   self.t("Tipo", "Kind"), self.t("Jugadores", "Players"), self.t("Partida", "Match"),
                   self.t("Gente", "People"), self.t("Duró", "Lasted"), self.t("Cerró porque", "Closed because"),
                   self.t("Versión", "Version"), self.t("Países", "Countries"))
        sortable = {0: "created", 1: "played", 4: "peak", 5: "match", 7: "lasted"}
        inner = pagers.get("r_", "") + '<div class="card"><table class="cards"><thead><tr>%s</tr></thead><tbody>' % (
            "".join(self.th(h, q, "r_", c, sortable[i], orders["r_"], "r" if i == 4 else "") if i in sortable else
                    "<th>%s</th>" % esc(c) for i, c in enumerate(columns)))
        for room in r["latest"]:
            if room["played"]:
                played = '<span class="badge yes">%s</span>' % self.t("Sí", "Yes")
            elif room["matches"]:
                played = '<span class="badge short">%s</span>' % self.t("No: menos de 30 s", "No: under 30 s")
            else:
                played = '<span class="badge no">%s</span>' % self.t("No", "No")
            match = self.t("%d partida(s), %s", "%d match(es), %s") % (room["matches"], clock(room["match_seconds"])) \
                if room["matches"] else "-"
            if room["relayed_kb"]:
                match += ", relay %d KB" % room["relayed_kb"]
            game = room["title"] or (room["game"].split(".", 1)[-1].replace("c-", "CRC ") if room["game"] else
                                     self.t("(elegir juntos: sin juego elegido)", "(choose together: none chosen)")
                                     if room["kind"] == "together" else "-")
            if room["kind"] == "together" and room["common"] >= 0:
                game += " · " + (self.t("sin juegos en común", "no game in common") if room["common"] == 0 else
                                 self.t("%d en común", "%d in common") % room["common"])
            people = self.t("%d/%d; entraron %d, se fueron %d", "%d/%d; %d joined, %d left") % (
                room["peak"], room["max_members"], room["joins"], room["guests_left"])
            if room["dropped"]:
                people += self.t(", %d sin conexión", ", %d lost") % room["dropped"]
            if room["late_joins"]:  # (01.05) players who joined the match under way
                people += self.t(", %d entraron con la partida empezada", ", %d joined the match under way") % \
                    room["late_joins"]
            if room["watchers"]:  # (01.05) spectators
                people += self.t(", %d miraron", ", %d watched") % room["watchers"]
            refused = [(n, why_names[k]) for k, n in (("full", room["refused_full"]), ("version", room["refused_version"]),
                                                      ("started", room["refused_started"]),
                                                      ("kicked", room["refused_kicked"])) if n]
            if room["host_refused"]:
                refused.append((room["host_refused"], self.t("el anfitrión no aceptó", "the host said no")))
            if room["host_no_answer"]:
                refused.append((room["host_no_answer"], self.t("el anfitrión no contestó", "the host didn't answer")))
            if refused:
                people += "; " + self.t("rechazados: ", "refused: ") + ", ".join("%d %s" % (n, w) for n, w in refused)
            lasted = max(0, (room["closed"] or 0) - (room["created"] or 0))
            # (Each cell as HTML: the time and the badge are already; the rest escaped.)
            cells = (self.when(room["created"]), played, esc(game), esc(kinds.get(room["kind"], room["kind"])),
                     esc(people.split(";")[0]), esc(match), esc(people.split("; ", 1)[1] if "; " in people else "-"),
                     esc(clock(lasted)), esc(reasons.get(room["reason"], room["reason"])), esc(room["version"]),
                     esc(flags(room["countries"])))
            classes = {2: ' class="wide"', 4: ' class="r"', 6: ' class="wide"', 7: ' class="r"'}
            inner += '<tr class="%s">%s</tr>' % ("yes" if room["played"] else "no", "".join(
                '<td data-label="%s"%s>%s</td>' % (esc(c), classes.get(i, ""), v)
                for i, (c, v) in enumerate(zip(columns, cells))))
        if not r["latest"]:
            inner += "<tr><td>-</td></tr>"
        body += self.section("%s (%d)" % (self.t("Las salas, de la más nueva", "The rooms, the newest first"), r["count"]),
                             inner + "</tbody></table></div>", q, "r_", open_default=True)
        if r["refusals"]:
            requests = {"join": self.t("entrar", "join"), "create": self.t("crear", "create"),
                        "hello": self.t("saludo", "hello"), "list": self.t("lista", "list"),
                        "list-all": self.t("tabla", "table"), "info": "info", "other": self.t("otro", "other")}
            order = self.sorting(q, "x_", ("request", "why", "n"), "n")
            refusals = sorted(r["refusals"], key=lambda x: x[{"request": 0, "why": 1, "n": 2}[order[0]]],
                              reverse=order[1])
            offset, limit, pager = self.pager(h, q, len(refusals), "x_")
            inner = pager + '<div class="card"><table><thead><tr>%s%s%s</tr></thead><tbody>' % (
                self.th(h, q, "x_", self.t("Pedido", "Request"), "request", order),
                self.th(h, q, "x_", self.t("Por qué", "Why"), "why", order),
                self.th(h, q, "x_", self.t("Veces", "Times"), "n", order, "r"))
            for request, why, n in refusals[offset:offset + limit]:
                inner += '<tr><td>%s</td><td>%s</td><td class="r">%d</td></tr>' % (
                    esc(requests.get(request, request)), esc(why_names.get(why, why)), n)
            body += self.section("%s (%d)" % (self.t("Rechazos del servidor", "The server's refusals"),
                                              sum(n for _, _, n in r["refusals"])),
                                 inner + "</tbody></table></div>", q, "x_")
        inner = pagers.get("g_", "") + '<div class="card"><table class="cards"><thead><tr><th>%s</th>%s%s%s%s</tr>' \
                                        '</thead><tbody>' % (
                                            self.t("Consola", "Console"),
                                            self.th(h, q, "g_", self.t("Juego", "Game"), "game", orders["g_"]),
                                            self.th(h, q, "g_", self.t("Salas", "Rooms"), "rooms", orders["g_"], "r"),
                                            self.th(h, q, "g_", self.t("Partidas", "Matches"), "matches", orders["g_"],
                                                    "r"),
                                            self.th(h, q, "g_", self.t("Tiempo", "Time"), "minutes", orders["g_"], "r"))
        for g in r["games"]:
            title = g["title"] or (g["game"].split(".", 1)[-1].replace("c-", "CRC ") if g["game"] else
                                   self.t("(elegido juntos)", "(chosen together)"))
            inner += '<tr><td data-label="%s">%s</td><td data-label="%s">%s</td><td class="r" data-label="%s">%d</td>' \
                     '<td class="r" data-label="%s">%d</td><td class="r" data-label="%s">%s</td></tr>' % (
                        self.t("Consola", "Console"), esc(g["console"] if g["console"] != "*" else "-"),
                        self.t("Juego", "Game"), esc(title), self.t("Salas", "Rooms"), g["rooms"],
                        self.t("Partidas", "Matches"), g["matches"], self.t("Tiempo", "Time"), clock(g["minutes"] * 60))
        if not r["games"]:
            inner += "<tr><td>-</td></tr>"
        body += self.section("%s (%d)" % (self.t("Los juegos más jugados en línea", "The games most played online"),
                                          r["games_total"]), inner + "</tbody></table></div>", q, "g_")
        order = self.sorting(q, "d_", ("date", "rooms", "played", "minutes"), "date")
        days_with = sorted([d for d in r["daily"] if d["rooms"]], key=lambda d: (d[order[0]], d["date"]),
                           reverse=order[1])
        offset, limit, pager = self.pager(h, q, len(days_with), "d_")
        inner = pager + '<div class="card"><table><thead><tr>%s%s%s%s<th class="r">%s</th></tr></thead><tbody>' % (
            self.th(h, q, "d_", self.t("Día", "Day"), "date", order),
            self.th(h, q, "d_", self.t("Salas", "Rooms"), "rooms", order, "r"),
            self.th(h, q, "d_", self.t("Con partida", "With a match"), "played", order, "r"),
            self.th(h, q, "d_", self.t("Tiempo", "Time"), "minutes", order, "r"),
            self.t("Públ./priv./juntos/cable", "Publ./priv./together/link"))
        for d in days_with[offset:offset + limit]:
            inner += '<tr><td>%s</td><td class="r">%d</td><td class="r">%d (%s)</td><td class="r">%s</td>' \
                     '<td class="r">%d/%d/%d/%d</td></tr>' % (esc(d["date"]), d["rooms"], d["played"],
                                                             share(d["played"], d["rooms"]), clock(d["minutes"] * 60),
                                                             d["public"], d["private"], d["together"], d["link"])
        if not days_with:
            inner += "<tr><td>-</td></tr>"
        body += self.section("%s (%d)" % (self.t("Totales por día", "Totals by day"), len(days_with)),
                             inner + "</tbody></table></div>", q, "d_")
        body += '<p class="muted">%s</p>' % self.t(
            "Una fila por sala al cerrarse: sin direcciones, apodos ni códigos. «¿Jugaron?»: Sí cuando el anfitrión "
            "empezó una partida y duró 30 s o más. «Gente»: quiénes entraron, se fueron solos o perdieron la conexión, "
            "y las entradas rechazadas con su motivo (las del anfitrión y los juegos en común llegan con la app 01.04). "
            "Países: IP Geolocation by DB-IP.",
            "A row a room when it closed: no addresses, nicknames or codes. \"Played?\": Yes when the host started a "
            "match that went 30 s or more. \"People\": who joined, left by themselves or lost the connection, and the "
            "joins refused and why (the host's and the games in common come with the 01.04 app). Countries: IP "
            "Geolocation by DB-IP.")
        return self.send(h, 200, self.page(self.t("Salas", "Rooms"), body))

    def chart(self, daily, legend=None):
        """Each day's active installs (bars) and its peak online (green), as an SVG - attributes, no style. (01.04)
        Other numbers in the same two places ("active", "peak") with their own legend: the visits'."""
        if not daily:
            return "-"
        width, height, pad = 720, 200, 24
        most = max(1, max(max(d["active"], d["peak"]) for d in daily))
        step = (width - pad) / float(len(daily))
        out = '<svg class="chart" viewBox="0 0 %d %d" role="img">' % (width, height + 24)
        for i, d in enumerate(daily):
            x = pad + i * step
            bh = (height - 10) * d["active"] / float(most)
            ph = (height - 10) * d["peak"] / float(most)
            out += '<rect class="bar" x="%.1f" y="%.1f" width="%.1f" height="%.1f"><title>%s: %d</title></rect>' % (
                x, height - bh, max(1.0, step * 0.62), bh, esc(d["date"]), d["active"])
            out += '<rect class="bar2" x="%.1f" y="%.1f" width="%.1f" height="%.1f"><title>%s %s: %d</title></rect>' % (
                x + step * 0.64, height - ph, max(1.0, step * 0.22), ph, esc(d["date"]), self.t("pico", "peak"),
                d["peak"])
            last = i == len(daily) - 1
            if len(daily) <= 31 and (i % max(1, len(daily) // 8) == 0 and len(daily) - i > 3 or last):
                out += '<text class="axis" x="%.1f" y="%d"%s>%s</text>' % (
                    width if last else x, height + 19, ' text-anchor="end"' if last else "", esc(d["date"][5:]))
        out += '<text class="axis" x="0" y="16">%d</text></svg>' % most
        return out + '<p class="muted">%s</p>' % (legend or self.t("Barras: instalaciones activas; verde: pico en línea.",
                                                                 "Bars: active installs; green: peak online."))
