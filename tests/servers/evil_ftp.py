#!/usr/bin/env python3
"""Malicious FTP server for tests/test_verify_security.cpp (TestEvilFtp).

Usage: python3 -I evil_ftp.py PORT ABSDIR LOGFILE      (stdlib only)

The test starts it on a free port and logs in as u/p. The server:

 * lists /evil with hostile names next to the regular file ok.txt: relative
   traversal, a name that is the absolute path ABSDIR/..., ".", "..", hostile
   directories, the same one level deeper in /evil/sub;
 * says yes to anything the client asks about the hostile names and serves a
   payload for every RETR, so it is the client alone that stops the attack;
 * writes every received command line to LOGFILE, flushed at once (the test
   reads the log while the server still runs). Lines are split at CR, LF and
   CR/LF as the most permissive real servers do, so a CR/LF that reached the
   wire shows as a separate "DELE /victim..." line.

EVIL_FTP_MODE selects the listing the client gets (default "mlsd"):
  mlsd   FEAT announces MLST, listings by MLSD
  list   no MLST in FEAT, Unix LIST format, with a symlink out of the tree
  dos    no MLST in FEAT, IIS/DOS LIST format
EVIL_KEEP_LOG names a second log that outlives the test's temporary one.
EVIL_FTP_STOR_PARTIAL is the reply to a STOR of a .filepart file; that one, and
names with "missing", are then the names that do not exist. Other files are
taken and thrown away, and answered with EVIL_FTP_STOR_END when that is set.
"""
import os
import re
import socket
import socketserver
import sys
import threading

PORT = int(sys.argv[1])
ABSDIR = sys.argv[2]
LOGFILE = sys.argv[3]
MODE = os.environ.get("EVIL_FTP_MODE", "mlsd")
STOR_PARTIAL = os.environ.get("EVIL_FTP_STOR_PARTIAL")
STOR_END = os.environ.get("EVIL_FTP_STOR_END", "226 Transfer complete")

OK_DATA = b"regular file\n"
PAYLOAD = b"PWNED by evil_ftp\n"

FILE, DIR, LINK = "file", "dir", "link"

# Directory -> entries (name, kind). The names are what an attacker sends.
TREE = {
    "/": [("evil", DIR)],
    "/evil": [
        (".", DIR),
        ("..", DIR),
        ("ok.txt", FILE),
        ("../esc1.txt", FILE),
        ("../../esc2.txt", FILE),
        ("../../../esc3.txt", FILE),
        ("../../../../esc4.txt", FILE),
        ("sub/../../esc5.txt", FILE),
        ("./../../esc6.txt", FILE),
        ("..//..//esc7.txt", FILE),
        (ABSDIR + "/pwned-file.txt", FILE),
        ("/" + ABSDIR + "/pwned-file2.txt", FILE),
        ("../../escdir", DIR),
        (ABSDIR + "/pwned-dir", DIR),
        ("x/..", DIR),
        ("..\\..\\backslash.txt", FILE),
        ("sub", DIR),
    ],
    "/evil/sub": [
        ("ok2.txt", FILE),
        ("../../esc8.txt", FILE),
        ("../../../../esc9.txt", FILE),
        (ABSDIR + "/pwned-sub.txt", FILE),
    ],
}
if MODE == "list":
    TREE["/evil"].append(("link-out", LINK))

log_lock = threading.Lock()
log_files = [open(name, "a", buffering=1, encoding="utf-8", errors="surrogateescape")
             for name in (LOGFILE, os.environ.get("EVIL_KEEP_LOG")) if name]


def log(line):
    with log_lock:
        for log_file in log_files:
            log_file.write(line + "\n")
            log_file.flush()


def kind_of(path):
    """Everything exists: "/evil" and co. are directories, the hostile
    directories too, the rest is a file."""
    path = path.rstrip("/") or "/"
    if path in TREE:
        return DIR
    if "escdir" in path.rsplit("/", 1)[-1] or "pwned-dir" in path or path.endswith("/.."):
        return DIR
    return FILE


def list_of(path):
    path = path.rstrip("/") or "/"
    if path in TREE:
        return TREE[path]
    # a hostile directory the client walked into
    return [("dir-payload.txt", FILE), ("../../../../../../esc10.txt", FILE)]


def data_of(path):
    return OK_DATA if path.endswith("/ok.txt") or path.endswith("/ok2.txt") else PAYLOAD


def mlsd_line(name, kind):
    size = len(OK_DATA) if name in ("ok.txt", "ok2.txt") else len(PAYLOAD)
    if kind == DIR:
        if name == ".":
            facts = "type=cdir;modify=20200101000000;perm=flcdmpe;"
        elif name == "..":
            facts = "type=pdir;modify=20200101000000;perm=flcdmpe;"
        else:
            facts = "type=dir;modify=20200101000000;perm=flcdmpe;"
    else:
        facts = "type=file;size=%d;modify=20200101000000;perm=adfrw;" % size
    return "%s %s\r\n" % (facts, name)


def unix_line(name, kind):
    size = len(OK_DATA) if name in ("ok.txt", "ok2.txt") else len(PAYLOAD)
    if kind == DIR:
        return "drwxr-xr-x 2 evil evil 4096 Jan 01  2020 %s\r\n" % name
    if kind == LINK:
        return "lrwxrwxrwx 1 evil evil 10 Jan 01  2020 %s -> %s\r\n" % (name, ABSDIR)
    return "-rw-r--r-- 1 evil evil %d Jan 01  2020 %s\r\n" % (size, name)


def dos_line(name, kind):
    size = len(OK_DATA) if name in ("ok.txt", "ok2.txt") else len(PAYLOAD)
    return "01-01-20  12:00AM  %20s %s\r\n" % ("<DIR>" if kind == DIR else str(size), name)


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.cwd = "/"
        self.listener = None
        self.active = None

    def send(self, text):
        self.request.sendall(text.encode("utf-8", "surrogateescape") + b"\r\n")

    def abspath(self, arg):
        if arg.startswith("/"):
            return arg
        return self.cwd.rstrip("/") + "/" + arg

    def open_data(self):
        if self.active is not None:
            sock = socket.create_connection(self.active, timeout=10)
            self.active = None
            return sock
        if self.listener is None:
            return None
        self.listener.settimeout(10)
        sock, _ = self.listener.accept()
        self.listener.close()
        self.listener = None
        return sock

    def transfer(self, data):
        self.send("150 Opening data connection")
        sock = self.open_data()
        if sock is None:
            self.send("425 Use PASV or PORT first")
            return
        try:
            sock.sendall(data)
        finally:
            sock.close()
        self.send("226 Transfer complete")

    def listing(self, path):
        entries = list_of(path)
        if MODE == "mlsd":
            lines = [mlsd_line(n, k) for n, k in entries]
        elif MODE == "dos":
            lines = [dos_line(n, k) for n, k in entries]
        else:
            lines = [unix_line(n, k) for n, k in entries]
        return "".join(lines).encode("utf-8", "surrogateescape")

    def handle(self):
        self.send("220 evil_ftp ready")
        buf = b""
        while True:
            try:
                chunk = self.request.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while True:
                match = re.search(rb"\r\n|\n|\r", buf)
                if match is None:
                    break
                line, buf = buf[:match.start()], buf[match.end():]
                text = line.decode("utf-8", "surrogateescape")
                log(text)
                if not self.command(text):
                    return

    def command(self, text):
        verb, _, arg = text.partition(" ")
        verb = verb.upper()
        if verb == "USER":
            self.send("331 Password required")
        elif verb == "PASS":
            self.send("230 Logged in")
        elif verb == "SYST":
            self.send("215 Windows_NT" if MODE == "dos" else "215 UNIX Type: L8")
        elif verb == "FEAT":
            self.send("211-Features:")
            if MODE == "mlsd":
                self.send(" MLST type*;size*;modify*;perm*;")
            for feature in ("MDTM", "SIZE", "REST STREAM", "UTF8", "EPSV"):
                self.send(" " + feature)
            self.send("211 End")
        elif verb in ("OPTS", "TYPE", "MODE", "STRU", "NOOP", "SITE", "CLNT", "PBSZ", "PROT"):
            self.send("200 OK")
        elif verb == "PWD":
            self.send('257 "%s" is the current directory' % self.cwd)
        elif verb == "CWD":
            path = self.abspath(arg)
            if kind_of(path) == DIR:
                self.cwd = path.rstrip("/") or "/"
                self.send("250 Directory changed")
            else:
                self.send("550 Not a directory")
        elif verb == "CDUP":
            self.cwd = self.cwd.rsplit("/", 1)[0] or "/"
            self.send("250 Directory changed")
        elif verb == "PASV":
            self.new_listener()
            host, port = self.listener.getsockname()
            self.send("227 Entering Passive Mode (%s,%d,%d)" % (host.replace(".", ","), port >> 8, port & 255))
        elif verb == "EPSV":
            self.new_listener()
            self.send("229 Entering Extended Passive Mode (|||%d|)" % self.listener.getsockname()[1])
        elif verb == "PORT":
            parts = arg.split(",")
            self.active = (".".join(parts[:4]), (int(parts[4]) << 8) + int(parts[5]))
            self.send("200 PORT OK")
        elif verb == "EPRT":
            parts = arg.split(arg[0])
            self.active = (parts[2], int(parts[3]))
            self.send("200 EPRT OK")
        elif verb in ("LIST", "NLST", "MLSD"):
            args = [a for a in arg.split(" ") if a and not a.startswith("-")]
            self.transfer(self.listing(self.abspath(args[0]) if args else self.cwd))
        elif STOR_PARTIAL and verb in ("MLST", "SIZE", "MDTM") and (arg.endswith(".filepart") or "missing" in arg):
            self.send("550 No such file")
        elif STOR_PARTIAL and verb == "STOR":
            if arg.endswith(".filepart"):
                self.send(STOR_PARTIAL)
            else:
                self.send("150 Opening data connection")
                sock = self.open_data()
                while sock.recv(65536):
                    pass
                sock.close()
                self.send(STOR_END)
        elif verb == "MLST":
            path = self.abspath(arg) if arg else self.cwd
            self.send("250-Listing " + path)
            self.send(" " + mlsd_line(path, kind_of(path)).rstrip("\r\n"))
            self.send("250 End")
        elif verb == "SIZE":
            path = self.abspath(arg)
            if kind_of(path) == DIR:
                self.send("550 Is a directory")
            else:
                self.send("213 %d" % len(data_of(path)))
        elif verb == "MDTM":
            self.send("213 20200101000000")
        elif verb == "REST":
            self.send("350 Restarting")
        elif verb == "RETR":
            self.transfer(data_of(self.abspath(arg)))
        elif verb == "RNFR":
            self.send("350 Ready for RNTO")
        elif verb in ("RNTO", "DELE", "RMD"):
            self.send("250 OK")
        elif verb == "MKD":
            self.send('257 "%s" created' % self.abspath(arg))
        elif verb == "ABOR":
            self.send("226 Aborted")
        elif verb == "QUIT":
            self.send("221 Bye")
            return False
        else:
            self.send("502 Not implemented")
        return True

    def new_listener(self):
        if self.listener is not None:
            self.listener.close()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
