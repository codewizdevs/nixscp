#!/usr/bin/env python3
"""The pyftpdlib servers of tests/test_ftpex.cpp (NIXSCP_TEST_FTPX_PORT), one
server per process:

  ftpx.py ROOT BASEPORT CERTDIR MODE

  MODE     port  server
  extras   +0    XLOG, HASH, XSHA256, XMD5, XCRC, CSID, CLNT, HOST, SITE CPFR/CPTO
  nomlsd   +1    the same without MLSD/MLST and HASH
  clicert  +2    FTPS, client certificate of the test CA required
  tls12    +3    FTPS, TLS 1.2 only
  tls10    +4    FTPS, TLS 1.0 only (see ftpx-all.sh for the python it needs)
  iis      +5    IIS-like: SYST Windows_NT, rename over a file refused
  ftpproxy +6    FTP firewall for ftp.target.test, user fwuser/fwpass: logon
                 types 1-8 of the test's "FTP proxy logon type"

Users: test/secret on ROOT. XLOG answers with all commands of the session,
"211-<command>" per line, PASS masked as "PASS ****".
"""
import binascii
import hashlib
import logging
import os
import shutil
import sys

from OpenSSL import SSL
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.log import config_logging
from pyftpdlib.servers import ThreadedFTPServer

ROOT, BASE, CERTS, MODE = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
TARGET = "ftp.target.test"
FW_USER, FW_PASS = "fwuser", "fwpass"
USER, PASSWORD = "test", "secret"

HASHES = {"SHA-1": "sha1", "SHA-256": "sha256", "MD5": "md5"}


def authorizer():
    result = DummyAuthorizer()
    result.add_user(USER, PASSWORD, ROOT, perm="elradfmwMT")
    return result


def extra_commands(base):
    commands = dict(base)

    def add(name, needs_auth=True):
        commands[name] = dict(perm=None, auth=needs_auth, arg=None, help="Syntax: %s" % name)

    add("XLOG", False)
    add("HOST", False)
    add("CLNT", False)
    add("CSID", False)
    add("HASH")
    add("XSHA256")
    add("XMD5")
    add("XCRC")
    add("SITE CPFR")
    add("SITE CPTO")
    return commands


class Extras:
    """Mixed into FTPHandler and TLS_FTPHandler."""
    features = ["HASH SHA-1;SHA-256*;MD5;CRC32", "XSHA256", "XMD5", "XCRC", "CSID", "CLNT", "HOST", "SITE COPY"]

    def __init__(self, *args, **kwargs):
        self.xlog = []
        self.hash_alg = "SHA-256"
        self.copy_from = None
        super().__init__(*args, **kwargs)
        self._extra_feats = list(self._extra_feats) + self.features

    def pre_process_command(self, line, cmd, arg):
        self.xlog.append("PASS ****" if cmd == "PASS" else line)
        super().pre_process_command(line, cmd, arg)

    def ftp_XLOG(self, line):
        self.push("".join("211-%s\r\n" % entry for entry in self.xlog))
        self.respond("211 End of log.")

    def ftp_HOST(self, line):
        self.respond("220 Host accepted.")

    def ftp_CLNT(self, line):
        self.respond("200 Noted.")

    def ftp_CSID(self, line):
        self.respond("200 Name=WP07 Test FTPd; Version=1.2.3;")

    def ftp_OPTS(self, line):
        if line.upper().startswith("HASH"):
            alg = line.partition(" ")[2].upper()
            if alg in HASHES or alg == "CRC32":
                self.hash_alg = alg
                self.respond("200 " + alg)
            else:
                self.respond("501 Unknown algorithm.")
            return
        super().ftp_OPTS(line)

    def local_file(self, arg):
        """The (optionally quoted) path of a command with a path of its own."""
        arg = arg.strip()
        if len(arg) >= 2 and arg[0] == '"' and arg[-1] == '"':
            arg = arg[1:-1]
        path = self.fs.ftp2fs(arg)
        if self.fs.validpath(path) and os.path.isfile(path):
            return path
        self.respond("550 No such file.")
        return None

    def digest(self, path, alg):
        with open(path, "rb") as f:
            data = f.read()
        if alg == "CRC32":
            return "%08x" % (binascii.crc32(data) & 0xFFFFFFFF), len(data)
        return hashlib.new(HASHES[alg], data).hexdigest(), len(data)

    def ftp_HASH(self, line):
        path = self.local_file(line)
        if path is not None:
            value, size = self.digest(path, self.hash_alg)
            self.respond("213 %s 0-%d %s %s" % (self.hash_alg, size, value, self.fs.fs2ftp(path)))

    def single_hash(self, line, alg):
        path = self.local_file(line)
        if path is not None:
            self.respond("213 " + self.digest(path, alg)[0])

    def ftp_XSHA256(self, line):
        self.single_hash(line, "SHA-256")

    def ftp_XMD5(self, line):
        self.single_hash(line, "MD5")

    def ftp_XCRC(self, line):
        self.single_hash(line, "CRC32")

    def ftp_SITE_CPFR(self, line):
        path = self.local_file(line)
        if path is not None:
            self.copy_from = path
            self.respond("350 File exists, ready for destination name.")

    def ftp_SITE_CPTO(self, line):
        path = self.fs.ftp2fs(line)
        if self.copy_from is None or not self.fs.validpath(path):
            self.respond("503 Bad sequence of commands.")
            return
        shutil.copyfile(self.copy_from, path)
        self.copy_from = None
        self.respond("250 Copy successful.")


class Plain(Extras, FTPHandler):
    authorizer = authorizer()
    proto_cmds = extra_commands(FTPHandler.proto_cmds)


class NoMlsd(Plain):
    proto_cmds = {name: value for name, value in Plain.proto_cmds.items() if name not in ("MLSD", "MLST", "HASH")}
    features = [feature for feature in Plain.features if not feature.startswith("HASH")]


class Iis(Plain):
    def ftp_SYST(self, line):
        self.respond("215 Windows_NT")

    def ftp_RNTO(self, path):
        if os.path.exists(path):
            self.respond("550 Cannot create a file when that file already exists.")
            return None
        return super().ftp_RNTO(path)


class FtpProxy(Plain):
    """An FTP firewall in front of ftp.target.test: the logon sequences of
    WinSCP's "FTP proxy" types. Only the right credentials pass; the target
    connection is this very server."""
    proto_cmds = dict(Plain.proto_cmds)
    proto_cmds["ACCT"] = dict(perm=None, auth=False, arg=True, help="Syntax: ACCT")
    proto_cmds["OPEN"] = dict(perm=None, auth=False, arg=True, help="Syntax: OPEN")

    def __init__(self, *args, **kwargs):
        self.pending = None
        super().__init__(*args, **kwargs)

    def pre_process_command(self, line, cmd, arg):
        if cmd == "SITE" and not self.authenticated:
            self.xlog.append(line)
            self.respond("220 Connected to %s." % arg if arg == TARGET else "530 Unknown target.")
            return
        super().pre_process_command(line, cmd, arg)

    def ftp_OPEN(self, line):
        self.respond("220 Connected to %s." % line if line == TARGET else "530 Unknown target.")

    def ftp_USER(self, line):
        if line in (FW_USER, "%s@%s" % (FW_USER, TARGET)):
            self.pending = "firewall"
        elif line in (USER, "%s@%s" % (USER, TARGET)):
            self.pending = "target"
        elif line == "%s@%s %s" % (USER, TARGET, FW_USER):
            self.pending = "target+acct"
        elif line == "%s@%s@%s" % (USER, FW_USER, TARGET):
            self.pending = "target+firewall"
        else:
            self.pending = None
        self.username = USER
        self.respond("331 Password required for %s." % line)

    def login(self):
        self.handle_auth_success(self.authorizer.get_home_dir(USER), PASSWORD, "Logged in.")

    def ftp_PASS(self, line):
        if self.pending == "firewall" and line == FW_PASS:
            self.respond("230 Firewall login accepted.")
        elif self.pending == "target" and line == PASSWORD:
            self.login()
        elif self.pending == "target+acct" and line == PASSWORD:
            self.respond("332 Need account for login.")
            self.pending = "acct"
            return
        elif self.pending == "target+firewall" and line == "%s@%s" % (PASSWORD, FW_PASS):
            self.login()
        else:
            self.respond("530 Login incorrect.")
        self.pending = None

    def ftp_ACCT(self, line):
        if self.pending == "acct" and line == FW_PASS:
            self.login()
        else:
            self.respond("530 Login incorrect.")
        self.pending = None


def tls_handler(minimum, maximum, ciphers=None, client_certificate=False):
    context = SSL.Context(SSL.TLS_SERVER_METHOD)
    context.use_certificate_chain_file(CERTS + "/server-chain.pem")
    context.use_privatekey_file(CERTS + "/server.key")
    context.set_session_id(b"ftpx")
    if minimum is not None:
        context.set_min_proto_version(minimum)
    if maximum is not None:
        context.set_max_proto_version(maximum)
    if ciphers is not None:
        context.set_cipher_list(ciphers)
    if client_certificate:
        context.load_verify_locations(CERTS + "/ca.crt")
        context.set_verify(SSL.VERIFY_PEER | SSL.VERIFY_FAIL_IF_NO_PEER_CERT, lambda conn, cert, errno, depth, ok: ok)

    class Tls(Extras, TLS_FTPHandler):
        authorizer = authorizer()
        proto_cmds = extra_commands(TLS_FTPHandler.proto_cmds)
        ssl_context = context

    return Tls


SERVERS = {
    "extras": (0, lambda: Plain),
    "nomlsd": (1, lambda: NoMlsd),
    "clicert": (2, lambda: tls_handler(None, None, client_certificate=True)),
    "tls12": (3, lambda: tls_handler(SSL.TLS1_2_VERSION, SSL.TLS1_2_VERSION)),
    "tls10": (4, lambda: tls_handler(SSL.TLS1_VERSION, SSL.TLS1_VERSION, b"DEFAULT:@SECLEVEL=0")),
    "iis": (5, lambda: Iis),
    "ftpproxy": (6, lambda: FtpProxy),
}

config_logging(level=logging.WARNING)
offset, handler = SERVERS[MODE]
ThreadedFTPServer(("127.0.0.1", BASE + offset), handler()).serve_forever()
