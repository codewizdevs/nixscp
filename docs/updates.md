# New-release check

NixSCP can tell you when a new version is released. It never downloads or installs anything by itself.

## How it works

- About 15 seconds after NixSCP starts, it asks GitHub for the latest release of
  [codewizdevs/nixscp](https://github.com/codewizdevs/nixscp/releases) (`api.github.com`). Only the usual HTTPS
  request is sent (with a `NixSCP/<version>` user agent); no data about you, your sites or your settings.
- It checks **at most once a day**. If GitHub cannot be reached, it tries again at the next start.
- When the latest release is newer than your version, a notice shows the new version number, with:
  - **Open download page**: opens the release page in your browser.
  - **Skip this version**: no more notices for this version (a later one is announced again).
  - **Remind me later**: closes the notice; you are reminded at a later check (the next day at the earliest).

The notice does not block NixSCP; you can keep working while it is open.

## Settings

**Preferences > Updates**:

- **Check for a new version when NixSCP starts (at most once a day)**: on by default. Turn it off to never contact
  GitHub.
- The time of the last check.
- **Check now**: checks immediately and tells you whether you have the latest version (or why the check failed).

Setting the environment variable `NIXSCP_NO_UPDATE_CHECK` (to any value) also disables the check at startup, for
example for managed installations.

## Updating

Download the new package from the release page (or [nixscp.com/download](https://nixscp.com/download)) and install
it over the old one: `.deb`/`.rpm` with your package manager, replace the AppImage file, run the new Windows installer,
or unpack the new portable zip. Your settings are kept, with one exception for NixSCP Pro: this version cannot read
what NixSCP 1.2.3 or older encrypted, and it cannot be converted. If you use encryption, make a plain export with 1.2.3
first ([encryption.md](encryption.md#exporting-and-importing)), and keep that file private.

If you updated without one:

- **The encrypted configuration** stays intact. NixSCP says that it was encrypted by NixSCP 1.2.3 or older, that 1.2.3
  still opens it, and which files to delete to start over. On Android, the lock page offers **Start over...**, which
  deletes those files with the sites and passwords on the phone; what was synced stays in the cloud.
- **Cloud data** from those versions can be replaced, not read:
  see [cloud-sync.md](cloud-sync.md#accounts-from-nixscp-123-or-older).
- **Encrypted backups** from those versions open only in 1.2.3.

The Help menu also links to the project: **Product Homepage** (nixscp.com), **Report a Bug / Request a Feature**
(GitHub issues) and **Version History** (GitHub releases).
