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
- The key is kept in memory only, never written to disk.

If you changed the password on another computer, this computer still uses the previous password until it syncs
again; the recovery key always works.

## What is encrypted, and what is not

Encrypted (the format is the same NXS1 format as cloud snapshots, AES-256-GCM with the sync data key, unlocked by
the password through Argon2id or by the recovery key):

| File (next to the configuration) | Contents |
|---|---|
| `nixscp.ini` | Sites, stored passwords, preferences, workspaces, bookmarks, custom commands, host key fingerprints configured in sites |
| `nixscp-cloud-session.ini` | The cached open tabs of cloud sync |
| `nixscp-teams.ini` | Team names and team sync state |
| `backups/nixscp-*.ini` | Copies made before cloud settings were applied |
| `agent-sessions/*.log` | [Agent Access](agent-access.md) session recordings |

Not encrypted:

| What | Why / protection |
|---|---|
| `nixscp-cloud.ini` | Cloud account e-mail, sign-in (refresh) token, sync state and the data key **wrapped** with your password and recovery key. Readable by your user only. The wrapped key is useless without the password or recovery key. |
| `nixscp-agent.log` | Agent Access audit log: times, site names, commands and results, never credentials or output. Readable by your user only. |
| `known_hosts` | SSH host keys you accepted (OpenSSH format). |
| Private key files | Your `.ppk` or OpenSSH key files are not touched; protect them with a passphrase. |
| Session logs, XML logs, temporary and edited files | Written where you configured them, as in WinSCP. |
| Exports you choose to make as plain files | See below. |

## The master password is retired

When encryption is on, the NixSCP password replaces WinSCP's master password: a master password that is set and
known is removed (site passwords are then protected by the encryption of the whole file), and
**Preferences > Security** says that the configuration is encrypted with your NixSCP password. Change the password
in **Pro > Cloud Sync Settings... > Change NixSCP password...**.

Configurations imported or synced from a computer that still uses a master password are converted the same way.

## Exporting and importing

**Login > Tools > Export/Backup Configuration...** asks how to export while encryption is on:

- **Encrypted backup** (default): opens in NixSCP on any computer with your NixSCP password or recovery key. Safe to
  keep anywhere, but WinSCP cannot read it. Written readable by your user only.
- **Plain file for WinSCP**: a normal WinSCP-compatible configuration that WinSCP and NixSCP can import. Site
  passwords in it are only obfuscated (no master password), so anyone who gets the file can read them. You are then
  also asked whether to include the cloud sync key ([cloud-sync.md](cloud-sync.md#configuration-backups-with-the-sync-key)).

**Import/Restore Configuration...** recognizes both. An encrypted backup made with the current key opens without a
question; one made with another password or another account asks for the NixSCP password it was made with, or its
recovery key. Importing it does not change your current password or key.

## Recovery key

The recovery key is created when you set up cloud sync on your first computer (**Create a recovery key
(recommended)**) and shown once: 13 groups of 4 characters. Typing mistakes between `O`/`0` and `I`/`L`/`1`, and
upper or lower case, are tolerated. It unlocks:

- NixSCP at startup (**Use my recovery key instead**),
- `nixscp-cli` (see below),
- encrypted backups,
- cloud sync on a new computer, after which you choose a new password.

Without the password and the recovery key, the encrypted files cannot be opened by anyone, including NixSCP
support. Keep it somewhere safe, away from the computer.

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

After **Sign Out**, the Cloud Sync window offers **Remove encryption from this computer...**. The configuration is
then stored unencrypted again, NixSCP no longer asks for the password at startup, and saved site passwords are only
obfuscated, as in WinSCP (set a master password in Preferences > Security if you want them encrypted). Signing in to
NixSCP Pro encrypts the configuration again.

[Agent Access](agent-access.md) needs either this encryption or a master password.
