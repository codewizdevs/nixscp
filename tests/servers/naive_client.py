#!/usr/bin/env python3
"""Negative control for evil_ftp.py, evil_dav.py and evil_sftp.py: a client
without any name check, as a vulnerable product would be, downloads /evil the
way test_verify_security.cpp does (target Work/a/b/target, ABSDIR beside it)
and applies the test's own rule to the result. Each server must make it fail:
if it does not, a pass of the real test proves nothing.

Usage: naive_client.py            (stdlib only, runs the servers itself)
"""
import ftplib
import http.client
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urlsplit
from xml.etree import ElementTree

HERE = os.path.dirname(os.path.abspath(__file__))
SANDBOX = [""]
ESCAPED_SANDBOX = [0]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise SystemExit("server did not start")


def store(local, data):
    """What a client without checks does: join and write, but never outside
    of this control's own scratch directory."""
    if not os.path.normpath(local).startswith(SANDBOX[0] + "/"):
        print("   (would write outside of the sandbox, not done: %s)" % os.path.normpath(local))
        ESCAPED_SANDBOX[0] += 1
        return
    try:
        os.makedirs(os.path.dirname(local), exist_ok=True)
        with open(local, "wb") as f:
            f.write(data)
    except OSError as e:
        print("   (write failed: %s: %s)" % (local, e))


def verdict(name, work, target, absdir):
    allowed = target + "/evil/"
    files = [os.path.join(d, f) for root in (work, absdir) for d, _, fs in os.walk(root) for f in fs]
    escaped = sorted(os.path.normpath(f) for f in files if not os.path.normpath(f).startswith(allowed))
    print("%-7s %d of %d written files escaped the target (the test's CHECK_VULN would fire)" % (name, len(escaped), len(files)))
    for f in escaped[:40]:
        print("   ", f.replace(work, "<Work>").replace(absdir, "<AbsDir>"))
    return bool(escaped)


def ftp(mode, work, target, absdir):
    port = free_port()
    env = dict(os.environ, EVIL_FTP_MODE=mode)
    server = subprocess.Popen(["python3", "-I", HERE + "/evil_ftp.py", str(port), absdir, work + "/../ftp.log"], env=env)
    try:
        wait_port(port)
        c = ftplib.FTP()
        c.connect("127.0.0.1", port)
        c.login("u", "p")

        def walk(remote, local):
            lines = []
            if mode == "mlsd":
                c.cwd(remote)
                c.retrlines("MLSD", lines.append)
                entries = [(l.split(" ", 1)[1], "type=dir" in l) for l in lines]
            else:
                c.cwd(remote)
                c.retrlines("LIST", lines.append)
                entries = [(l.split(None, 8)[8].split(" -> ")[0], l[0] == "d") for l in lines]
            for name, is_dir in entries:
                if name in (".", ".."):
                    continue
                child = remote + "/" + name
                dest = os.path.join(local, name)
                if is_dir:
                    walk(child, dest)
                else:
                    chunks = []
                    c.retrbinary("RETR " + child, chunks.append)
                    store(dest, b"".join(chunks))

        walk("/evil", target + "/evil")
        c.quit()
    finally:
        server.kill()


def dav(work, target, absdir):
    port = free_port()
    server = subprocess.Popen(["python3", "-I", HERE + "/evil_dav.py", str(port), absdir, work + "/../dav.log"])
    try:
        wait_port(port)
        c = http.client.HTTPConnection("127.0.0.1", port)

        def walk(remote, local):
            c.request("PROPFIND", remote, headers={"Depth": "1"})
            xml = c.getresponse().read()
            for resp in ElementTree.fromstring(xml):
                raw = resp.find("{DAV:}href").text
                href = urlsplit(raw).path
                is_dir = resp.find(".//{DAV:}collection") is not None
                name = unquote(href[len(remote):]) if href.startswith(remote) else unquote(href)
                if not name.strip("/") or "\0" in name:
                    continue
                dest = os.path.join(local, name.rstrip("/"))
                if is_dir:
                    walk(href, dest)
                else:
                    c.request("GET", href)
                    store(dest, c.getresponse().read())

        walk("/evil/", target + "/evil")
    finally:
        server.kill()


def sftp(work, target, absdir):
    server = subprocess.Popen(["python3", "-I", HERE + "/evil_sftp.py", absdir], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    counter = [0]

    def s(data):
        return struct.pack(">I", len(data)) + data

    def recv():
        length = struct.unpack(">I", server.stdout.read(4))[0]
        return server.stdout.read(length)

    def req(packet_type, body):
        counter[0] += 1
        packet = bytes([packet_type]) + struct.pack(">I", counter[0]) + body
        server.stdin.write(struct.pack(">I", len(packet)) + packet)
        server.stdin.flush()
        return recv()

    try:
        packet = bytes([1]) + struct.pack(">I", 3)
        server.stdin.write(struct.pack(">I", len(packet)) + packet)
        server.stdin.flush()
        recv()

        def walk(remote, local):
            handle = req(11, s(remote))[9:]
            entries = []
            while True:
                reply = req(12, s(handle))
                if reply[0] != 104:
                    break
                count = struct.unpack_from(">I", reply, 5)[0]
                pos = 9
                for _ in range(count):
                    length = struct.unpack_from(">I", reply, pos)[0]
                    name = reply[pos + 4:pos + 4 + length]
                    pos += 4 + length
                    length = struct.unpack_from(">I", reply, pos)[0]
                    pos += 4 + length
                    flags, _, _, _, mode, _, _ = struct.unpack_from(">IQIIIII", reply, pos)
                    pos += 32
                    entries.append((name, (mode & 0o170000) == 0o040000))
            for name, is_dir in entries:
                if name in (b".", b"..") or b"\0" in name:
                    continue
                child = remote + b"/" + name
                dest = os.path.join(local, name.decode())
                if is_dir:
                    walk(child, dest)
                else:
                    handle2 = req(3, s(child) + struct.pack(">II", 1, 0))[9:]
                    data = req(5, s(handle2) + struct.pack(">QI", 0, 65536))
                    store(dest, data[9:])

        walk(b"/evil", (target + "/evil"))
    finally:
        server.stdin.close()
        server.kill()


def run(name, func, *args):
    with tempfile.TemporaryDirectory() as tmp:
        SANDBOX[0] = tmp
        work = tmp + "/work"
        target = work + "/a/b/target"
        absdir = tmp + "/abs"
        os.makedirs(target)
        os.makedirs(absdir)
        func(*args, work, target, absdir)
        return verdict(name, work, target, absdir)


def crlf():
    """A client that sends a CR/LF: the logs must show what the test looks for."""
    result = True
    with tempfile.TemporaryDirectory() as tmp:
        for script, request, marker in (
                ("evil_ftp.py", b"USER u\r\nRNTO /evil/x\r\nDELE /victim1\r\nMKD /evil/y\nDELE /victim2\r\n", "\nDELE /victim2\n"),
                ("evil_dav.py", b"MKCOL /evil/y\r\nX-Injected: 2/ HTTP/1.1\r\nHost: x\r\n\r\n", "\n  X-Injected:")):
            port = free_port()
            log = tmp + "/" + script + ".log"
            server = subprocess.Popen(["python3", "-I", HERE + "/" + script, str(port), tmp, log])
            try:
                wait_port(port)
                with socket.create_connection(("127.0.0.1", port)) as c:
                    c.sendall(request)
                    time.sleep(0.3)
            finally:
                server.kill()
            text = open(log).read()
            seen = marker in text
            print("%-7s CR/LF injection %s in the log" % (script[5:8], "shows" if seen else "DOES NOT SHOW"))
            result = result and seen
    return result


caught = [run("ftp-mlsd", ftp, "mlsd"), run("ftp-list", ftp, "list"), run("webdav", dav), run("sftp", sftp), crlf()]
print("all servers attack a naive client" if all(caught) else "SOME SERVER DID NOT ATTACK")
sys.exit(0 if all(caught) else 1)
