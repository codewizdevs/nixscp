#!/usr/bin/env python3
"""SOCKS4/4A + SOCKS5 (puser/ppass) + HTTP CONNECT proxy of tests/test_ftpex.cpp
on one port (NIXSCP_TEST_PROXY_PORT, stdlib only):

  proxy.py PORT LOGFILE

ftp.nixscp.test resolves to 127.0.0.1 here only; anything but the local host
is refused. LOGFILE gets "<kind> <host>:<port>" per connection, kind one of
socks4a, socks5, http; opened for append, as the test truncates it.
"""
import select
import socket
import socketserver
import struct
import sys
import threading

PORT = int(sys.argv[1])
LOGFILE = sys.argv[2]
LOCAL = {"ftp.nixscp.test": "127.0.0.1", "127.0.0.1": "127.0.0.1", "localhost": "127.0.0.1"}

log_lock = threading.Lock()


def log(line):
    with log_lock:
        with open(LOGFILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def connect(host, port):
    address = LOCAL.get(host)
    if address is None:
        return None
    try:
        return socket.create_connection((address, port), timeout=5)
    except OSError:
        return None


def pump(a, b):
    a.settimeout(None)
    b.settimeout(None)
    sockets = [a, b]
    while True:
        readable, _, _ = select.select(sockets, [], [])
        for sock in readable:
            data = sock.recv(65536)
            if not data:
                return
            (b if sock is a else a).sendall(data)


class Handler(socketserver.BaseRequestHandler):
    def read(self, size):
        data = b""
        while len(data) < size:
            chunk = self.request.recv(size - len(data))
            if not chunk:
                raise ConnectionError
            data += chunk
        return data

    def read_zstring(self):
        data = b""
        while True:
            byte = self.read(1)
            if byte == b"\0":
                return data
            data += byte

    def handle(self):
        try:
            first = self.read(1)
            if first == b"\x04":
                remote = self.socks4()
            elif first == b"\x05":
                remote = self.socks5()
            else:
                remote = self.http(first)
            if remote is not None:
                with remote:
                    pump(self.request, remote)
        except (ConnectionError, OSError):
            pass

    def socks4(self):
        command, port = struct.unpack(">BH", self.read(3))
        address = self.read(4)
        self.read_zstring()
        host = socket.inet_ntoa(address)
        if address[:3] == b"\0\0\0" and address[3] != 0:
            host = self.read_zstring().decode()
        log("socks4a %s:%d" % (host, port))
        remote = connect(host, port) if command == 1 else None
        self.request.sendall(struct.pack(">BBH4s", 0, 90 if remote is not None else 91, 0, b"\0\0\0\0"))
        return remote

    def socks5(self):
        methods = self.read(self.read(1)[0])
        if 2 not in methods:
            self.request.sendall(b"\x05\xff")
            return None
        self.request.sendall(b"\x05\x02")
        self.read(1)
        user = self.read(self.read(1)[0])
        password = self.read(self.read(1)[0])
        if (user, password) != (b"puser", b"ppass"):
            self.request.sendall(b"\x01\x01")
            return None
        self.request.sendall(b"\x01\x00")
        _, command, _, kind = self.read(4)
        if kind == 1:
            host = socket.inet_ntoa(self.read(4))
        elif kind == 3:
            host = self.read(self.read(1)[0]).decode()
        else:
            host = socket.inet_ntop(socket.AF_INET6, self.read(16))
        port = struct.unpack(">H", self.read(2))[0]
        log("socks5 %s:%d" % (host, port))
        remote = connect(host, port) if command == 1 else None
        self.request.sendall(bytes([5, 0 if remote is not None else 5, 0, 1, 0, 0, 0, 0, 0, 0]))
        return remote

    def http(self, first):
        head = first
        while not head.endswith(b"\r\n\r\n"):
            head += self.read(1)
        request = head.split(b"\r\n", 1)[0].decode().split(" ")
        if request[0] != "CONNECT":
            self.request.sendall(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n")
            return None
        host, _, port = request[1].rpartition(":")
        log("http %s:%s" % (host, port))
        remote = connect(host, int(port))
        self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n" if remote is not None else
                             b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
        return remote


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
