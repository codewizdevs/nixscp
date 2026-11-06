# Differences from WinSCP

NixSCP is a C++/Qt 6 port of WinSCP's GPL source code. It keeps WinSCP's interface, captions, defaults, scripting
language and `WinSCP.ini` configuration format wherever they make sense, and its keyboard shortcuts for the features it
has, so WinSCP's own documentation applies to most of it. This page lists what is different. NixSCP is not affiliated
with or endorsed by the WinSCP project.

## Added in NixSCP

| Feature | Summary | Availability |
|---|---|---|
| Native Linux application | Qt 6, Wayland and X11, GNOME/KDE Plasma/XFCE, desktop Trash, `xdg-open`, your terminal for "Open in PuTTY", `sftp://` link handling, inotify for "Keep remote directory up to date" | Free |
| Windows build | The same application for Windows 10 and 11 (installer and portable zip), for people who use NixSCP on both systems | Free |
| macOS version | A universal app for Apple Silicon and Intel Macs (macOS 12 or newer); not signed/notarized yet, so the first start needs right-click › Open | Free |
| [Agent Access](agent-access.md) | AI agents and other programs use saved SSH sites through NixSCP without getting passwords or keys: per-site permissions, approval, command rules, recordings, audit log; `nixscp --list/--connect/--get/--put/--tunnel` | Free |
| [MCP server](mcp.md) | `nixscp --mcp` for MCP clients such as Claude Code | Free |
| [Tunnels](tunnels.md) | Saved SSH local port forwards per site, started and stopped in Tunnels > Manage Tunnels... | Free |
| [New-release check](updates.md) | A notice when a new release is on GitHub; never installs anything | Free |
| [Cloud sync](cloud-sync.md) | End-to-end encrypted sync of sites, passwords, preferences, workspaces, bookmarks and open tabs between computers | NixSCP Pro, free during the beta |
| [Local encryption](encryption.md) | The whole configuration encrypted with the NixSCP password, recovery key, encrypted backups | NixSCP Pro, free during the beta |
| [Teams](teams.md) | Shared, end-to-end encrypted site folders with roles and one-time invite codes | NixSCP Pro, free during the beta |
| Imports | Sites from OpenSSH `~/.ssh/config` and `known_hosts` (plus PuTTY and FileZilla, as in WinSCP), WinSCP `.reg` exports | Free |

## Changed

| Area | WinSCP | NixSCP |
|---|---|---|
| Configuration storage | Windows registry (default) or `WinSCP.ini` | An INI file in WinSCP's format: `~/.config/nixscp/nixscp.ini` (Linux), `~/Library/Application Support/NixSCP/nixscp.ini` (macOS), `%APPDATA%\NixSCP\nixscp.ini` (Windows), or `/ini=<file>`. Import registry settings from a `.reg` export. SSH host keys are the exception, see the next line. |
| SSH host keys | Stored with the configuration | In `~/.ssh/known_hosts`, through libssh, even with `/ini=nul`. |
| OpenSSH configuration | Not used | `~/.ssh/config` is not read when connecting, and the default keys `~/.ssh/id_*` are not tried: a site uses ssh-agent or its own key. Bring hosts in as sites with Site Import. |
| Keys and values NixSCP does not use | n/a | Kept untouched, so a configuration that goes WinSCP > NixSCP > WinSCP loses nothing. |
| Stored passwords | Obfuscated, or encrypted with a master password | The same, compatible with WinSCP; with NixSCP Pro the whole file is encrypted with the NixSCP password, which replaces the master password ([encryption.md](encryption.md)). |
| Exports with NixSCP Pro | n/a | Choice of an encrypted backup (NixSCP only) or a plain WinSCP-compatible file. |
| Console client | `winscp.com` | `nixscp-cli`, with the same script commands, switches and exit codes, including `-append`, `-hostkey=acceptnew`, `/keygen`, `/copyid` and `copyid`, apart from the differences listed below. |
| Authentication agent | Pageant | ssh-agent (`SSH_AUTH_SOCK`) on Linux and macOS; there is no agent choice in Preferences. The Windows build has no SSH agent support: neither Pageant nor the Windows OpenSSH agent. |
| Local files | Drive letters, Windows file attributes, Recycle Bin | Linux and macOS paths and permissions; desktop Trash on Linux, the Trash on macOS. |
| Help | Help buttons and F1 open winscp.net | Still open the matching WinSCP documentation page; NixSCP's own features open nixscp.com. The Help menu links to nixscp.com, GitHub issues and GitHub releases. |
| Updates | Built-in update checker and Updates page | A notice about new GitHub releases and Preferences > Updates ([updates.md](updates.md)). |
| Languages | Translations installed separately | WinSCP's translations included, chosen from the system language (Preferences > Languages). |

Also different, or worth knowing:

- **`/keygen`** writes `.ppk` files only, has no `/certificate`, and refuses keys in ssh.com format.
- **`-hostkey`** is authoritative, which is stricter than WinSCP: a key that does not match it is refused, also when
  `known_hosts` knows the key.
- **Targets in scripts:** an existing directory counts as a directory with or without a trailing slash (`get`, `put`,
  `mv`, `cp`).
- **Script `synchronize`** is case-sensitive by default on Linux. `/rawconfig Interface\SynchronizeParams` applies.
- **URLs** never expand `$VAR` or `%VAR%`.
- **Host keys and URLs:** `*` and `acceptnew` work only from `-hostkey`, `-certificate`, `-rawsettings` or a site, never
  from a URL. A fingerprint in a clicked link is ignored, and the normal prompt appears. A fingerprint from a pasted URL
  or the command line is never saved and does not count under `DisableAcceptingHostKeys`.
- **Clicked links** take only the URL: anything else on the command line is dropped. They apply only a short list of
  `x-` settings. A pasted URL with other `x-` settings asks first.
- **`-rawsettings` and `/batchsettings`** cannot set Agent Access settings or `PortForwards`.
- **Forwarded command lines** take only a site or URL: session switches start a new instance.
- **Uploads:** when the temporary `.filepart` file cannot be created, NixSCP asks before writing directly over an
  existing file, unless the recycle bin keeps the old one. In a script without anyone to ask, the file is skipped.
- **Custom commands:** patterns without quotes around them are quoted, in local and remote commands.
- **Proxy commands:** a target host with shell syntax is refused.
- **WebDAV:** redirects to another domain and unencrypted redirects are never followed.
- **Connecting through an SSH tunnel** works for SFTP and SCP sites; on FTP, WebDAV and S3 sites it is refused.
- **SSH algorithms** are those of libssh: RSA key exchange, `diffie-hellman-group15` and `group17`, DES, Arcfour and
  Ed448 are absent.
- **Password change on expiry** works through keyboard-interactive only.
- **New files and folders** get the server's default rights.
- **Masks** starting with `./` work.

## Windows and macOS

Some of what the Linux version does is missing elsewhere.

| Feature | Windows | macOS |
|---|---|---|
| Open in PuTTY | OpenSSH in a console | OpenSSH in Terminal.app |
| Sleep or Shut Down when the queue is done | No | No |
| Prevent sleep during transfers | No | No |
| Taskbar progress | No | No |
| Desktop Icon, registering as URL handler | Yes | No |
| Desktop shortcuts for sites and workspaces | No | No |
| SSH agent | No | Yes |

Also on Windows:

- Names in a server listing that would leave their folder when read as a Windows path are hidden: `..\x`,
  `\\server\share`, `a:b.txt`, `.. `. Reserved names such as `CON` get a different local name when downloaded.
- `"` is left out of file names in local custom commands, where it would end the quoting.
- There is no local proxy command: a site set to use one does not connect.

## macOS and BSD servers

- Machine-wide settings are read from `/Library/Application Support/NixSCP/nixscp.ini` on macOS, and from `$XDG_CONFIG_DIRS` where it is set.
- The macOS app uses its bundled OpenSSL for TLS.
- Checksums over a shell fall back to `shasum -a N` and `md5 -r` on servers without `sha256sum` and `md5sum`.
- SCP to a BSD or macOS server keeps the precision of its `ls`, which may show a date without a time.
- Names that differ only in Unicode form count as the same file on macOS.

## Not ported

- **Explorer interface**: NixSCP has the Commander interface only.
- **Parts of the Commander interface**: the directory tree in both panels (Ctrl+Alt+T), the command line (Ctrl+Shift+M,
  Ctrl+Enter), the thumbnail view and toolbar customization. The options for the tree and the system context menu are
  gone as well.
- **Session commands**: Change Password, and Install Public Key on an open session. The Generate Code option of the
  transfer dialogs, custom commands in the synchronization checklist, and the full certificate view.
- **WebDAV**: locking, `COPY`, quota, client certificates and Negotiate authentication.
- **S3**: ACLs, tags and credentials from instance metadata. The `AWSAPI` and `HttpsCertificateValidation` settings.
- **SSH**: host CAs, SFTP above version 3, GSSAPI key exchange and the choice of GSSAPI library, the SSH bug switches
  other than Rekey and Ignore, `RekeyTime`, single-DES, DST modes and the local port of a tunnel.
- **Encryption of files on the server** (WinSCP's "Encrypt new files" for SFTP).
- **Local-to-local synchronization** (Synchronize between two local folders).
- **WinSCP extensions** (the extension commands on the Commands page of Preferences).
- **Windows integration**: registry storage, shell extensions and the Explorer context menu, jump lists, embedded
  sessions in the executable, the installer maintenance switches (`/Update`, `/UninstallCleanup`,
  `/AddSearchPath`...), usage statistics.
- **.NET assembly and COM** (`WinSCPnet.dll`): use `nixscp-cli` scripts instead.
- **Settings without a Linux equivalent**: e.g. clearing the archive attribute, invalid-character replacement in file
  names, the random seed file, the PuTTY registry key, tips. Their values in `WinSCP.ini` are preserved.
- **KiTTY** session import (KiTTY is Windows-only). "Open in PuTTY" opens your terminal with OpenSSH on Linux, Terminal.app on macOS and a console on Windows.
- Smaller interface details, such as some animations and balloon hints, and drag and drop of bookmarks within the
  bookmark tree (Up/Down/Move To cover it).

Bug reports and feature requests: [GitHub issues](https://github.com/codewizdevs/nixscp/issues).

## Following WinSCP

NixSCP is based on WinSCP 6.7 (`master` at `246bbd42342c`, October 2026). New WinSCP changes, especially bug and
security fixes, are reviewed for NixSCP: a daily job opens a GitHub issue for each new WinSCP commit and release, and
each is ported or closed as not applicable by hand. See [upstream/README.md](../upstream/README.md).
