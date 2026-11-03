<p align="center">
  <img src="docs/screenshots/nixscp-icon.png" width="128" alt="NixSCP icon">
</p>

<h1 align="center">NixSCP — WinSCP for Linux</h1>

<p align="center">
  <b>A native Linux SFTP, SCP, FTP/FTPS, WebDAV and Amazon S3 client with the familiar WinSCP interface.</b><br>
  Same Commander layout, same shortcuts, same settings file — your WinSCP sites, passwords and preferences move over in one click.
</p>

<p align="center">
  <a href="../../releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/codewizdevs/nixscp?label=release"></a>
  <img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-green">
  <img alt="Platform: Linux" src="https://img.shields.io/badge/platform-Linux-orange">
  <img alt="Qt 6" src="https://img.shields.io/badge/Qt-6-41cd52">
  <img alt="Desktops" src="https://img.shields.io/badge/desktop-GNOME%20%7C%20KDE%20%7C%20XFCE-lightgrey">
  <img alt="Languages" src="https://img.shields.io/badge/languages-55-blueviolet">
</p>

---

**NixSCP** is a free, open-source **WinSCP alternative for Linux** — not a Wine wrapper, but a native C++/Qt 6 rewrite of WinSCP's code base. If you moved from Windows to Ubuntu, Fedora, Debian, Linux Mint, Arch or openSUSE and miss WinSCP, NixSCP gives you the same two-panel file manager for secure file transfer, with the same keyboard shortcuts (F5 Copy, F6 Move, F7 New folder, F8 Delete…), dialogs and behaviour — and it reads and writes WinSCP's own configuration format.

## Documentation

WinSCP's own documentation covers everything NixSCP shares with WinSCP. The features NixSCP adds are documented in
[docs/](docs/README.md): [Agent Access](docs/agent-access.md), [MCP server](docs/mcp.md),
[Tunnels](docs/tunnels.md), [Cloud sync](docs/cloud-sync.md), [Local encryption](docs/encryption.md),
[Teams](docs/teams.md), [New-release check](docs/updates.md) and
[Differences from WinSCP](docs/differences-from-winscp.md). Online: [nixscp.com/docs](https://nixscp.com/docs).

## Why NixSCP?

- 🖥️ **WinSCP interface, natively on Linux** — the Commander two-panel layout, Login/Site Manager, Copy/Move dialogs, transfer queue, Synchronize, Find Files and Preferences you already know. No Wine, no lag.
- 🔁 **1:1 WinSCP data compatibility** — sites, folders, stored passwords, workspaces, bookmarks, custom commands, transfer settings presets and preferences use **WinSCP's own `WinSCP.ini` format and key names**. Import from Windows, export back to Windows, keep using both.
- 🔑 **Your keys just work** — PuTTY `.ppk` private keys (v2 and v3, RSA, Ed25519, ECDSA), OpenSSH keys, ssh-agent, password, keyboard-interactive.
- 📜 **Scripting like `winscp.com`** — `nixscp-cli` runs WinSCP scripts with the same commands, switches and exit codes (`open`, `get`, `put`, `synchronize`, `keepuptodate`, `call`, `option batch`…).
- ⚡ **Fast** — pipelined SFTP, directory cache, parallel background transfers, segmented downloads of large files, batched SCP; large-file SFTP throughput on par with or above OpenSSH's own client.
- 🌍 **55 languages** — WinSCP's translations, picked automatically from your system language (Preferences → Languages to change).
- 🐧 **Feels at home on Linux** — GNOME, KDE Plasma and XFCE; Wayland and X11; desktop Trash, `xdg-open`, your terminal for "Open in PuTTY", `sftp://` link handling.

## WinSCP compatibility: import & export across versions

NixSCP stores everything in **one WinSCP.ini-compatible file** (`~/.config/nixscp/nixscp.ini`). Section names, key names, value encodings and password obfuscation are byte-compatible with WinSCP, so settings travel in both directions:

| From | To | How |
|---|---|---|
| WinSCP (portable / INI storage) | NixSCP | Login → Tools → **Import Sites…** and pick `WinSCP.ini`, or **Import/Restore Configuration** |
| WinSCP (registry storage, the default on Windows) | NixSCP | On Windows: `reg export "HKCU\Software\Martin Prikryl\WinSCP 2" winscp.reg`, then import the `.reg` file in NixSCP |
| NixSCP | WinSCP | Login → Tools → **Export/Backup Configuration…** — the result is a regular `WinSCP.ini` that WinSCP imports |
| PuTTY / OpenSSH / FileZilla | NixSCP | Site import from PuTTY sessions, `~/.ssh/config`, `known_hosts` and FileZilla site manager |

What carries over: sites and site folders, host/port/protocol/user, **stored passwords** (WinSCP's obfuscation and master-password encryption), private key paths, advanced site settings (proxy, tunnel, SSH algorithms, keepalives, environment, directories…), workspaces, bookmarks, custom commands, transfer settings presets and preferences. Settings that NixSCP does not use on Linux are **preserved untouched**, so a file that goes Windows → Linux → Windows loses nothing.

## Features

**Protocols**
- SFTP (SSH File Transfer Protocol) and SCP over SSH, with automatic SCP fallback
- FTP, FTPS (explicit and implicit TLS), active and passive mode, MLSD/MLST and classic `LIST` parsing
- WebDAV / WebDAVS (HTTP/HTTPS, Basic and Digest authentication)
- Amazon S3 and S3-compatible storage (AWS Signature V4, region detection, multipart upload)
- SSH proxies (SOCKS4/5, HTTP, local command), SSH tunnel / jump host, host key verification via `known_hosts`, TLS certificate verification

**File management**
- WinSCP Commander interface: local and remote panels, session tabs, local-local tabs, address bar, history, bookmarks
- Copy, move, rename, delete (remote recycle bin, local desktop Trash), duplicate, move to, symbolic and hard links
- Properties: permissions (octal or checkboxes, recursive), owner/group, checksums, directory sizes
- Edit remote files in the internal editor or any external editor; saved changes upload automatically through the transfer queue
- Drag & drop between panels and to/from your desktop file manager, file clipboard (Ctrl+C / Ctrl+V)
- Find files, filters and masks with full WinSCP mask syntax (size and date conditions), compare directories

**Transfers**
- Background transfer queue with multiple connections, pause/resume, reordering, speed limits, "once empty" actions, Proceed in Background
- Resume of interrupted transfers (`.filepart`), transfer settings presets, text/binary/automatic mode, file name case, include/exclude masks
- Synchronize (local ↔ remote, both, mirror, preview checklist) and **Keep remote directory up to date** (inotify)

**Automation & tools**
- `nixscp-cli` — WinSCP-compatible scripting and command-line client
- Custom commands with WinSCP's patterns (`!`, `!&`, `!/`, `!@`, `!U`, `!?prompt?default!`…)
- Console (run remote shell commands), "Open in PuTTY" with your terminal and OpenSSH (optionally passing the session password)
- Workspaces, session URLs (`sftp://user@host/path`), session logs and WinSCP-compatible XML action logs
- Context help (Help buttons and F1 open the matching WinSCP documentation page)

## Installation

### Packages

Download the package for your distribution from the [latest release](../../releases/latest):

| Distribution | Package |
|---|---|
| Ubuntu 26.04 | `nixscp_<version>_ubuntu26.04_amd64.deb` |
| Debian 13 | `nixscp_<version>_debian13_amd64.deb` |
| Fedora 42 / 43 | `nixscp-<version>.fc42.x86_64.rpm` / `.fc43` |
| Other distributions (Ubuntu 24.04+, Linux Mint, Arch, openSUSE…) | `NixSCP-<version>-x86_64.AppImage` |
| Windows 10 / 11 (64-bit) | `NixSCP-<version>-setup.exe` (installer) or `NixSCP-<version>-windows-x64.zip` (portable) |

```bash
sudo apt install ./nixscp_*_amd64.deb      # Ubuntu / Debian
sudo dnf install ./nixscp-*.x86_64.rpm     # Fedora
chmod +x NixSCP-*-x86_64.AppImage && ./NixSCP-*-x86_64.AppImage   # any other distribution
```

Release packages use your desktop icon theme. The AppImage bundles Qt, libssh and OpenSSL and runs on distributions with glibc 2.39 or newer. Building from source needs **libssh 0.11 or newer** and **OpenSSL 3.2 or newer**.

On Windows, the installer can add `nixscp-cli` to `PATH`; the portable zip runs from any folder. Settings are stored in `%APPDATA%\NixSCP`.

### Build from source

Dependencies (Ubuntu/Debian names):

```bash
sudo apt install build-essential cmake pkgconf qt6-base-dev libssh-dev libssl-dev
```

Fedora: `sudo dnf install gcc-c++ cmake pkgconf qt6-qtbase-devel libssh-devel openssl-devel`
Arch: `sudo pacman -S base-devel cmake pkgconf qt6-base libssh openssl`

```bash
git clone https://github.com/codewizdevs/nixscp.git
cd nixscp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/nixscp
sudo cmake --install build      # optional: menu entry, icons, sftp:// handler
```

Build your own packages with `cd build && cpack -G DEB` (or `-DCPACK_GENERATOR=RPM` at configure time and `cpack -G RPM` on Fedora).

### Building for Windows

The Windows version is cross-compiled on Linux with llvm-mingw, Qt for Windows (llvm-mingw build) and OpenSSL/libssh built for Windows; see `cmake/windows-llvm-mingw.cmake` for the expected `win-sdk/` layout. `packaging/build-windows.sh` builds the portable folder, the zip and (with NSIS) the installer into `bin/windows/`.

### Building with WinSCP's original icons

The repository includes WinSCP's original toolbar and dialog icon set (`resources/icons`, `resources/animations`, `resources/dialogs`). It is **off by default** — the default build uses your desktop icon theme. To build with the original WinSCP look:

```bash
cmake -S . -B build-winscp-icons -DCMAKE_BUILD_TYPE=Release -DNIXSCP_WINSCP_ICONS=ON
cmake --build build-winscp-icons -j"$(nproc)"
./build-winscp-icons/nixscp
```

The icon set is © its respective owners and is not covered by NixSCP's GPL license (see `resources/icons/README`). `tools/extract_glyphs.py` regenerates it from a WinSCP source checkout.

## Usage

```bash
nixscp                                  # Login dialog / Site Manager
nixscp "My Server"                      # open a stored site (or a workspace)
nixscp sftp://user@example.com/var/www  # open a session URL

# WinSCP-compatible scripting
nixscp-cli /command "open sftp://user@example.com/" "put report.pdf /srv/" "exit"
nixscp-cli /script=deploy.txt /log=deploy.log
```

Your existing WinSCP scripts (`winscp.com /script=…`) generally run unchanged with `nixscp-cli`.

## Cloud sync (NixSCP Pro beta)

Optional end-to-end encrypted sync of sites, stored passwords, preferences, workspaces, bookmarks and open sessions between your computers (Linux and Windows): **Cloud Sync › Sign In…** in NixSCP. Everything is encrypted on your computer (Argon2id + AES-256-GCM) with a sync password that never leaves it; the server only stores ciphertext. Sync is **free during the beta**; afterwards it becomes a paid NixSCP Pro feature and NixSCP tells you in the app. NixSCP itself stays free and open source. Details: [nixscp.com/security](https://nixscp.com/security).

## Agent Access

Let AI agents and other programs use your saved SSH sites **without ever getting their passwords or keys**. Enable it per site with **Agent Access...** in the site settings: off, allowed, or ask me each time (allow once, for 1 hour, for 24 hours, or always). The running NixSCP application makes the connection; agents only use these commands:

```bash
nixscp --list                                    # sites the agent may use
nixscp --connect web1 --cmd "df -h"              # one command: output and exit code
nixscp --connect web1                            # interactive shell (in a terminal)
nixscp --get web1 /var/log/app.log ./app.log     # download a file
nixscp --put web1 ./build.tar.gz /tmp/           # upload a file
nixscp --tunnel db1 5433:localhost:5432          # port forwarding while it runs
nixscp --help                                    # instructions written for agents
```

**MCP server:** `nixscp --mcp` serves the same through the Model Context Protocol (tools `list_sites`, `run_command`, `download_file`, `upload_file`), e.g. `claude mcp add nixscp -- nixscp --mcp`.

Everything is allowed by default; per site you can optionally:
- choose what agents may do: commands, downloads, uploads, tunnels;
- set **command rules**: allowed and denied patterns (`systemctl status *`), checked for every part of a command (`;`, `&&`, `|`...), with a read-only preset. Rules are a safety net, not a sandbox;
- give the site a different **name for agents**, hiding your folder and client names;
- keep **session recordings** (on by default): every command and its output, in Commands › Agent Sessions.

Free for everyone. Requires NixSCP running, with your saved passwords encrypted: a **master password** (Preferences › Security) or, with NixSCP Pro, your **NixSCP password**, which encrypts the whole configuration. Every request is written to the agent access log (Commands › Agent Access Log). Private key files should have a passphrase: any program you run can read an unprotected key file directly.

## Tunnels

**Tunnels › Manage Tunnels...** keeps saved port forwards per site (e.g. the server's PostgreSQL on `127.0.0.1:5433`), started and stopped with one click; they listen on this computer only. Agents open them with `nixscp --tunnel <site> <name>` when the site allows it.

## Teams (NixSCP Pro beta)

Share site folders with colleagues, end-to-end encrypted: **Pro › Teams...**. A team's sites appear as `Team: <name>` in the site tree; owners and admins edit them, members use them. Invite with one-time codes (the code's secret never reaches the server); removing a member renews the team key and removes the sites from their NixSCP. Agent access stays each person's own setting.

## Status

**Version 1.2.** NixSCP ports WinSCP's features, captions, defaults and shortcuts one-to-one where they make sense on Linux. Windows-only features without a sane Linux equivalent (registry storage, drive letters, Windows file attributes, shell extensions) are replaced by Linux equivalents or removed. Bug reports and feature requests are welcome in [Issues](../../issues).

## FAQ

**Is this WinSCP?** No. NixSCP is an independent port based on WinSCP's GPL source code. It is not affiliated with or endorsed by the WinSCP project. On Windows, [WinSCP](https://winscp.net/) itself is the original; the Windows version of NixSCP is for people who also use NixSCP on Linux and want the same application and synced settings everywhere.

**Can I keep using WinSCP on Windows and NixSCP on Linux with the same sites?** Yes — export from one, import into the other; the file format is the same.

**Does it need Wine?** No. NixSCP is a native Linux application built with Qt 6, libssh and OpenSSL.

**Which Linux desktops are supported?** GNOME, KDE Plasma, XFCE and others, on Wayland and X11.

## Contributing

Pull requests are welcome. The code follows WinSCP's structure and naming (`src/core` ≈ WinSCP `core/`, `src/gui` ≈ `forms/` + `windows/`, `src/cli` ≈ `winscp.com`), so the matching WinSCP source is the reference for behaviour. Run the test suite with `QT_QPA_PLATFORM=offscreen ctest --test-dir build`; protocol tests run against local test servers when their `NIXSCP_TEST_*` environment variables are set (see `tests/`).

## License

NixSCP is free software under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).

NixSCP is based on **[WinSCP](https://winscp.net/)**, Copyright © 2000–2026 Martin Prikryl, licensed under the GPL. Translations are WinSCP's community translations (see `resources/translations/README`). Portions © The Qt Company, libssh authors and the OpenSSL Project.

## Author

**Dragan Plavšić** — [CodeWiz](https://codewizdev.com/), Banja Luka, Bosnia and Herzegovina · [hello@codewizdev.com](mailto:hello@codewizdev.com)

<sub>Keywords: WinSCP for Linux, WinSCP alternative, WinSCP Linux, SFTP client for Linux, SCP client, FTP client, FTPS, WebDAV client, S3 client, Ubuntu SFTP GUI, file transfer, two-panel file manager, Commander, Qt.</sub>
