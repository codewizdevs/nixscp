# Differences from WinSCP

NixSCP is a C++/Qt 6 port of WinSCP's GPL source code. It keeps WinSCP's interface, captions, defaults, keyboard
shortcuts, scripting language and `WinSCP.ini` configuration format wherever they make sense, so WinSCP's own
documentation applies to most of it. This page lists what is different. NixSCP is not affiliated with or endorsed by
the WinSCP project.

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
| Configuration storage | Windows registry (default) or `WinSCP.ini` | Always an INI file in WinSCP's format: `~/.config/nixscp/nixscp.ini` (Linux), `~/Library/Application Support/NixSCP/nixscp.ini` (macOS), `%APPDATA%\NixSCP\nixscp.ini` (Windows), or `/ini=<file>`. Import registry settings from a `.reg` export. |
| Keys and values NixSCP does not use | n/a | Kept untouched, so a configuration that goes WinSCP > NixSCP > WinSCP loses nothing. |
| Stored passwords | Obfuscated, or encrypted with a master password | The same, compatible with WinSCP; with NixSCP Pro the whole file is encrypted with the NixSCP password, which replaces the master password ([encryption.md](encryption.md)). |
| Exports with NixSCP Pro | n/a | Choice of an encrypted backup (NixSCP only) or a plain WinSCP-compatible file. |
| Console client | `winscp.com` | `nixscp-cli`, with the same script commands, switches and exit codes. |
| Authentication agent | Pageant | ssh-agent (`SSH_AUTH_SOCK`) on Linux and macOS; there is no agent choice in Preferences. The Windows build does not support Pageant or the Windows OpenSSH agent yet. |
| Local files | Drive letters, Windows file attributes, Recycle Bin | Linux and macOS paths and permissions; desktop Trash on Linux, the Trash on macOS. |
| Help | Help buttons and F1 open winscp.net | Still open the matching WinSCP documentation page; NixSCP's own features open nixscp.com. The Help menu links to nixscp.com, GitHub issues and GitHub releases. |
| Updates | Built-in update checker and Updates page | A notice about new GitHub releases and Preferences > Updates ([updates.md](updates.md)). |
| Languages | Translations installed separately | WinSCP's translations included, chosen from the system language (Preferences > Languages). |

## Not ported

- **Explorer interface**: NixSCP has the Commander interface only.
- **Encryption of files on the server** (WinSCP's "Encrypt new files" for SFTP).
- **Local-to-local synchronization** (Synchronize between two local folders).
- **WinSCP extensions** (the extension commands on the Commands page of Preferences).
- **Windows integration**: registry storage, shell extensions and the Explorer context menu, jump lists, embedded
  sessions in the executable, the installer maintenance switches (`/Update`, `/UninstallCleanup`,
  `/AddSearchPath`...), usage statistics.
- **.NET assembly and COM** (`WinSCPnet.dll`): use `nixscp-cli` scripts instead.
- **Settings without a Linux equivalent**: e.g. clearing the archive attribute, invalid-character replacement in file
  names, the random seed file, the PuTTY registry key, tips. Their values in `WinSCP.ini` are preserved.
- **KiTTY** session import (KiTTY is Windows-only). "Open in PuTTY" opens your terminal with OpenSSH on Linux, and Terminal.app on macOS.
- Smaller interface details, such as some animations and balloon hints, and drag and drop of bookmarks within the
  bookmark tree (Up/Down/Move To cover it).

Bug reports and feature requests: [GitHub issues](https://github.com/codewizdevs/nixscp/issues).

## Following WinSCP

NixSCP is based on WinSCP 6.7 (`master` at `246bbd42342c`, October 2026). New WinSCP changes, especially bug and
security fixes, are reviewed for NixSCP: a daily job opens a GitHub issue for each new WinSCP commit and release, and
each is ported or closed as not applicable by hand. See [upstream/README.md](../upstream/README.md).
