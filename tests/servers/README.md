# Test servers

Many tests need a server and skip themselves without one, so a plain `ctest` run says little about the protocols.
A test that skips still counts as passed. These helpers make those tests run. They listen on 127.0.0.1 only and are
for test machines.

| Script | Serves | Used by |
|---|---|---|
| `ftpsrv.py` | FTP, explicit and implicit FTPS, FTP without MLSD (one process each) | test_ftp, test_verify_protocols, test_auxdialogs |
| `davsrv.py` | WebDAV with Basic, over TLS, and with Digest, plus the redirect paths | test_webdav, test_verify_protocols |
| `ftpx.py`, `proxy.py`, `mkcerts.sh` | The extra FTP servers, SOCKS and HTTP proxies, and certificates | test_ftpex |
| `evil_ftp.py`, `evil_dav.py`, `evil_sftp.py` | Malicious servers (hostile file names, CR/LF, path traversal) | test_verify_security |
| `naive_client.py` | A client with no name checks: proves the malicious servers really attack | run by hand |

`ftpsrv.py`, `davsrv.py`, `ftpx.py` and `proxy.py` need a Python environment with `pyftpdlib`, `pyopenssl`, `wsgidav` and
`cheroot`. The `evil_*` scripts use only the standard library, because the test starts them with `python3 -I`.

Each test's header comment lists the `NIXSCP_TEST_*` variables it reads. `NIXSCP_TEST_EVIL_SERVERS` points to this folder.

Things worth knowing:

- The SSH tests need an OpenSSH server running as your user on this machine. A non-root sshd works: extract
  `openssh-server` and `openssh-sftp-server` with `apt-get download` and `dpkg -x`, and start `sshd` by its absolute path.
  Set `PerSourcePenalties no`, or failed logins in one test block the next.
- Unix socket paths have a length limit: use a short `TMPDIR` and a private `XDG_RUNTIME_DIR`.
- Moto stands in for S3 but ignores credentials, and `rclone serve s3` checks them but stores no folder markers.
  Neither passes every check in test_s3, and wsgidav cannot pass every check in test_webdav.
- `pyftpdlib`'s threaded server changes the process's working directory, so never delete a folder on a shared server
  while it may be in use.
- Close to the limit of inotify instances, the Keep Up To Date tests fail with a watcher limit error.
- test_puttykey needs the public keys of `tests/data/ppk/*.pub` in the test server's `authorized_keys`.
- Live cloud tests (test_cloudlive, test_teamslive) need fresh accounts for each run, and the API's limit of 30 token
  requests a minute will fail them if it is on. test_agentaccess skips silently without its `NIXSCP_TEST_AGENT_*` variables.
- `NIXSCP_TEST_SECURITY_STRICT=1` makes known weaknesses fail in test_verify_security.
- Run the tests as a normal user. Root ignores file permissions, so the tests of a failed save fail under root.
- On macOS: test programs get their own `HOME` (tests/MacTestHome.cpp), BSD tools differ from GNU ones, and Python's bundled
  LibreSSL cannot serve an EC certificate, so test certificates are RSA.
