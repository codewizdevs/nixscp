#!/usr/bin/env python3
"""Malicious WebDAV server for tests/test_verify_security.cpp (TestEvilWebDav).

Usage: python3 -I evil_dav.py PORT ABSDIR LOGFILE      (stdlib only)

 * PROPFIND of /evil/ answers with hrefs that traverse in every way a href can:
   raw "..", "%2e%2e", "%2f" hidden separators, absolute paths, absolute URLs,
   a NUL, hostile collections, the same one level deeper in /evil/sub/.
 * Every listed href exists and every GET gets a payload, so that the client
   alone stops the attack. Other paths are 404, so a rename reaches MOVE.
 * LOGFILE, flushed at once: the raw request line, then each received header
   as "  Name: value". The head is split into lines by hand (at CR/LF or LF)
   instead of by http.server: a CR/LF in the request target must appear as a
   header line of its own, as it would in any HTTP server behind it.
EVIL_KEEP_LOG names a second log that outlives the test's temporary one.
"""
import os
import re
import socketserver
import sys
import threading
from urllib.parse import quote, unquote
from xml.sax.saxutils import escape

PORT = int(sys.argv[1])
ABSDIR = sys.argv[2]
LOGFILE = sys.argv[3]

OK_DATA = b"regular file\n"
PAYLOAD = b"PWNED by evil_dav\n"
ORIGIN = "http://127.0.0.1:%d" % PORT


def enc(path):
    """A path with all of its separators and dots percent-encoded."""
    return "".join("%%%02x" % ord(c) if c in "/." else quote(c, safe="") for c in path)


# hrefs of the /evil/ listing: (href, collection)
EVIL = [
    ("/evil/ok.txt", False),
    ("/evil/../esc1.txt", False),
    ("/evil/../../esc2.txt", False),
    ("/evil/..%2fesc3.txt", False),
    ("/evil/..%2f..%2fesc4.txt", False),
    ("/evil/..%2F..%2F..%2Fesc5.txt", False),
    ("/evil/%2e%2e%2f%2e%2e%2fesc6.txt", False),
    ("/evil/%2e%2e/%2e%2e/esc7.txt", False),
    ("/evil/sub%2f..%2f..%2f..%2fesc8.txt", False),
    ("/evil/./../../esc9.txt", False),
    ("/evil/.%2f..%2f..%2fesc10.txt", False),
    (ORIGIN + "/evil/..%2f..%2fesc11.txt", False),
    (ORIGIN + "/evil/../../esc12.txt", False),
    ("//evil/..%2f..%2fesc13.txt", False),
    ("/evil/x%00/../../esc14.txt", False),
    ("/evil/" + enc(ABSDIR + "/pwned1.txt"), False),
    ("/evil//" + ABSDIR.lstrip("/") + "/pwned2.txt", False),
    ("/evil/%2f" + ABSDIR.lstrip("/") + "/pwned3.txt", False),
    (ABSDIR + "/pwned4.txt", False),
    (ORIGIN + ABSDIR + "/pwned5.txt", False),
    ("/evil/..%5c..%5cbackslash.txt", False),
    ("/evil/%252e%252e%252fdouble-encoded.txt", False),
    ("/evil/%2e%2e/", True),
    ("/evil/%2e%2e%2f%2e%2e%2fescdir/", True),
    ("/evil/" + enc(ABSDIR + "/pwned-dir") + "/", True),
    ("/evil/sub/", True),
]
SUB = [
    ("/evil/sub/ok2.txt", False),
    ("/evil/sub/..%2f..%2f..%2fesc15.txt", False),
    ("/evil/sub/" + enc(ABSDIR + "/pwned-sub.txt"), False),
]

LISTED = {href for href, _ in EVIL + SUB}
LISTED_PLAIN = {unquote(href).rstrip("/") for href in LISTED}

log_lock = threading.Lock()
log_files = [open(name, "a", buffering=1, encoding="utf-8", errors="surrogateescape")
             for name in (LOGFILE, os.environ.get("EVIL_KEEP_LOG")) if name]


def log(line):
    with log_lock:
        for log_file in log_files:
            log_file.write(line + "\n")
            log_file.flush()


def data_of(path):
    return OK_DATA if path.endswith("/ok.txt") or path.endswith("/ok2.txt") else PAYLOAD


def response_xml(href, collection):
    if collection:
        props = "<D:resourcetype><D:collection/></D:resourcetype>"
    else:
        size = len(data_of(href))
        props = "<D:resourcetype/><D:getcontentlength>%d</D:getcontentlength>" % size
    return ("<D:response><D:href>%s</D:href><D:propstat><D:prop>%s"
            "<D:getlastmodified>Wed, 01 Jan 2020 00:00:00 GMT</D:getlastmodified></D:prop>"
            "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>") % (escape(href), props)


def multistatus(entries):
    body = "".join(response_xml(h, c) for h, c in entries)
    return ('<?xml version="1.0" encoding="utf-8"?>\n<D:multistatus xmlns:D="DAV:">%s</D:multistatus>\n' % body).encode()


def propfind(target, depth):
    path = target.split("?", 1)[0]
    plain = unquote(path).rstrip("/") or "/"
    if plain == "/":
        entries = [("/", True)]
        if depth != "0":
            entries.append(("/evil/", True))
    elif plain == "/evil":
        entries = [("/evil/", True)]
        if depth != "0":
            entries += EVIL
    elif plain == "/evil/sub":
        entries = [("/evil/sub/", True)]
        if depth != "0":
            entries += SUB
    elif "escdir" in plain or "pwned-dir" in plain or plain.endswith("/.."):
        # a hostile collection the client walked into
        entries = [(path if path.endswith("/") else path + "/", True)]
        if depth != "0":
            entries.append((path.rstrip("/") + "/..%2f..%2f..%2f..%2fesc16.txt", False))
    elif path in LISTED or plain in LISTED_PLAIN:
        entries = [(path, False)]
    else:
        # the new names of the test's rename and mkdir do not exist yet, so
        # that the client really sends MOVE with its Destination header
        return None
    return multistatus(entries)


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.buf = b""

    def respond(self, status, headers=(), body=b"", head_only=False):
        lines = ["HTTP/1.1 " + status, "Server: evil_dav", "Content-Length: %d" % len(body)]
        lines += ["%s: %s" % h for h in headers]
        self.request.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + (b"" if head_only else body))

    def fill(self):
        chunk = self.request.recv(65536)
        if not chunk:
            raise ConnectionError
        self.buf += chunk

    def read_body(self, headers):
        if headers.get("transfer-encoding", "").lower() == "chunked":
            body = b""
            while True:
                while b"\r\n" not in self.buf:
                    self.fill()
                size, self.buf = self.buf.split(b"\r\n", 1)
                size = int(size.split(b";")[0], 16)
                while len(self.buf) < size + 2:
                    self.fill()
                body += self.buf[:size]
                self.buf = self.buf[size + 2:]
                if size == 0:
                    return body
        length = int(headers.get("content-length", "0") or 0)
        while len(self.buf) < length:
            self.fill()
        body, self.buf = self.buf[:length], self.buf[length:]
        return body

    def handle(self):
        try:
            while self.one_request():
                pass
        except (ConnectionError, OSError):
            pass

    def one_request(self):
        while True:
            match = re.search(rb"\r?\n\r?\n", self.buf)
            if match is not None:
                break
            self.fill()
        head, self.buf = self.buf[:match.start()], self.buf[match.end():]
        lines = re.split(r"\r\n|\n|\r", head.decode("utf-8", "surrogateescape"))
        log(lines[0])
        headers = {}
        for line in lines[1:]:
            log("  " + line)
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        self.read_body(headers)

        parts = lines[0].split(" ")
        method, target = parts[0], parts[1] if len(parts) > 1 else "/"
        if method == "OPTIONS":
            self.respond("200 OK", [("DAV", "1, 2"), ("Allow", "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY")])
        elif method == "PROPFIND":
            body = propfind(target, headers.get("depth", "1"))
            if body is None:
                self.respond("404 Not Found")
            else:
                self.respond("207 Multi-Status", [("Content-Type", "application/xml; charset=utf-8")], body)
        elif method in ("GET", "HEAD"):
            self.respond("200 OK", [("Content-Type", "application/octet-stream"),
                                    ("Last-Modified", "Wed, 01 Jan 2020 00:00:00 GMT")],
                         data_of(unquote(target)), method == "HEAD")
        elif method in ("MKCOL", "MOVE", "COPY", "PUT"):
            self.respond("201 Created")
        elif method == "DELETE":
            self.respond("204 No Content")
        elif method == "PROPPATCH":
            self.respond("207 Multi-Status", [("Content-Type", "application/xml; charset=utf-8")], multistatus([]))
        else:
            self.respond("405 Method Not Allowed")
        return headers.get("connection", "").lower() != "close"


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
