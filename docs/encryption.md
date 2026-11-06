# Local encryption and the NixSCP password (NixSCP Pro)

WinSCP stores site passwords in its configuration either obfuscated (readable by anyone who has the file and knows
the scheme) or encrypted with an optional **master password**. Everything else, including host names, user names
and site names, is plain text.

With NixSCP Pro (free during the beta), NixSCP encrypts **the whole configuration** on disk instead, with your
**NixSCP password**. This is the same password as your [cloud sync](cloud-sync.md) password: one password protects
your synced data and your local files.

## How it is turned on

Encryption is on automatically whenever NixSCP has your sync key: after you set up or unlock
[cloud sync](cloud-sync.md) on a computer, NixSCP rewrites the configuration encrypted. There is no separate switch.

From then on:

- **NixSCP asks for the NixSCP password when it starts**, before it reads the configuration. You can tick
  **Use my recovery key instead** and enter the recovery key. **Quit** closes NixSCP without opening anything.
- If NixSCP is already running and unlocked, opening a site, URL or file from the command line or the desktop is
  handed to the running instance without asking again.
- The key stays in memory, with the limits described under [Keys in memory](#keys-in-memory). It reaches a file only
  sealed, while a key is being changed (`nixscp-cloud-keys.ini`, `nixscp-cloud-bridge.ini`).

If you changed the password on another computer, this computer still uses the previous password until it syncs
again. A plain password change leaves the recovery key valid. A recovery key that was replaced, or a data key that was
replaced, still opens only what was encrypted before: an old local file on a computer that has not synced yet, or an
old backup ([cloud-sync.md](cloud-sync.md#changing-the-password-recovery-signing-out)). A computer that reconnects after
the data key was replaced elsewhere asks for the current password once, and its local files move to the new key then.

## What is encrypted, and what is not

Encrypted (the format is the same NXS1 format as cloud snapshots, AES-256-GCM with the sync data key, unlocked by
the password through Argon2id or by the recovery key):

| File (next to the configuration) | Contents |
|---|---|
| `nixscp.ini` | Sites, stored passwords, preferences, workspaces, bookmarks, custom commands, host key fingerprints configured in sites |
| `nixscp-cloud.ini` | Cloud account e-mail, server address, sign-in (refresh) token, sync state and the data key wrapped with your password and recovery key |
| `nixscp-cloud-keys.ini` | Key changes under way: a new data key, old keys, team keys on their way to the server, a recovery key you have not confirmed yet |
| `nixscp-cloud-bridge.ini` | The old data key, only while a new key reaches the local files |
| `nixscp-cloud-session.ini` | The cached open tabs of cloud sync |
| `nixscp-teams.ini` | Team names and team sync state, the pins that detect tampering (identity keys, and the member list and content last seen) and old team keys |
| `backups/nixscp-*.ini` | Copies made before cloud settings were applied |
| `agent-sessions/*.log` | [Agent Access](agent-access.md) session recordings |

Before a key exists, `nixscp-cloud.ini` holds only this computer's sync options. A sign-in that waits for the sync
password stays in memory: if NixSCP exits then, you sign in again.

Each encrypted file names its role in its authenticated header, so one file cannot stand in for another. NixSCP refuses
to start with a plain or missing configuration when the encrypted `nixscp-cloud.ini` beside it belongs to that
configuration, because it may have been replaced. Another file in the same folder, given with `/ini=`, can be plain.

**Files from NixSCP 1.2.3 or older.** This version cannot read them, and they cannot be converted. NixSCP says so at
startup, with the files to delete to start over; 1.2.3 still opens them ([updates.md](updates.md#updating)).

**If someone gets the files.** A stolen disk or copied folder gives only the headers in readable form: the account's
uuid, the password salt and the wrapped keys. These allow guessing the password offline, at Argon2id's cost, and
nothing else opens without it, so a strong password matters. Someone who can write to the folder can still put an
older encrypted copy of a file back, or delete everything.

**On Windows** the configuration, logs, recordings and backups get a private ACL: your user and SYSTEM, with nothing
inherited from the folder. FAT, exFAT and some network shares take no ACL, so only the encryption protects files there.

Not encrypted:

| What | Why / protection |
|---|---|
| `nixscp-agent.log` | Agent Access audit log: times, site names, results and the first 500 characters of each command as it was sent, so a secret typed into a command lands here. No stored passwords, keys or output. Readable by your user only. |
| `nixscp-agent-seals` | With a master password only: the seals of Agent Access settings ([agent-access.md](agent-access.md#settings-prompt-master-password-only)). Readable by your user only. |
| `~/.ssh/known_hosts` | SSH host keys you accepted (OpenSSH format). |
| Private key files | Your `.ppk` or OpenSSH key files are not touched; protect them with a passphrase. |
| Session logs, XML logs, temporary and edited files | Written where you configured them, as in WinSCP. |
| Exports you choose to make as plain files | See below. |

The password NixSCP hands to a terminal (**Open in PuTTY**, when it is set to pass the session password) does not touch
the disk. `ssh` gets it from the running NixSCP, once, over a local socket, and only with a one-time token that travels
in the terminal's process environment. A terminal that does not pass that environment on asks for the password in the
terminal. A process of your user that can read `/proc/<pid>/environ` can still get the token. On macOS the token sits in
a private `.command` script for about a second, until the Terminal window opens.

## Keys in memory

- The data key is kept in memory of its own, locked against swapping where the system allows it, left out of core
  dumps and overwritten when released. Not every secret is covered: site passwords in use, for example, are ordinary
  memory.
- **Linux:** while a key is loaded, NixSCP turns core files off and closes itself to debuggers and other programs of
  your user. In sessions using xdg-desktop-portal (including Flatpak and Snap) the second part stays off, because
  portals would refuse NixSCP, so the process stays debuggable by the same user.
- **macOS:** core files are off, and NixSCP refuses debuggers where the system allows it. The app is not signed, so a
  program started with `DYLD_INSERT_LIBRARIES` or under a debugger gets around that.
- **Windows:** Windows Error Reporting makes no dump of a crash, and the data key is excluded from other reports where
  Windows supports it. **A program of your user can still read NixSCP's memory.**
- This is no defense against malware running as you.

## The master password is retired

When encryption is on, the NixSCP password replaces WinSCP's master password: a master password that is set and
known is removed (site passwords are then protected by the encryption of the whole file), and
**Preferences > Security** says that the configuration is encrypted with your NixSCP password. Change the password
in **Pro > Cloud Sync Settings... > Change NixSCP password...**.

Configurations imported or synced from a computer that still uses a master password are converted the same way.

## Exporting and importing

**Login > Tools > Export/Backup Configuration...** asks how to export while encryption is on:

- **Encrypted backup** (default): opens in NixSCP on any computer with your NixSCP password or recovery key. It is as
  strong as that password: whoever has the file can test guesses offline. WinSCP cannot read it. Written readable by
  your user only.
- **Plain file for WinSCP**: a normal WinSCP-compatible configuration that WinSCP and NixSCP can import. Site
  passwords in it are only obfuscated (no master password), so anyone who gets the file can read them. You are then
  also asked whether to include the cloud sync key ([cloud-sync.md](cloud-sync.md#configuration-backups-with-the-sync-key)).

**Import/Restore Configuration...** recognizes both. An encrypted backup made with the current key opens without a
question; one made with another password or another account asks for the NixSCP password it was made with, or its
recovery key. Importing it does not change your current password or key. A backup cannot replace `nixscp.ini` by
copying it there: NixSCP refuses it, so use Import. An encrypted backup from NixSCP 1.2.3 or older opens only in 1.2.3.

## Recovery key

The recovery key is created when you set up cloud sync on your first computer (**Create a recovery key
(recommended)**) and shown once: 13 groups of 4 characters. Typing mistakes between `O`/`0` and `I`/`L`/`1`, and
upper or lower case, are tolerated. It unlocks:

- NixSCP at startup (**Use my recovery key instead**),
- `nixscp-cli` (see below),
- encrypted backups,
- cloud sync on a new computer, after which you choose a new password.

**New recovery key...** in Cloud Sync Settings replaces it
([cloud-sync.md](cloud-sync.md#changing-the-password-recovery-signing-out)).

There is no reset: without the password and the recovery key, NixSCP support cannot open your encrypted files or
recover them. Keep it somewhere safe, away from the computer.

## nixscp-cli and scripts

`nixscp-cli` reads the same configuration. When it is encrypted, it prompts:

```
NixSCP password (or recovery key):
```

You get three attempts. If the configuration cannot be unlocked (no password given, or no interactive input),
`nixscp-cli` stops with exit code 1 and a message. For unattended scripts that do not need stored sites, use
`/ini=nul` and give the connection details in the script (for example `open sftp://user@host/ -privatekey=...`), or
use a separate, unencrypted configuration file with `/ini=<file>`.

The [Agent Access](agent-access.md) commands (`nixscp --connect` etc.) never ask for the password: they use the
running, unlocked NixSCP.

## Removing encryption

After **Sign Out**, the Cloud Sync window offers **Remove encryption from this computer...**, which asks for the
NixSCP password or recovery key. The configuration is then stored unencrypted again, NixSCP no longer asks for the
password at startup, and saved site passwords are only
obfuscated, as in WinSCP (set a master password in Preferences > Security if you want them encrypted). Signing in to
NixSCP Pro encrypts the configuration again.

[Agent Access](agent-access.md) needs either this encryption or a master password.

## On Android

With NixSCP Pro the Android app encrypts the same files as the desktop, plus:

| File | Contents |
|---|---|
| `sites.dat` | Favourites, recent sites and folders, folded folders and the key chosen for each site (it replaces the site data that used to be in `mobile.ini`) |
| Private SSH keys | Encrypted on disk; a decrypted key exists in memory only |

What stays in plain text at rest, in the app's private storage:

| What | Why / how long |
|---|---|
| `mobile.ini` | The phone's own settings (theme, lock), needed before the unlock. |
| `known_hosts` | libssh reads it at every connection. Host names are hashed, so it does not tell which hosts these are; the keys stay readable. |
| Names of key files, and `.pub` files | Only private keys are encrypted. |
| Thumbnails, Open and Share copies | Until the session ends or the next start. |
| Copies of files handed to another app's editor | Until the session ends. |

The built-in editor keeps a file in memory once it is open. The app's screens are blocked from screen capture and from
the recent apps list. Android backup of the app is off.

The files cannot be reached on a phone, so when no password can open the configuration (it was replaced, it is damaged,
or NixSCP 1.2.3 or older encrypted it), the lock page says so and offers **Start over...**. This deletes the
configuration on the phone, with its sites and passwords, and signs it out of NixSCP Pro; what was synced stays in the
cloud.
