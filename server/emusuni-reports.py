#!/usr/bin/env python3
"""EmuSuNi's log reports (00.91): the players' logs, sent from the app (Settings -> Send Log, or Yes after EmuSuNi
closed by itself), kept for the owner under a 6-letter code the app shows to the player.

One program, the Python standard library only (Ubuntu 20.04's Python 3.8), beside emusuni-netd on the same VM:
  serve     HTTPS on TCP 443 to the server's IP, with its own certificate (scripts/server/make-reports-key.sh: an ECDSA
            P-256 key; the app pins its public key, src/LogReport.cpp). POST /report, the app's body (below); the answer
            "code ABCDEF". Run by systemd (emusuni-reports.service, scripts/server/deploy-reports.sh).
  list      the reports kept: code, date (UTC), version, country, files.
  show      a report's files, as text, on stdout (emusuni-reports show ABCDEF [crash.log]).
  export    a report as a tar.gz on stdout (the owner's PC: local/herramientas/scripts-pc/bajar-reporte.py).
  purge     the reports past their days deleted now (serve does it every hour).
  (00.92) The owner's panel in a browser, on the same HTTPS (everything but POST /report): server/reports_panel.py.
  admin-setup    its password and TOTP (asked here, never elsewhere: emusuni-reports admin-setup in a terminal).
  acme issue     a certificate from Let's Encrypt for the server's IP and the reports' key (server/reports_acme.py;
                 --agree-tos the first time, --staging to try), which serve renews by itself: --http-port 80 answers
                 its challenge (and sends the rest to HTTPS). acme status: the certificate's state.
                 (00.95) --name panel.emusuni.com: the panel's name, a certificate of its own for the same key.
serve uses Let's Encrypt's certificate while it is valid and for its key, else its own (--cert): the app pins the key,
so it doesn't see which; a changed certificate is taken within a minute, with no restart. (00.95) With the panel's
name (--public-host panel.emusuni.com,<IP>): a browser asking for the name (SNI) gets the name's certificate; once that
one is valid, the panel and port 80 send the browser from the IP to the name. The app's POST /report to the IP stays as
it was (the IP's certificate, the same key).

Privacy - the same rules as the statistics:
  - The address is never kept nor written: not in a report, not in the journal (no access log; an error never prints
    the peer). It is used, in memory only, for the country (DB-IP's IP to Country Lite, the statistics' file:
    /var/lib/emusuni-netd/dbip-country-lite.csv, "IP Geolocation by DB-IP") and for the reports a day per address
    (a keyed hash - the key random at each start - counted in memory, forgotten at the day's end).
  - The logs lose, before they are written: the addresses in them (IPv4), RetroAchievements' user name, the online
    nicknames (the player's and the others'), the room codes, the PS4's user ids, e-mail addresses. (The app has taken
    out its own nickname and RetroAchievements user already.)
  - Kept 90 days, then deleted.

Limits: a body of 4 MB at most (made smaller: a log of ~0.3 MB is ~30 KB), 24 MB once unpacked, 10 reports a day per
address, 2000 a day in all, 16 at a time, 20 s to send one, 1 GB of reports on disk (the oldest go first).

The app's body: zlib (deflate) of
    EMUSUNI-REPORT 1\n
    version <app version>\n
    file <name> <bytes>\n<the file's bytes>\n        (emusuni.log, emusuni-1.log, crash.log: those there)
    end\n
"""
import argparse
import array
import bisect
import gzip
import hashlib
import hmac
import http.server
import io
import json
import os
import re
import secrets
import shutil
import socket
import ssl
import sys
import tarfile
import threading
import time
import zlib

ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"  # the rooms' (src/NetplayProtocol.h: no 0/O, 1/I/L)
NAMES = ("emusuni.log", "emusuni-1.log", "crash.log")
MAX_BODY = 4 * 1024 * 1024
MAX_UNPACKED = 24 * 1024 * 1024
PER_ADDRESS_DAY = 10
PER_DAY = 2000
AT_ONCE = 16
SEND_SECONDS = 20
DISK_CAP = 1024 * 1024 * 1024
KEEP_DAYS = 90
PLAIN_AT_ONCE, PLAIN_SECONDS = 8, 10  # (00.92) the plain HTTP's (port 80)
HERE = os.path.dirname(os.path.abspath(__file__))


def say(text):
    print(text, flush=True)


# ---- Countries (DB-IP's CSV: "first,last,CC"; IPv4 only) ----
class Countries:
    def __init__(self, path):
        self.path = path
        self.mtime = None
        self.checked = 0.0
        self.firsts = array.array("L")
        self.lasts = array.array("L")
        self.codes = bytearray()  # two letters a range
        self.lock = threading.Lock()

    @staticmethod
    def v4(text):
        parts = text.split(".")
        if len(parts) != 4:
            return None
        value = 0
        for part in parts:
            if not part.isdigit() or int(part) > 255:
                return None
            value = value * 256 + int(part)
        return value

    def load(self):
        now = time.time()
        if now - self.checked < 3600 and self.mtime is not None:
            return
        self.checked = now
        try:
            mtime = os.stat(self.path).st_mtime
        except OSError:
            return
        if mtime == self.mtime:
            return
        firsts, lasts, codes = array.array("L"), array.array("L"), bytearray()
        try:
            with open(self.path, encoding="ascii", errors="replace") as f:
                for line in f:
                    fields = line.rstrip("\n").split(",")
                    if len(fields) < 3:
                        continue
                    first, last = self.v4(fields[0]), self.v4(fields[1])
                    if first is None or last is None:
                        continue
                    firsts.append(first)
                    lasts.append(last)
                    codes += (fields[2][:2].upper() + "??")[:2].encode("ascii", "replace")
        except OSError:
            return
        self.firsts, self.lasts, self.codes, self.mtime = firsts, lasts, codes, mtime
        say("countries: %d IPv4 ranges" % len(firsts))

    def of(self, address):
        with self.lock:
            self.load()
            value = self.v4(address)
            if value is None or not self.codes:
                return "??"
            i = bisect.bisect_right(self.firsts, value) - 1
            if i >= 0 and value <= self.lasts[i]:
                return self.codes[2 * i:2 * i + 2].decode("ascii", "replace")
            return "??"


# ---- What a log loses before it is kept ----
IPV4 = re.compile(r"(?<![\w.])(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}"
                  r"(?::\d{1,5})?(?![\w.])")
EMAIL = re.compile(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+")
USER_ID = re.compile(r"\buser (\d{3,})")
ROOM = re.compile(r"\broom ([%s]{6})\b" % ALPHABET)
# Lines that name RetroAchievements' user (rcheevos' and the app's).
RA_USER = [
    re.compile(r"ra: Attempting to log in (\S+)"),
    re.compile(r"ra: (\S+) logged in successfully"),
    re.compile(r"ra: logged in as (.+?) \(\d+ points"),
    re.compile(r"ra: logging in as (\S+)"),
]
# Lines that name an online nickname (src/Netplay.cpp, Menu.cpp, IngameMenu.cpp, Emulator.cpp).
NICK = [
    re.compile(r"netplay: nickname \"(.+)\"$"),
    re.compile(r"netplay: joining room \S+ (?:from the public list )?as (.+?)(?: \(|$)"),
    re.compile(r"netplay: (.+?) \([^)]*\) \S.*\(\d+ players? on that console"),
    re.compile(r"online: (.+?) asks to join our room"),
    re.compile(r"menu: (.+?) accepted in our room"),
    re.compile(r"netplay: (.+?) (?:accepted us in the room|accepted in the room|not accepted|removed from the room|"
               r"started the game|suggests a game|ended the match|has loaded the state|says frame)"),
    re.compile(r"netplay: (.+?)'s (?:request to join expired|games:)"),
    re.compile(r"netplay: (?:the )?direct path to (.+?)(?: \(| stopped)"),
    re.compile(r"netplay: frame \d+ differs from (.+?)'s"),
    re.compile(r"netplay: (.+?) (?:muted|heard again)$"),
]
NOT_NAMES = {"the", "a", "an", "we", "us", "our", "host", "the host", "player", "nobody"}


def scrub(text):
    """The log as it is kept: names, addresses and codes left out (each name everywhere it appears)."""
    ra, nicks = set(), set()
    for line in text.splitlines():
        for pattern in RA_USER:
            m = pattern.search(line)
            if m:
                ra.add(m.group(1))
        for pattern in NICK:
            m = pattern.search(line)
            if m:
                nicks.add(m.group(1).strip().strip('"'))
    for name in sorted(ra, key=len, reverse=True):
        if len(name) >= 2:
            text = text.replace(name, "<ra-user>")
    for name in sorted(nicks, key=len, reverse=True):
        if len(name) >= 2 and name.lower() not in NOT_NAMES and not name.startswith("<"):
            text = re.sub(r"(?<!\w)" + re.escape(name) + r"(?!\w)", "<player>", text)
    text = IPV4.sub("<address>", text)
    text = EMAIL.sub("<e-mail>", text)
    text = USER_ID.sub("user <id>", text)
    text = ROOM.sub("room <code>", text)
    return text


# ---- The app's body ----
def unpack(body):
    """(version, {name: text}) of the app's body; ValueError when it isn't one."""
    inflater = zlib.decompressobj()
    data = inflater.decompress(body, MAX_UNPACKED + 1)
    if len(data) > MAX_UNPACKED or inflater.unconsumed_tail:
        raise ValueError("too big unpacked")
    stream = io.BytesIO(data)
    if stream.readline() != b"EMUSUNI-REPORT 1\n":
        raise ValueError("not a report")
    version, files = "?", {}
    while True:
        line = stream.readline()
        if not line:
            raise ValueError("cut")
        words = line.decode("ascii", "replace").rstrip("\n").split(" ")
        if words[0] == "end":
            break
        if words[0] == "version" and len(words) == 2:
            version = re.sub(r"[^0-9A-Za-z._-]", "", words[1])[:16] or "?"
        elif words[0] == "file" and len(words) == 3 and words[1] in NAMES and words[2].isdigit():
            size = int(words[2])
            content = stream.read(size)
            if len(content) != size or stream.read(1) != b"\n":
                raise ValueError("cut")
            files[words[1]] = content.decode("utf-8", "replace")
        else:
            raise ValueError("bad line")
    if not files:
        raise ValueError("no files")
    return version, files


# ---- The reports on disk: <dir>/<CODE>/meta.json and <name>.gz ----
class Store:
    def __init__(self, folder):
        self.folder = folder
        self.lock = threading.Lock()
        os.makedirs(folder, exist_ok=True)

    def codes(self):
        try:
            return sorted(c for c in os.listdir(self.folder) if len(c) == 6 and all(ch in ALPHABET for ch in c))
        except OSError:
            return []

    def meta(self, code):
        try:
            with open(os.path.join(self.folder, code, "meta.json"), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def used(self):
        total = 0
        for root, _, files in os.walk(self.folder):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(root, f))
                except OSError:
                    pass
        return total

    def purge(self, now=None):
        """Reports older than KEEP_DAYS deleted, then the oldest while over the disk's cap. How many went."""
        now = now if now is not None else time.time()
        gone = 0
        with self.lock:
            dated = []
            for code in self.codes():
                meta = self.meta(code)
                when = meta.get("time", 0) if meta else 0
                if not meta or now - when > KEEP_DAYS * 86400:
                    shutil.rmtree(os.path.join(self.folder, code), ignore_errors=True)
                    gone += 1
                else:
                    dated.append((when, code))
            dated.sort()
            used = self.used()
            while dated and used > DISK_CAP:
                _, code = dated.pop(0)
                path = os.path.join(self.folder, code)
                size = sum(os.path.getsize(os.path.join(path, f)) for f in os.listdir(path))
                shutil.rmtree(path, ignore_errors=True)
                used -= size
                gone += 1
        return gone

    def save(self, version, country, files):
        """The report kept under a new code (its files scrubbed and gzipped). The code."""
        with self.lock:
            for _ in range(100):
                code = "".join(secrets.choice(ALPHABET) for _ in range(6))
                path = os.path.join(self.folder, code)
                try:
                    os.mkdir(path, 0o750)
                    break
                except FileExistsError:
                    continue
            else:
                raise OSError("no free code")
        sizes = {}
        for name, text in files.items():
            data = scrub(text).encode("utf-8")
            sizes[name] = len(data)
            with gzip.open(os.path.join(path, name + ".gz"), "wb", 9) as f:
                f.write(data)
        meta = {"code": code, "time": int(time.time()), "version": version, "country": country, "files": sizes}
        with open(os.path.join(path, "meta.json.part"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
        os.replace(os.path.join(path, "meta.json.part"), os.path.join(path, "meta.json"))
        return code


# ---- The HTTPS server ----
class Limits:
    """Reports a day: per address (a keyed hash in memory, never the address) and in all."""

    def __init__(self):
        self.key = secrets.token_bytes(32)
        self.day = None
        self.per = {}
        self.all = 0
        self.lock = threading.Lock()

    def take(self, address):
        with self.lock:
            day = time.strftime("%Y-%m-%d", time.gmtime())
            if day != self.day:
                self.day, self.per, self.all = day, {}, 0
                self.key = secrets.token_bytes(32)
            who = hmac.new(self.key, address.encode(), hashlib.sha256).digest()[:16]
            if self.all >= PER_DAY or self.per.get(who, 0) >= PER_ADDRESS_DAY:
                return False
            self.per[who] = self.per.get(who, 0) + 1
            self.all += 1
            return True


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "EmuSuNi"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # (no access log: it would have the address)

    def answer(self, status, text):
        body = (text + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        # (00.92) The owner's panel.
        if self.server.panel:
            return self.server.panel.handle(self, "GET")
        self.answer(404, "not here")

    def do_POST(self):
        srv = self.server
        if self.path != "/report":
            if srv.panel:
                return srv.panel.handle(self, "POST")
            return self.answer(404, "not here")
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if length < 0:
            return self.answer(411, "length needed")
        if length > MAX_BODY:
            return self.answer(413, "too big")
        # (The body read before any answer: a client still sending would see the connection cut, not the answer.)
        body = self.rfile.read(length)
        if len(body) != length:
            return
        if not srv.limits.take(self.client_address[0]):
            return self.answer(429, "too many today")
        try:
            version, files = unpack(body)
        except (ValueError, zlib.error):
            return self.answer(400, "not a report")
        srv.store.purge()
        if srv.store.used() > DISK_CAP:
            return self.answer(507, "full")
        country = srv.countries.of(self.client_address[0])
        try:
            code = srv.store.save(version, country, files)
        except OSError as e:
            say("report not kept: %s" % type(e).__name__)
            return self.answer(500, "not kept")
        say("report %s: version %s, %s, %d file(s), %d KB sent" % (code, version, country, len(files), length // 1024))
        self.answer(200, "code " + code)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, bind, port, context, store, countries):
        self.context, self.store, self.countries = context, store, countries
        self.panel = None
        self.limits = Limits()
        self.slots = threading.BoundedSemaphore(AT_ONCE)
        super().__init__((bind, port), Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def finish_request(self, request, client_address):
        request.settimeout(SEND_SECONDS)
        try:
            tls = self.context.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return  # (a scan, a wrong client, a timeout: nothing said - it would name the address)
        try:
            Handler(tls, client_address, self)
        finally:
            try:
                tls.close()
            except OSError:
                pass

    def handle_error(self, request, client_address):
        say("a request failed: %s" % type(sys.exc_info()[1]).__name__)  # (never the address)


def modules():
    """server/reports_panel.py and reports_acme.py, beside this file (python3 -I leaves its folder out of the path)."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import reports_acme
    import reports_panel
    return reports_acme, reports_panel


# ---- (00.92) The certificate: Let's Encrypt's while it is valid and for the key, else the own one ----
# (00.95) And a name's (the panel's, panel.emusuni.com): its own certificate for the same key, given to a browser that
# asks for that name (SNI) while it is valid; the app (the IP, no name) gets the IP's as before.
class Certs:
    def __init__(self, own_cert, key, acme_dir, names=()):
        self.acme, _ = modules()
        self.own, self.key, self.acme_dir = own_cert, key, acme_dir
        self.le = os.path.join(acme_dir, "cert.pem")
        self.spki = self.acme.public_key(key)
        self.lock = threading.Lock()
        self.chosen = None  # (path, mtime)
        self.expiry, self.problem = 0, "none"
        self.names = list(names)
        self.by_name = {}  # name -> {"chosen": mtime, "context", "expiry", "problem"}
        for name in self.names:
            self.by_name[name] = {"chosen": None, "context": None, "expiry": 0, "problem": "none"}

    def context(self, cert, sni=True):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert, self.key)
        if sni:
            context.sni_callback = self.pick
        return context

    def pick(self, tls, server_name, context):
        """(00.95) The TLS hello's name: its certificate's context when it has a valid one."""
        entry = self.by_name.get((server_name or "").lower())
        chosen = entry and entry["context"]
        if chosen:
            tls.context = chosen
        return None

    def check(self):
        expiry, problem = self.acme.check_cert(self.le, self.key, spki=self.spki)
        with self.lock:
            self.expiry, self.problem = expiry, problem
        return expiry, problem

    def status(self, host=None):
        """(expiry, problem) of the IP's certificate, or of the name's when host is one of the names."""
        with self.lock:
            entry = self.by_name.get((host or "").lower())
            if entry:
                return entry["expiry"], entry["problem"]
            return self.expiry, self.problem

    def ready(self, name):
        """(00.95) The name has a valid certificate of ours now (the panel sends the browser to it then)."""
        with self.lock:
            entry = self.by_name.get(name)
            return bool(entry and entry["context"] and not entry["problem"])

    def refresh(self, server):
        """The context for the certificate to use now, made again when the choice or its file changed."""
        for name in self.names:
            self.refresh_name(name)
        _, problem = self.check()
        path = self.own if problem else self.le
        try:
            chosen = (path, os.stat(path).st_mtime)
        except OSError:
            return
        if chosen == self.chosen:
            return
        try:
            server.context = self.context(path)
        except (ssl.SSLError, OSError) as e:
            say("tls: %s not usable (%s): the one before stays" % (path, type(e).__name__))
            return
        self.chosen = chosen
        if problem:
            say("tls: the own certificate (Let's Encrypt's: %s)" % problem)
        else:
            say("tls: Let's Encrypt's certificate, valid until %s UTC" %
                time.strftime("%Y-%m-%d %H:%M", time.gmtime(self.expiry)))

    def refresh_name(self, name):
        path = self.acme.cert_file(self.acme_dir, name)
        expiry, problem = self.acme.check_cert(path, self.key, spki=self.spki)
        entry = self.by_name[name]
        with self.lock:
            entry["expiry"], entry["problem"] = expiry, problem
        if problem:
            if entry["context"]:
                say("tls: %s's certificate %s: the IP's for it" % (name, problem))
            with self.lock:
                entry["context"], entry["chosen"] = None, None
            return
        try:
            chosen = os.stat(path).st_mtime
        except OSError:
            return
        if chosen == entry["chosen"]:
            return
        try:
            context = self.context(path, sni=False)
        except (ssl.SSLError, OSError) as e:
            say("tls: %s not usable (%s): the one before stays" % (path, type(e).__name__))
            return
        with self.lock:
            entry["context"], entry["chosen"] = context, chosen
        say("tls: %s's certificate (Let's Encrypt), valid until %s UTC" %
            (name, time.strftime("%Y-%m-%d %H:%M", time.gmtime(expiry))))


# ---- (00.92) Plain HTTP (port 80): ACME's http-01 challenge; anything else to HTTPS ----
class PlainHandler(http.server.BaseHTTPRequestHandler):
    server_version = "EmuSuNi"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # (no access log: it would have the address)

    def reply(self, status, body, headers=None):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        prefix = "/.well-known/acme-challenge/"
        if path.startswith(prefix):
            token = path[len(prefix):]
            body = None
            if re.fullmatch(r"[A-Za-z0-9_-]{1,128}", token):
                try:
                    with open(os.path.join(self.server.challenges, token), "rb") as f:
                        body = f.read(512)
                except OSError:
                    pass
            return self.reply(200, body) if body else self.reply(404, b"not here\n")
        # (The configured address, never the request's Host: no redirect elsewhere. 00.95: the panel's name once it
        # has its certificate, else the IP.)
        safe = path if re.fullmatch(r"/[A-Za-z0-9/_.-]*", path) else "/"
        self.reply(301, b"", {"Location": "https://%s%s" % (self.server.host(), safe)})


class PlainServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, bind, port, host, challenges):
        self.host, self.challenges = host, challenges  # (host: a function, the address to send to now)
        self.slots = threading.BoundedSemaphore(PLAIN_AT_ONCE)
        super().__init__((bind, port), PlainHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def finish_request(self, request, client_address):
        request.settimeout(PLAIN_SECONDS)
        try:
            PlainHandler(request, client_address, self)
        except OSError:
            pass

    def handle_error(self, request, client_address):
        say("a plain request failed: %s" % type(sys.exc_info()[1]).__name__)  # (never the address)


def serve(args):
    store = Store(args.dir)
    countries = Countries(args.geoip)
    acme_dir = args.acme_dir or os.path.join(args.dir, "acme")
    os.makedirs(os.path.join(acme_dir, "challenges"), exist_ok=True)
    acme, panel = modules()
    # (00.95) The panel's addresses: its name first (panel.emusuni.com), then the IP - "name,IP".
    hosts = [h.strip().lower() for h in args.public_host.split(",") if h.strip()]

    def is_name(host):
        try:
            return acme.identifier(host)[0] == "dns"
        except acme.AcmeError:
            return False  # (an address with its port: a test's)

    names = [h for h in hosts if is_name(h)]
    certs = Certs(args.cert, args.key, acme_dir, names)
    server = Server(args.bind, args.port, certs.context(args.cert), store, countries)
    certs.refresh(server)

    def canonical():
        """The panel's address to send a browser to: its name once its certificate is valid (None: any of them)."""
        return names[0] if names and certs.ready(names[0]) else None

    if hosts:
        server.panel = panel.Panel(store, args.dir, ",".join(hosts), args.stats_dir, args.titles, args.repo,
                                   certs.status, say, canonical=canonical)

    def hourly():
        while True:
            gone = store.purge()
            if gone:
                say("purge: %d report(s) deleted" % gone)
            time.sleep(3600)

    def certificates():
        # Every minute the certificates to use; every 6 hours (each hour after a failure) a new one when due - (00.95)
        # each one's by itself: the IP's, each name's.
        due = {}  # identity -> when to look at it again
        while True:
            time.sleep(60)
            certs.refresh(server)
            account = acme.saved_account(acme_dir)
            if not account:
                continue
            for identity in acme.identities(account):
                if time.time() < due.get(identity, 0):
                    continue
                expiry, problem = acme.check_cert(acme.cert_file(acme_dir, identity), args.key, spki=certs.spki)
                due[identity] = time.time() + 6 * 3600
                if not problem and expiry - time.time() > acme.RENEW_BEFORE:
                    continue
                try:
                    acme.Client(acme_dir, account["directory"], say).issue(identity, args.key,
                                                                         acme.cert_file(acme_dir, identity))
                    certs.refresh(server)
                except (acme.AcmeError, OSError, ValueError, KeyError) as e:
                    say("acme: no new certificate for %s now (%s): again in an hour" % (identity, e))
                    due[identity] = time.time() + 3600

    threading.Thread(target=hourly, daemon=True).start()
    threading.Thread(target=certificates, daemon=True).start()
    if args.http_port:
        fallback = hosts[-1] if hosts else args.bind
        plain = PlainServer(args.bind, args.http_port, lambda: canonical() or fallback,
                            os.path.join(acme_dir, "challenges"))
        threading.Thread(target=plain.serve_forever, daemon=True).start()
    say("emusuni-reports: HTTPS on port %d%s%s, %d report(s) kept in %s" % (
        args.port, ", HTTP on port %d (ACME, to HTTPS)" % args.http_port if args.http_port else "",
        ", the panel for %s" % " and ".join(hosts) if hosts else "", len(store.codes()), args.dir))
    server.serve_forever()


# ---- The owner's commands ----
def find(store, code):
    code = code.strip().upper()
    if code not in store.codes():
        sys.exit("no report %s" % code)
    return code


def cmd_list(args):
    store = Store(args.dir)
    rows = []
    for code in store.codes():
        meta = store.meta(code) or {}
        rows.append((meta.get("time", 0), code, meta))
    for when, code, meta in sorted(rows):
        if args.days and time.time() - when > args.days * 86400:
            continue
        files = ", ".join("%s %d KB" % (n, s // 1024) for n, s in sorted(meta.get("files", {}).items()))
        say("%s  %s UTC  %-6s  %s  %s" % (code, time.strftime("%Y-%m-%d %H:%M", time.gmtime(when)),
                                          meta.get("version", "?"), meta.get("country", "??"), files))
    say("%d report(s), %.1f MB" % (len(rows), store.used() / 1e6))


def cmd_show(args):
    store = Store(args.dir)
    code = find(store, args.code)
    names = [args.file] if args.file else [n for n in NAMES if os.path.exists(os.path.join(args.dir, code, n + ".gz"))]
    for name in names:
        path = os.path.join(args.dir, code, name + ".gz")
        if not os.path.exists(path):
            sys.exit("%s has no %s" % (code, name))
        say("==== %s %s ====" % (code, name))
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
            sys.stdout.write(f.read())
        sys.stdout.flush()


def cmd_export(args):
    store = Store(args.dir)
    code = find(store, args.code)
    with tarfile.open(fileobj=sys.stdout.buffer, mode="w|gz") as tar:
        tar.add(os.path.join(args.dir, code), arcname=code)


def cmd_purge(args):
    say("%d report(s) deleted" % Store(args.dir).purge())


def cmd_admin_setup(args):
    _, panel = modules()
    sys.exit(panel.admin_setup(os.path.join(args.dir, "panel")))


def cmd_acme(args):
    acme, _ = modules()
    folder = args.acme_dir or os.path.join(args.dir, "acme")
    account = acme.saved_account(folder)
    if args.action == "status":
        # (00.95) Each certificate: the IP's, each name's.
        found = acme.identities(account) if account else []
        say("account: %s" % ("%s, for %s" % (account["directory"], ", ".join(found)) if account else "none yet"))
        for identity in found or [args.ip or "the IP"]:
            path = acme.cert_file(folder, identity) if found else os.path.join(folder, "cert.pem")
            expiry, problem = acme.check_cert(path, args.key)
            say("certificate for %s: %s" % (identity, "valid until %s UTC" % time.strftime(
                "%Y-%m-%d %H:%M", time.gmtime(expiry)) if not problem else problem))
        return
    if args.action == "renew" and not account:
        sys.exit("no certificate asked for yet: emusuni-reports acme issue --ip ADDRESS --agree-tos")
    directory = args.directory or (acme.STAGING if args.staging else
                                   account["directory"] if args.action == "renew" else acme.PRODUCTION)
    # issue: the IP (--ip, else the account's) or (00.95) a name (--name); renew: those asked, else every one.
    wanted = [v for v in (args.ip, args.name) if v]
    if not wanted:
        wanted = acme.identities(account) if args.action == "renew" else [(account or {}).get("ip", "")]
    wanted = [v for v in wanted if v]
    if not wanted:
        sys.exit("--ip: the server's public address (or --name: the panel's name)")
    failed = False
    for value in wanted:
        try:
            acme.Client(folder, directory, say).issue(value, args.key, acme.cert_file(folder, value),
                                                      agree=args.agree_tos)
        except acme.AcmeError as e:
            say("acme: %s: %s" % (value, e))
            failed = True
    if failed:
        sys.exit(1)
    say("emusuni-reports serve takes it within a minute; it renews it by itself.")


def main():
    parser = argparse.ArgumentParser(description="EmuSuNi's log reports")
    parser.add_argument("--dir", default="/var/lib/emusuni-reports")
    sub = parser.add_subparsers(dest="command")
    p = sub.add_parser("serve")
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=443)
    p.add_argument("--cert", default="/etc/emusuni-reports/cert.pem")
    p.add_argument("--key", default="/etc/emusuni-reports/key.pem")
    p.add_argument("--geoip", default="/var/lib/emusuni-netd/dbip-country-lite.csv")
    # (00.92) The owner's panel (its address: the browser's - the Origin its forms must come from), plain HTTP for
    # ACME, the statistics read.
    p.add_argument("--public-host", default="", help="the panel's address: the server's public IP, or (00.95) its "
                   "name and the IP, \"panel.emusuni.com,132.145.164.206\" (none: no panel)")
    p.add_argument("--http-port", type=int, default=0, help="plain HTTP: ACME's challenge, the rest to HTTPS")
    p.add_argument("--acme-dir", default="")
    p.add_argument("--stats-dir", default="/var/lib/emusuni-netd/stats")
    p.add_argument("--titles", default="/var/lib/emusuni-netd/titles.txt")
    p.add_argument("--repo", default="arturofuentes1988/EmuSuNi")
    p = sub.add_parser("list")
    p.add_argument("--days", type=int, default=0)
    p = sub.add_parser("show")
    p.add_argument("code")
    p.add_argument("file", nargs="?", choices=NAMES)
    p = sub.add_parser("export")
    p.add_argument("code")
    sub.add_parser("purge")
    sub.add_parser("admin-setup")
    p = sub.add_parser("acme")
    p.add_argument("action", choices=("issue", "renew", "status"))
    p.add_argument("--ip", default="")
    p.add_argument("--name", default="", help="(00.95) a name's certificate (the panel's: panel.emusuni.com)")
    p.add_argument("--staging", action="store_true", help="Let's Encrypt's staging (a certificate no browser trusts)")
    p.add_argument("--directory", default="", help="another ACME server's directory URL (tests)")
    p.add_argument("--agree-tos", action="store_true", help="accept Let's Encrypt's terms (the account's first time)")
    p.add_argument("--acme-dir", default="")
    p.add_argument("--key", default="/etc/emusuni-reports/key.pem")
    args = parser.parse_args()
    commands = {"serve": serve, "list": cmd_list, "show": cmd_show, "export": cmd_export, "purge": cmd_purge,
                "admin-setup": cmd_admin_setup, "acme": cmd_acme}
    if args.command not in commands:
        parser.print_help()
        sys.exit(2)
    commands[args.command](args)


if __name__ == "__main__":
    main()
