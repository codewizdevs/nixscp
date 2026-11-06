# Cloud sync (NixSCP Pro)

Cloud sync keeps your NixSCP settings the same on all your computers, Linux, macOS and Windows: sites and site folders,
stored passwords, preferences, workspaces, bookmarks, custom commands, transfer presets, and the session tabs you
had open. Everything is **end-to-end encrypted** on your computer before it is uploaded; the NixSCP cloud stores it
only as ciphertext, which it cannot read without your sync password.

Cloud sync is part of **NixSCP Pro**, which is **free during the public beta**. The NixSCP application itself stays
free and open source. Signing in also turns on [local encryption](encryption.md) of the configuration.

## Setting up

1. Create an account at [nixscp.com](https://nixscp.com/login?register&next=/account), or from NixSCP:
   **Cloud Sync > Create Account...** (the menu is in the main window's menu bar). The website e-mails you a link to
   confirm your address; [Teams](teams.md) need that.
2. In NixSCP, choose **Cloud Sync > Sign In...** and click **Sign in with browser...**. Your browser opens the
   NixSCP website; confirm there, and NixSCP is signed in. (The browser hands a one-time code back to NixSCP on
   `127.0.0.1`; your account password is never entered in NixSCP.)
3. **First computer:** choose your **sync password**: at least 12 characters. NixSCP shows a strength estimate,
   refuses passwords that are too easy to guess and offers **Generate a passphrase**. Keep **Create a recovery key
   (recommended)** ticked, and store the recovery key that NixSCP shows (13 groups of 4 characters) somewhere safe.
   **Other computers:** sign in with the same account and enter the same sync password, or the recovery key
   (**I forgot it: use the recovery key**).

The sync password is also your **NixSCP password**: it encrypts the configuration on each computer and NixSCP asks
for it at startup. It is separate from your account password; do not reuse that one. See [encryption.md](encryption.md).

Once signed in, the menu is called **Pro** and offers **Sync Now**, **Cloud Sync Settings...**,
**Manage Account...** and **Sign Out**. The status bar shows the sync state (`Cloud: synced`, `Cloud: syncing...`,
`Cloud: conflict`, `Cloud: offline`, `Cloud: enter sync password`, `Cloud: no license`); click it to open the
settings.

## What is synced

| Synced | Stays on each computer |
|---|---|
| Sites and site folders, with stored passwords, private key paths and all site settings (including [tunnels](tunnels.md) and personal [Agent Access](agent-access.md) settings) | Window sizes and positions, panel splitter and column layouts |
| Preferences | Histories (`Configuration\History`), the remote directory cache |
| Workspaces, bookmarks, custom commands, transfer settings presets | Last used local folder, last selected site, expanded site folders |
| The open session tabs (as a separate stream) | The auto-saved workspace (open tabs travel in the session stream instead) |

[Team](teams.md) sites (`Team: <name>/...`) are synced with their team, not with your personal settings. Your files
are never uploaded: transfers always go directly between your computer and your servers.

Private key **files** are not synced, only their paths. Copy key files to the same location on each computer, or
use paths that exist on every computer.

## When it syncs

- At startup, every 5 minutes, when the NixSCP window gets focus (if the last check is older than a minute),
  10 seconds after you change the settings (adding or editing a site, changing preferences), and when NixSCP exits
  (waiting at most 4 seconds).
- Open tabs: at most once a minute while they change, and on exit. At startup, NixSCP reopens the tabs saved last by
  any of your computers instead of showing the Login dialog.
- **Pro > Sync Now** syncs immediately.

Options under **Cloud Sync Settings... > On this computer**:

- **Sync open sessions (tabs)**: upload this computer's open tabs.
- **Reopen the last open sessions at startup**.

Tabs of saved sites reopen as links to the site. Tabs of unsaved sessions carry their password only if
**Save passwords** of the auto-saved workspace is on. A local folder that does not exist on the other computer is
skipped.

## Conflicts and history

Every upload carries the version it is based on, so a computer that was offline can never overwrite newer cloud
settings by accident:

- No local changes and newer settings in the cloud: they are applied.
- Local changes and no newer settings in the cloud: they are uploaded.
- Changes on both sides: NixSCP asks you. **Use cloud settings** (default), **Keep this computer's settings**
  (uploads this computer's settings over the cloud's), or **Decide later** (syncing pauses; the status bar shows
  `Cloud: conflict`).

Before settings from the cloud replace the local ones, NixSCP saves a copy in the `backups` folder next to the
configuration (`nixscp-<time>.ini`, the last 10). The cloud keeps the last 30 versions of your settings.

## Encryption

- Your sync password is turned into a key with **Argon2id** (64 MiB of memory, 3 passes) on your computer.
- Settings and tabs are encrypted with **AES-256-GCM** under a random 256-bit data key. The data key is stored only
  wrapped with the password-derived key and, optionally, with the recovery key.
- Each snapshot's account, type and version are authenticated, so the server cannot swap or replay snapshots without
  NixSCP noticing.
- Each snapshot also names the one before it. A computer that has already synced remembers the newest version it saw,
  for the settings, the open tabs and each [team](teams.md), and stops syncing with a message when the cloud shows
  something older or a history that does not continue. This detects a rollback only on such computers: a new computer
  has nothing to compare with, a complete freeze of updates by the server cannot be detected, and a version the server
  no longer keeps ends the check.
- The server never receives the sync password, the data key or anything in plaintext. A wrong password simply
  fails to decrypt, and nobody can reset it on the server.
- Anyone who holds a snapshot, the server included, can test password guesses offline, at Argon2id's cost: every
  snapshot carries the wrapped data key. A strong password is what stands against that, which is why NixSCP asks for
  12 characters, shows a strength estimate and can generate a passphrase.

The protocol is described in detail at [nixscp.com/security](https://nixscp.com/security).

## Changing the password, recovery, signing out

- **Change NixSCP password...** (in Cloud Sync Settings) sets a new sync password. The data key stays the same.
  Other computers keep using the previous password until they sync, and the recovery key stays valid.
- **A password change alone does not revoke access.** Whoever learned the old password can still open the data key
  from any old copy of your encrypted data. If a password or recovery key may have leaked, tick **Also replace the data
  key and the recovery key** in the same window. Everything is encrypted again under a new data key, you get a new
  recovery key, and your other computers ask for the new password once. Older versions kept in the cloud stay readable
  with the old key.
- **New recovery key...** creates a new recovery key, shown once, with **Also replace the data key** ticked. Without
  that, the old recovery key can still open the data key from any old copy of your encrypted data.
- **A replacement that is interrupted** (a crash, a lost connection) finishes or is called off at the next sync, and a
  new recovery key is shown then. Until that, what the change must not lose waits in `nixscp-cloud-keys.ini`, sealed
  like your other files, and `nixscp-cloud-bridge.ini` holds the old key, sealed, while the new one reaches the local
  files.
- **A computer that reconnects after the data key was replaced elsewhere** stops syncing and asks for the current
  password once. It keeps what it has until then, and goes on from where it was.
- **Forgot the sync password:** on a new computer, choose **I forgot it: use the recovery key**, then set a new
  password. Without the password or the recovery key, synced data cannot be decrypted by anyone, by design. The
  settings on your computers are not affected.
- **Sign Out** keeps your settings on this computer and in the cloud. The configuration stays encrypted with your
  NixSCP password until you sign in again or choose **Remove encryption from this computer...** in the Cloud Sync
  window ([encryption.md](encryption.md)).
- A computer removed from your account on the website, or signed out by a password reset, stops syncing and NixSCP
  tells you why. Changing your account password on the website signs out your computers too, unless you untick that.
  A signed-out computer keeps what it already has.

## Accounts from NixSCP 1.2.3 or older

This version cannot read cloud data written by NixSCP 1.2.3 or older, so no password opens it. When you sign in, NixSCP
says so and offers **Replace the cloud data...**: you choose a new sync password, and after you confirm, this computer's
settings replace the data in the cloud. Nothing is replaced until you confirm. The old data stays in the cloud history,
unread, and your other computers then need the new password. See also [updates.md](updates.md#updating).

## License states

- **Beta:** sync is free for every account.
- **No active license** (after the beta, or when a subscription ends): settings from your other computers still
  download, and nothing is lost, but changes on this computer are not uploaded. NixSCP tells you once, with a link
  to renew. Changes waiting on this computer are uploaded once the license is active again.

## Configuration backups with the sync key

When you export a plain configuration file while signed in (Login > Tools > **Export/Backup Configuration...**),
NixSCP asks whether to **include your cloud sync key**. With the key, importing the backup on another computer starts
syncing without asking for the sync password, but anyone who gets the file can decrypt your synced data. The default
is **Export without key**. See [encryption.md](encryption.md#exporting-and-importing) for the encrypted backup format.

See also: [Local encryption](encryption.md), [Teams](teams.md).
