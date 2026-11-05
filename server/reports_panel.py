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
GITHUB_SECONDS = 3600
SKETCH_ALL, SKETCH_LABEL, SKETCH_GAME = 14, 11, 8
LABEL_KINDS = ("country", "version", "language", "state", "console", "game")
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
--warn:#ffc870;--bad:#ff8a80;--mark:#5a4a00}
@media (prefers-color-scheme:light){:root{--bg:#f4f5f8;--card:#fff;--line:#d9dce4;--text:#1c1f26;--dim:#5d6372;
--accent:#5b3fd1;--good:#1e7f46;--warn:#9a5c00;--bad:#b3261e;--mark:#ffe58a}}
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
th,td{text-align:left;padding:8px 6px;border-bottom:1px solid var(--line);vertical-align:top}
th{font-size:13px;color:var(--dim);font-weight:600}
td.r,th.r{text-align:right}
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
        self.github = GitHub(repo, self.state_dir, say)
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
            links = ('<a href="/reports">%s</a><a href="/stats">%s</a>'
                     '<form method="post" action="/logout"><input type="hidden" name="csrf" value="%s">'
                     '<button class="plain" type="submit">%s</button></form>' %
                     (self.t("Reportes", "Reports"), self.t("Estadísticas", "Statistics"), esc(self.csrf),
                      self.t("Salir", "Log out")))
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
                (self.lang, esc(title), self.css_tag, links, body, cert, other,
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
        try:
            page = max(1, int(q("page", "1")))
        except ValueError:
            page = 1
        pages = max(1, (len(shown) + LIST_PAGE - 1) // LIST_PAGE)
        page = min(page, pages)

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
        body += '<p class="muted">%s</p>' % (self.t("%d de %d reportes; %d nuevos.", "%d of %d reports; %d new.") %
                                            (len(shown), len(rows), new))
        if shown:
            body += ('<table class="cards"><thead><tr><th>%s</th><th>%s</th><th>%s</th><th>%s</th><th>%s</th>'
                     '<th>%s</th><th class="r">%s</th><th>%s</th></tr></thead><tbody>' %
                     (self.t("Código", "Code"), self.t("Fecha (UTC)", "Date (UTC)"), self.t("Versión", "Version"),
                      self.t("País", "Country"), self.t("Consola", "Console"), self.t("Juego", "Game"),
                      self.t("Tamaño", "Size"), self.t("Estado", "Status")))
            for r in shown[(page - 1) * LIST_PAGE:page * LIST_PAGE]:
                st = r["panel"]["status"]
                body += ('<tr><td data-label="%s"><a href="/r/%s">%s</a></td><td data-label="%s">%s</td>'
                         '<td data-label="%s">%s</td><td data-label="%s">%s</td><td data-label="%s">%s</td>'
                         '<td data-label="%s">%s</td><td class="r" data-label="%s">%s</td><td data-label="%s">'
                         '<span class="tag %s">%s</span>%s</td></tr>' %
                         (self.t("Código", "Code"), r["code"], r["code"], self.t("Fecha", "Date"), stamp(r.get("time")),
                          self.t("Versión", "Version"), esc(r.get("version", "?")), self.t("País", "Country"),
                          esc(r.get("country", "??")), self.t("Consola", "Console"), esc(r.get("console") or "-"),
                          self.t("Juego", "Game"), esc(r.get("game") or "-"), self.t("Tamaño", "Size"),
                          kb(r["size"]), self.t("Estado", "Status"), st, esc(self.status_name(st)),
                          ' <span class="tag crash">crash.log</span>' if r["crash"] else ""))
            body += "</tbody></table>"
        else:
            body += '<p class="card">%s</p>' % self.t("Ningún reporte con estos filtros.", "No report with these filters.")
        if pages > 1:
            body += '<div class="pages">'
            for p in range(1, pages + 1):
                if p == page:
                    body += "<span>%d</span>" % p
                else:
                    args = dict((k, v[0]) for k, v in urllib.parse.parse_qs(urllib.parse.urlsplit(h.path).query).items())
                    args["page"] = str(p)
                    body += '<a href="/reports?%s">%d</a>' % (esc(urllib.parse.urlencode(args)), p)
            body += "</div>"
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
                     "game": self.t("Juegos (los 100 primeros)", "Games (the top 100)")}
            for kind in ("version", "country", "console", "game", "state", "language"):
                values = s["by"].get(kind, [])
                if kind == "game":
                    values = sorted(values, key=lambda v: -v["hours"])[:100]
                hours = kind in ("console", "state", "game")
                # (On a phone only the games' table - 4 columns - becomes cards.)
                body += '<h2>%s</h2><div class="card"><table%s><thead><tr><th>%s</th>%s<th class="r">' \
                        '%s</th>%s</tr></thead><tbody>' % (
                            names[kind], ' class="cards"' if kind == "game" else "", self.t("Valor", "Value"),
                            "<th>%s</th>" % self.t("Título", "Title") if kind == "game" else "",
                            self.t("Instalaciones", "Installs"),
                            '<th class="r">%s</th>' % self.t("Horas", "Hours") if hours else "")
                for v in values:
                    # (00.95) A version not published on GitHub: a test one (not on the public page).
                    test = kind == "version" and v["value"] not in s.get("published", set())
                    body += '<tr><td data-label="%s">%s</td>%s<td class="r" data-label="%s">%d</td>%s</tr>' % (
                        names[kind], esc(v["value"]) + (' <span class="muted">(%s)</span>' %
                                                        self.t("prueba", "test") if test else ""),
                        '<td data-label="%s">%s</td>' % (self.t("Título", "Title"), esc(v["title"] or "-"))
                        if kind == "game" else "", self.t("Instalaciones", "Installs"), v["installs"],
                        '<td class="r" data-label="%s">%.1f</td>' % (self.t("Horas", "Hours"), v["hours"])
                        if hours else "")
                if not values:
                    body += "<tr><td>-</td></tr>"
                body += "</tbody></table></div>"
            if s["updates"]:
                body += '<h2>%s</h2><div class="card"><table><tbody>' % self.t("Actualizaciones instaladas",
                                                                              "Updates installed")
                for u in s["updates"]:
                    body += '<tr><td>%s &rarr; %s</td><td class="r">%d</td></tr>' % (esc(u["from"]), esc(u["to"]),
                                                                                   u["installs"])
                body += "</tbody></table></div>"
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

    def chart(self, daily):
        """Each day's active installs (bars) and its peak online (green), as an SVG - attributes, no style."""
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
        return out + '<p class="muted">%s</p>' % self.t("Barras: instalaciones activas; verde: pico en línea.",
                                                       "Bars: active installs; green: peak online.")
