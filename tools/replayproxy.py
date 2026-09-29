#!/usr/bin/env python3
"""replayproxy.py record|replay STORE [PORT]: an HTTP(S) proxy that records
what a browser loads and plays it back, so page loads can be measured with
the network out of the picture.

  record  forwards every request to the real server and keeps the response
  replay  answers only from STORE: the same method and URL, else the same
          method and URL without its query (scripts add timestamps and cache
          busters), else 404. Nothing reaches the network.

HTTPS: CONNECT tunnels are answered with a certificate for the host, signed
by a test CA made in STORE/ca.* on first use (the Mac's openssl), so the
proxy can read them. The browser has to accept that CA or ignore TLS errors:
heliotrope does so only with HELIOTROPE_PROXY set (a test mode).

Listens on 127.0.0.1 (iris's NAT reaches it from the guest at the gateway
address, 10.28.0.1 on atom). Standard library only, plus openssl.
"""
import hashlib
import http.client
import json
import os
import socket
import ssl
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOP = {"connection", "keep-alive", "proxy-connection", "proxy-authorization", "proxy-authenticate",
       "te", "trailer", "transfer-encoding", "upgrade"}

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
STORE = sys.argv[2] if len(sys.argv) > 2 else ""
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8087
if MODE not in ("record", "replay") or not STORE:
    sys.exit(__doc__)
os.makedirs(os.path.join(STORE, "bodies"), exist_ok=True)
os.makedirs(os.path.join(STORE, "certs"), exist_ok=True)
INDEX = os.path.join(STORE, "index.json")
lock = threading.Lock()
index = json.load(open(INDEX)) if os.path.exists(INDEX) else {}
stats = {"hit": 0, "loose": 0, "miss": 0, "recorded": 0}


def save_index():
    tmp = INDEX + ".tmp"
    with open(tmp, "w") as f:
        json.dump(index, f)
    os.replace(tmp, INDEX)


# ---- certificates ----

CA_KEY = os.path.join(STORE, "ca.key")
CA_CRT = os.path.join(STORE, "ca.crt")


def openssl(*args):
    subprocess.run(["openssl"] + list(args), check=True, capture_output=True)


def ensure_ca():
    if not os.path.exists(CA_CRT):
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", CA_KEY, "-out", CA_CRT,
                "-days", "3650", "-subj", "/CN=heliotrope replay proxy test CA")


def host_context(host):
    key = os.path.join(STORE, "certs", host + ".key")
    crt = os.path.join(STORE, "certs", host + ".crt")
    with lock:
        if not os.path.exists(crt):
            csr = os.path.join(STORE, "certs", host + ".csr")
            ext = os.path.join(STORE, "certs", host + ".ext")
            with open(ext, "w") as f:
                f.write("subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\n" % host)
            openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=" + host)
            openssl("x509", "-req", "-in", csr, "-CA", CA_CRT, "-CAkey", CA_KEY, "-CAcreateserial",
                    "-out", crt, "-days", "825", "-extfile", ext)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    ctx.set_alpn_protocols(["http/1.1"])
    return ctx


# ---- the store ----

def keys(method, url):
    loose = url.split("?", 1)[0]
    return method + " " + url, method + " ~" + loose


def remember(method, url, status, headers, body):
    digest = hashlib.sha1(body).hexdigest()
    path = os.path.join(STORE, "bodies", digest)
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.write(body)
    entry = {"status": status, "headers": headers, "body": digest}
    exact, loose = keys(method, url)
    with lock:
        index[exact] = entry
        index.setdefault(loose, entry)
        stats["recorded"] += 1
        save_index()


def recall(method, url):
    exact, loose = keys(method, url)
    with lock:
        if exact in index:
            stats["hit"] += 1
            return index[exact], "hit"
        if loose in index:
            stats["loose"] += 1
            return index[loose], "loose"
        stats["miss"] += 1
    return None, "miss"


# ---- the proxy ----

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    scheme_host = None  # set inside a CONNECT tunnel: ("https", "host:port")

    def log_message(self, fmt, *args):
        pass

    def do_CONNECT(self):
        host = self.path.split(":", 1)[0]
        self.send_response(200, "Connection Established")
        self.end_headers()
        try:
            tls = host_context(host).wrap_socket(self.connection, server_side=True)
        except (ssl.SSLError, OSError) as e:
            print("tls %s: %s" % (host, e), flush=True)
            self.close_connection = True
            return
        inner = type("Tunnel", (Handler,), {"scheme_host": ("https", self.path)})
        inner(tls, self.client_address, self.server)
        self.close_connection = True

    def url(self):
        if self.scheme_host:
            scheme, hostport = self.scheme_host
            host = hostport[:-4] if hostport.endswith(":443") else hostport
            return "%s://%s%s" % (scheme, host, self.path)
        return self.path  # plain proxy request: absolute URL

    def handle_any(self):
        url = self.url()
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if MODE == "replay":
            entry, how = recall(self.command, url)
            if entry is None:
                print("miss %s %s" % (self.command, url[:150]), flush=True)
                self.reply(404, [("Content-Type", "text/plain")], b"not recorded\n")
                return
            data = open(os.path.join(STORE, "bodies", entry["body"]), "rb").read()
            self.reply(entry["status"], entry["headers"], data)
            return
        # record
        u = urllib.parse.urlsplit(url)
        conn_cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP}
        try:
            conn = conn_cls(u.netloc, timeout=60)
            conn.request(self.command, (u.path or "/") + ("?" + u.query if u.query else ""), body, headers)
            resp = conn.getresponse()
            data = resp.read()
            rh = [(k, v) for k, v in resp.getheaders() if k.lower() not in HOP and k.lower() != "content-length"]
            conn.close()
        except Exception as e:
            print("fail %s %s: %s" % (self.command, url[:150], e), flush=True)
            self.reply(502, [("Content-Type", "text/plain")], b"upstream failed\n")
            return
        remember(self.command, url, resp.status, rh, data)
        self.reply(resp.status, rh, data)

    def reply(self, status, headers, data):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    do_GET = do_POST = do_HEAD = do_PUT = do_DELETE = do_OPTIONS = do_PATCH = handle_any


def main():
    import signal
    signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt()))
    ensure_ca()
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.daemon_threads = True
    print("%s proxy on 127.0.0.1:%d, store %s (%d entries)" % (MODE, PORT, STORE, len(index)), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        print("stats %s" % stats, flush=True)


main()
