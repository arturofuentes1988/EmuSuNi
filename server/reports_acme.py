"""The log reports' certificate from Let's Encrypt (00.92), for the owner's panel in a browser (server/reports_panel.py).

ACME (RFC 8555) for the server's IP address (RFC 8738): Let's Encrypt's "shortlived" profile (160 hours - the only one
it gives an IP), the http-01 challenge on TCP 80 (emusuni-reports serve answers it there: the token's file in
<acme dir>/challenges). The certificate is made for the reports' own key (/etc/emusuni-reports/key.pem, a CSR signed with
it): the app pins that key, not a certificate (src/LogReport.cpp), and doesn't look at the dates - it sees no change, and
a renewal that fails leaves it working (only a browser would complain).

Python's standard library and the system's openssl command (Python has no ECDSA: the signatures and the account's key
are openssl's; the CSR and the certificate's dates are read and made here, in DER). Kept in <acme dir>: account.pem (the
account's key), account.json (its URL, the directory, the IP, the names), cert.pem (the IP's certificate and its chain).
(00.95) A name too - the panel's, panel.emusuni.com -: a certificate of its own for the same key (cert-<name>.pem),
asked for and renewed apart from the IP's (one that fails leaves the other); the server gives a browser the one of the
name it asks for (SNI), the app the IP's.
"""
import base64
import calendar
import hashlib
import json
import os
import re
import socket
import subprocess
import time
import urllib.error
import urllib.request

PRODUCTION = "https://acme-v02.api.letsencrypt.org/directory"
STAGING = "https://acme-staging-v02.api.letsencrypt.org/directory"
PROFILE = "shortlived"
RENEW_BEFORE = 72 * 3600  # s left of a certificate: a new one (the shortlived ones last 160 h)
USER_AGENT = "emusuni-reports-acme/1"


class AcmeError(Exception):
    pass


def b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def openssl(args, data=None):
    try:
        r = subprocess.run(["openssl"] + args, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise AcmeError("openssl %s: %s" % (args[0], e))
    if r.returncode != 0:
        raise AcmeError("openssl %s: %s" % (args[0], r.stderr.decode("utf-8", "replace").strip()[:300]))
    return r.stdout


# ---- DER: enough for a CSR, and a certificate's dates and key ----
def der(tag, content):
    n = len(content)
    if n < 0x80:
        length = bytes([n])
    else:
        raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
        length = bytes([0x80 | len(raw)]) + raw
    return bytes([tag]) + length + content


def der_seq(*parts):
    return der(0x30, b"".join(parts))


def der_int(n):
    raw = n.to_bytes(max(1, (n.bit_length() + 8) // 8), "big")
    return der(0x02, raw)


def der_oid(dotted):
    parts = [int(p) for p in dotted.split(".")]
    out = bytearray([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        p >>= 7
        while p:
            chunk.append(0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(reversed(chunk))
    return der(0x06, bytes(out))


def der_items(data):
    """The (tag, content, whole) of each item one after another in `data`."""
    items, at = [], 0
    while at < len(data):
        tag = data[at]
        n = data[at + 1]
        head = 2
        if n & 0x80:
            count = n & 0x7F
            n = int.from_bytes(data[at + 2:at + 2 + count], "big")
            head += count
        content = data[at + head:at + head + n]
        if len(content) != n:
            raise AcmeError("a cut DER item")
        items.append((tag, content, data[at:at + head + n]))
        at += head + n
    return items


def public_key(key_path):
    """The key's SubjectPublicKeyInfo (DER)."""
    return openssl(["pkey", "-in", key_path, "-pubout", "-outform", "DER"])


def sign(key_path, data):
    """An ECDSA P-256 signature of SHA-256(data), as DER (ECDSA-Sig-Value)."""
    return openssl(["dgst", "-sha256", "-sign", key_path], data)


def raw_signature(sig_der):
    """r and s, 32 bytes each (the JWS ES256 form) of a DER ECDSA-Sig-Value."""
    (tag, body, _), = der_items(sig_der)
    r, s = [content for _, content, _ in der_items(body)]
    return int.from_bytes(r, "big").to_bytes(32, "big") + int.from_bytes(s, "big").to_bytes(32, "big")


def ip_bytes(ip):
    try:
        return socket.inet_pton(socket.AF_INET, ip)
    except OSError:
        return socket.inet_pton(socket.AF_INET6, ip)


def identifier(value):
    """(00.95) ACME's identifier of an IP address or a name: ("ip", "132.145.164.206"), ("dns", "panel.emusuni.com")."""
    value = value.strip().lower().rstrip(".")
    try:
        ip_bytes(value)
        return "ip", value
    except OSError:
        pass
    labels = value.split(".")
    if len(value) <= 253 and len(labels) >= 2 and not labels[-1].isdigit() and \
            all(re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels):
        return "dns", value
    raise AcmeError("%r is neither an IP address nor a name" % value)


def cert_file(folder, value):
    """(00.95) Where the certificate of an IP address or a name is kept: the IP's cert.pem (as before), a name's
    cert-<name>.pem."""
    kind, value = identifier(value)
    return os.path.join(folder, "cert.pem" if kind == "ip" else "cert-%s.pem" % value)


def make_csr(key_path, ip):
    """A CSR (DER) for the IP address (00.95: or the name) alone - no subject, its subjectAltName - signed with the
    key."""
    kind, value = identifier(ip)
    name = der(0x87, ip_bytes(value)) if kind == "ip" else der(0x82, value.encode("ascii"))
    san = der_seq(der_oid("2.5.29.17"), der(0x04, der_seq(name)))
    request = der_seq(der_oid("1.2.840.113549.1.9.14"), der(0x31, der_seq(san)))
    info = der_seq(der_int(0), der_seq(), public_key(key_path), der(0xA0, request))
    return der_seq(info, der_seq(der_oid("1.2.840.10045.4.3.2")), der(0x03, b"\x00" + sign(key_path, info)))


def pem_certs(text):
    """The DER of each certificate in a PEM text."""
    out, inside, lines = [], False, []
    for line in text.splitlines():
        line = line.strip()
        if line == "-----BEGIN CERTIFICATE-----":
            inside, lines = True, []
        elif line == "-----END CERTIFICATE-----" and inside:
            out.append(base64.b64decode("".join(lines)))
            inside = False
        elif inside:
            lines.append(line)
    return out


def der_time(tag, content):
    text = content.decode("ascii")
    if tag == 0x17:  # UTCTime: YYMMDDHHMMSSZ
        year = int(text[0:2])
        text = ("19" if year >= 50 else "20") + text
    return calendar.timegm(time.strptime(text[:14], "%Y%m%d%H%M%S"))


def cert_info(der_cert):
    """(not_before, not_after, SubjectPublicKeyInfo DER) of a certificate."""
    (_, body, _), = der_items(der_cert)
    tbs = der_items(der_items(body)[0][1])
    at = 1 if tbs[0][0] == 0xA0 else 0  # (its version, when there is one)
    validity = der_items(tbs[at + 3][1])
    return der_time(validity[0][0], validity[0][1]), der_time(validity[1][0], validity[1][1]), tbs[at + 5][2]


def check_cert(cert_path, key_path, now=None, spki=None):
    """(not_after, problem) of the certificate file: problem "" when it is for the key and valid now. (spki: the key's
    public part, when known - else read from the key.)"""
    now = now if now is not None else time.time()
    try:
        with open(cert_path, encoding="ascii", errors="replace") as f:
            certs = pem_certs(f.read())
    except OSError:
        return 0, "none"
    if not certs:
        return 0, "no certificate in it"
    try:
        not_before, not_after, its_key = cert_info(certs[0])
    except (AcmeError, ValueError, IndexError):
        return 0, "not readable"
    if its_key != (spki or public_key(key_path)):
        return not_after, "for another key"
    if now < not_before or now >= not_after:
        return not_after, "expired" if now >= not_after else "not valid yet"
    return not_after, ""


# ---- The ACME client ----
class Client:
    def __init__(self, folder, directory, say):
        self.folder, self.directory_url, self.say = folder, directory, say
        os.makedirs(os.path.join(folder, "challenges"), exist_ok=True)
        self.key_path = os.path.join(folder, "account.pem")
        self.nonce = None
        self.kid = None
        self.directory = self.get_json(directory)
        if not os.path.exists(self.key_path):
            old = os.umask(0o077)
            try:
                openssl(["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", self.key_path])
            finally:
                os.umask(old)
        point = public_key(self.key_path)[-65:]  # (P-256's: 04, x, y)
        self.jwk = {"crv": "P-256", "kty": "EC", "x": b64u(point[1:33]), "y": b64u(point[33:65])}
        canonical = json.dumps(self.jwk, separators=(",", ":"), sort_keys=True).encode()
        self.thumbprint = b64u(hashlib.sha256(canonical).digest())

    # HTTP
    def http(self, url, data=None, headers=None):
        req = urllib.request.Request(url, data=data, headers=dict(headers or {}, **{"User-Agent": USER_AGENT}),
                                     method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()
        except (urllib.error.URLError, OSError) as e:
            raise AcmeError("%s: %s" % (url, e))

    def get_json(self, url):
        status, _, body = self.http(url)
        if status != 200:
            raise AcmeError("%s: HTTP %d" % (url, status))
        return json.loads(body.decode("utf-8"))

    def new_nonce(self):
        status, headers, _ = self.http(self.directory["newNonce"])
        nonce = headers.get("Replay-Nonce")
        if not nonce:
            raise AcmeError("no nonce (HTTP %d)" % status)
        return nonce

    def post(self, url, payload):
        """A signed request (payload None: POST-as-GET). (status, headers, body); a bad nonce is tried again."""
        for _ in range(4):
            nonce = self.nonce or self.new_nonce()
            self.nonce = None
            protected = {"alg": "ES256", "nonce": nonce, "url": url}
            if self.kid:
                protected["kid"] = self.kid
            else:
                protected["jwk"] = self.jwk
            head = b64u(json.dumps(protected).encode())
            body = "" if payload is None else b64u(json.dumps(payload).encode())
            signature = raw_signature(sign(self.key_path, (head + "." + body).encode("ascii")))
            data = json.dumps({"protected": head, "payload": body, "signature": b64u(signature)}).encode()
            status, headers, answer = self.http(url, data, {"Content-Type": "application/jose+json"})
            self.nonce = headers.get("Replay-Nonce")
            if status == 400 and b"badNonce" in answer:
                continue
            return status, headers, answer
        raise AcmeError("%s: the nonce refused again and again" % url)

    def post_json(self, url, payload, ok=(200, 201)):
        status, headers, body = self.post(url, payload)
        try:
            answer = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            answer = {}
        if status not in ok:
            raise AcmeError("%s: HTTP %d %s" % (url, status, answer.get("detail", answer.get("type", ""))))
        return headers, answer

    # The account
    def account(self, agree):
        saved = os.path.join(self.folder, "account.json")
        try:
            with open(saved, encoding="utf-8") as f:
                state = json.load(f)
            if state.get("directory") == self.directory_url and state.get("kid"):
                self.kid = state["kid"]
                return
        except (OSError, ValueError):
            pass
        terms = self.directory.get("meta", {}).get("termsOfService", "")
        if not agree:
            raise AcmeError("the account is made accepting Let's Encrypt's terms (%s): --agree-tos" % terms)
        headers, _ = self.post_json(self.directory["newAccount"], {"termsOfServiceAgreed": True})
        self.kid = headers.get("Location")
        if not self.kid:
            raise AcmeError("the account has no URL")
        with open(saved + ".part", "w", encoding="utf-8") as f:
            json.dump({"directory": self.directory_url, "kid": self.kid, "terms": terms}, f)
        os.replace(saved + ".part", saved)
        self.say("acme: account made (%s; terms %s)" % (self.directory_url, terms))

    def wait(self, url, done, seconds=180):
        until = time.time() + seconds
        while True:
            headers, answer = self.post_json(url, None, ok=(200,))
            if answer.get("status") in done:
                return answer
            if answer.get("status") == "invalid" or time.time() > until:
                problem = answer.get("error") or [c.get("error") for c in answer.get("challenges", []) if c.get("error")]
                raise AcmeError("%s: %s %s" % (url, answer.get("status"), json.dumps(problem)[:400]))
            try:
                pause = min(10.0, max(1.0, float(headers.get("Retry-After", "2"))))
            except ValueError:
                pause = 2.0
            time.sleep(pause)

    # The certificate
    def issue(self, ip, key_path, cert_path, agree=False):
        """A new certificate for the IP (00.95: or the name) and the key, in cert_path (the chain after it). Its
        expiry."""
        kind, ip = identifier(ip)
        self.account(agree)
        order = {"identifiers": [{"type": kind, "value": ip}]}
        # (The shortlived profile, 160 hours, for a name too: both renewed the same way.)
        profiles = self.directory.get("meta", {}).get("profiles", {})
        if PROFILE in profiles:
            order["profile"] = PROFILE
        headers, answer = self.post_json(self.directory["newOrder"], order)
        order_url = headers.get("Location")
        self.say("acme: order for %s%s" % (ip, " (%s)" % PROFILE if "profile" in order else ""))
        for authz_url in answer.get("authorizations", []):
            _, authz = self.post_json(authz_url, None, ok=(200,))
            if authz.get("status") == "valid":
                continue
            challenge = next((c for c in authz.get("challenges", []) if c.get("type") == "http-01"), None)
            if not challenge:
                raise AcmeError("no http-01 challenge")
            token = challenge["token"]
            if not token or any(ch not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" for ch in token):
                raise AcmeError("a strange token")
            path = os.path.join(self.folder, "challenges", token)
            with open(path, "w", encoding="ascii") as f:
                f.write(token + "." + self.thumbprint)
            try:
                self.post_json(challenge["url"], {})
                self.wait(authz_url, ("valid",))
            finally:
                try:
                    os.remove(path)
                except OSError:
                    pass
        answer = self.wait(order_url, ("ready", "valid"))
        if answer.get("status") == "ready":
            self.post_json(answer["finalize"], {"csr": b64u(make_csr(key_path, ip))})
            answer = self.wait(order_url, ("valid",))
        status, _, body = self.post(answer["certificate"], None)
        if status != 200:
            raise AcmeError("the certificate's download: HTTP %d" % status)
        text = body.decode("ascii", "replace")
        part = cert_path + ".part"
        with open(part, "w", encoding="ascii") as f:
            f.write(text)
        not_after, problem = check_cert(part, key_path)
        if problem:
            os.remove(part)
            raise AcmeError("the new certificate: %s" % problem)
        os.chmod(part, 0o644)
        os.replace(part, cert_path)
        remember_ip(self.folder, ip)  # (the renewals': emusuni-reports serve)
        self.say("acme: a new certificate for %s, valid until %s UTC" %
                 (ip, time.strftime("%Y-%m-%d %H:%M", time.gmtime(not_after))))
        return not_after


def saved_account(folder):
    """account.json (the directory, the IP, 00.95: the names), or None: no certificate was asked for yet."""
    try:
        with open(os.path.join(folder, "account.json"), encoding="utf-8") as f:
            state = json.load(f)
        return state if state.get("kid") and (state.get("ip") or state.get("names")) else None
    except (OSError, ValueError):
        return None


def identities(account):
    """(00.95) What the account's certificates are for - the IP first, then each name -, renewed each by itself."""
    return ([account["ip"]] if account.get("ip") else []) + list(account.get("names", []))


def remember_ip(folder, ip):
    """The IP (00.95: or a name, among the names) whose certificate the server renews."""
    kind, value = identifier(ip)
    path = os.path.join(folder, "account.json")
    with open(path, encoding="utf-8") as f:
        state = json.load(f)
    if kind == "ip":
        state["ip"] = value
    elif value not in state.get("names", []):
        state["names"] = state.get("names", []) + [value]
    with open(path + ".part", "w", encoding="utf-8") as f:
        json.dump(state, f)
    os.replace(path + ".part", path)
