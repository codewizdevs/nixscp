#!/usr/bin/env python3
"""Malicious SFTP v3 server for tests/test_verify_security.cpp (TestEvilSftp).

An sshd subsystem, stdlib only, speaks on stdin/stdout:

  Subsystem sftp /usr/bin/python3 -I /path/evil_sftp.py ABSDIR [LOGFILE]

ABSDIR is the test's NIXSCP_TEST_EVIL_SFTP_ABSDIR. READDIR of /evil returns
hostile names next to the regular ok.txt: relative traversal, the absolute
name ABSDIR/..., ".", "..", hostile directories, a name with a NUL, a symlink
out of the tree; /evil/sub repeats it one level deeper. Whatever the client
then asks about exists, and every READ returns a payload: only the client can
stop the attack. A server is free to send any byte string as a file name
(SSH_FXP_NAME has a length prefix), which is what this does.
"""
import struct
import sys

SSH_FXP_INIT, SSH_FXP_VERSION = 1, 2
SSH_FXP_OPEN, SSH_FXP_CLOSE, SSH_FXP_READ, SSH_FXP_WRITE = 3, 4, 5, 6
SSH_FXP_LSTAT, SSH_FXP_FSTAT, SSH_FXP_SETSTAT, SSH_FXP_FSETSTAT = 7, 8, 9, 10
SSH_FXP_OPENDIR, SSH_FXP_READDIR, SSH_FXP_REMOVE = 11, 12, 13
SSH_FXP_MKDIR, SSH_FXP_RMDIR, SSH_FXP_REALPATH, SSH_FXP_STAT = 14, 15, 16, 17
SSH_FXP_RENAME, SSH_FXP_READLINK, SSH_FXP_SYMLINK = 18, 19, 20
SSH_FXP_STATUS, SSH_FXP_HANDLE, SSH_FXP_DATA, SSH_FXP_NAME, SSH_FXP_ATTRS = 101, 102, 103, 104, 105
SSH_FXP_EXTENDED, SSH_FXP_EXTENDED_REPLY = 200, 201
FX_OK, FX_EOF, FX_NO_SUCH_FILE, FX_PERMISSION_DENIED, FX_FAILURE, FX_OP_UNSUPPORTED = 0, 1, 2, 3, 4, 8
ATTR_SIZE, ATTR_UIDGID, ATTR_PERMISSIONS, ATTR_ACMODTIME = 1, 2, 4, 8
S_IFDIR, S_IFREG, S_IFLNK = 0o040000, 0o100000, 0o120000
MTIME = 1577836800

ABSDIR = sys.argv[1].encode()
LOG = open(sys.argv[2], "ab", buffering=0) if len(sys.argv) > 2 else None

OK_DATA = b"regular file\n"
PAYLOAD = b"PWNED by evil_sftp\n"

FILE, DIR, LINK = "file", "dir", "link"
TREE = {
    b"/": [(b"evil", DIR)],
    b"/evil": [
        (b".", DIR),
        (b"..", DIR),
        (b"ok.txt", FILE),
        (b"../esc1.txt", FILE),
        (b"../../esc2.txt", FILE),
        (b"../../../esc3.txt", FILE),
        (b"../../../../esc4.txt", FILE),
        (b"sub/../../esc5.txt", FILE),
        (b"./../../esc6.txt", FILE),
        (b"..//..//esc7.txt", FILE),
        (ABSDIR + b"/pwned-file.txt", FILE),
        (b"/" + ABSDIR + b"/pwned-file2.txt", FILE),
        (b"../../escdir", DIR),
        (ABSDIR + b"/pwned-dir", DIR),
        (b"x/..", DIR),
        (b"nul\x00/../../esc-nul.txt", FILE),
        (b"..\\..\\backslash.txt", FILE),
        (b"link-out", LINK),
        (b"sub", DIR),
    ],
    b"/evil/sub": [
        (b"ok2.txt", FILE),
        (b"../../esc8.txt", FILE),
        (b"../../../../esc9.txt", FILE),
        (ABSDIR + b"/pwned-sub.txt", FILE),
    ],
}

stdin = sys.stdin.buffer
stdout = sys.stdout.buffer


def log(*args):
    if LOG is not None:
        LOG.write(b" ".join(a if isinstance(a, bytes) else str(a).encode() for a in args) + b"\n")


def string(data):
    return struct.pack(">I", len(data)) + data


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def u32(self):
        value = struct.unpack_from(">I", self.data, self.pos)[0]
        self.pos += 4
        return value

    def u64(self):
        value = struct.unpack_from(">Q", self.data, self.pos)[0]
        self.pos += 8
        return value

    def string(self):
        length = self.u32()
        value = self.data[self.pos:self.pos + length]
        self.pos += length
        return value


def send(packet_type, body):
    stdout.write(struct.pack(">IB", len(body) + 1, packet_type) + body)
    stdout.flush()


def status(req, code, message=b""):
    send(SSH_FXP_STATUS, struct.pack(">II", req, code) + string(message) + string(b"en"))


def norm(path):
    path = path.rstrip(b"/") or b"/"
    return path


def kind_of(path):
    path = norm(path)
    if path in TREE:
        return DIR
    last = path.rsplit(b"/", 1)[-1]
    if b"escdir" in last or b"pwned-dir" in path or last in (b"..", b"."):
        return DIR
    if last == b"link-out":
        return LINK
    return FILE


def data_of(path):
    return OK_DATA if path.endswith((b"/ok.txt", b"/ok2.txt")) else PAYLOAD


def attrs(kind, path):
    if kind == DIR:
        mode, size = S_IFDIR | 0o755, 4096
    elif kind == LINK:
        mode, size = S_IFLNK | 0o777, len(ABSDIR)
    else:
        mode, size = S_IFREG | 0o644, len(data_of(path))
    return struct.pack(">IQIIIII", ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME,
                       size, 1000, 1000, mode, MTIME, MTIME)


def long_name(name, kind, path):
    size = 4096 if kind == DIR else len(data_of(path))
    perms = {DIR: b"drwxr-xr-x", LINK: b"lrwxrwxrwx", FILE: b"-rw-r--r--"}[kind]
    return perms + b" 1 evil evil %d Jan  1  2020 " % size + name


def entries_of(path):
    path = norm(path)
    if path in TREE:
        return TREE[path]
    # a hostile directory the client walked into
    return [(b"dir-payload.txt", FILE), (b"../../../../../../esc10.txt", FILE)]


def main():
    handles = {}
    counter = 0
    while True:
        head = stdin.read(4)
        if len(head) < 4:
            return
        length = struct.unpack(">I", head)[0]
        packet = stdin.read(length)
        if len(packet) < length:
            return
        packet_type = packet[0]
        if packet_type == SSH_FXP_INIT:
            log("INIT", packet[1:5].hex())
            send(SSH_FXP_VERSION, struct.pack(">I", 3))
            continue
        reader = Reader(packet[1:])
        req = reader.u32()
        if packet_type == SSH_FXP_REALPATH:
            path = reader.string()
            log("REALPATH", path)
            result = b"/" if path in (b"", b".") else path
            send(SSH_FXP_NAME, struct.pack(">II", req, 1) + string(result) + string(result) + struct.pack(">I", 0))
        elif packet_type in (SSH_FXP_LSTAT, SSH_FXP_STAT):
            path = reader.string()
            log("STAT", path)
            kind = kind_of(path)
            if packet_type == SSH_FXP_STAT and kind == LINK:
                kind = DIR
            send(SSH_FXP_ATTRS, struct.pack(">I", req) + attrs(kind, path))
        elif packet_type == SSH_FXP_READLINK:
            path = reader.string()
            log("READLINK", path)
            send(SSH_FXP_NAME, struct.pack(">II", req, 1) + string(ABSDIR) + string(ABSDIR) + struct.pack(">I", 0))
        elif packet_type == SSH_FXP_OPENDIR:
            path = reader.string()
            log("OPENDIR", path)
            counter += 1
            handle = b"d%d" % counter
            handles[handle] = [path, list(entries_of(path))]
            send(SSH_FXP_HANDLE, struct.pack(">I", req) + string(handle))
        elif packet_type == SSH_FXP_READDIR:
            handle = reader.string()
            state = handles.get(handle)
            if state is None or not state[1]:
                status(req, FX_EOF, b"End of file")
                continue
            path, entries = state
            state[1] = []
            body = struct.pack(">II", req, len(entries))
            for name, kind in entries:
                log("READDIR ->", repr(name))
                body += string(name) + string(long_name(name, kind, name)) + attrs(kind, name)
            send(SSH_FXP_NAME, body)
        elif packet_type == SSH_FXP_OPEN:
            path = reader.string()
            pflags = reader.u32()
            log("OPEN", path, pflags)
            if pflags & 2:
                status(req, FX_PERMISSION_DENIED, b"read only")
                continue
            counter += 1
            handle = b"f%d" % counter
            handles[handle] = [path, data_of(path)]
            send(SSH_FXP_HANDLE, struct.pack(">I", req) + string(handle))
        elif packet_type == SSH_FXP_FSTAT:
            handle = reader.string()
            state = handles.get(handle)
            if state is None:
                status(req, FX_FAILURE, b"bad handle")
            else:
                send(SSH_FXP_ATTRS, struct.pack(">I", req) + attrs(FILE, state[0]))
        elif packet_type == SSH_FXP_READ:
            handle = reader.string()
            offset = reader.u64()
            size = reader.u32()
            state = handles.get(handle)
            if state is None:
                status(req, FX_FAILURE, b"bad handle")
            elif offset >= len(state[1]):
                status(req, FX_EOF, b"End of file")
            else:
                send(SSH_FXP_DATA, struct.pack(">I", req) + string(state[1][offset:offset + size]))
        elif packet_type == SSH_FXP_CLOSE:
            handles.pop(reader.string(), None)
            status(req, FX_OK, b"Success")
        else:
            log("UNSUPPORTED", packet_type)
            status(req, FX_OP_UNSUPPORTED, b"Operation unsupported")


main()
