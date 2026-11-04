# NixSCP for Android

A phone and tablet companion built on the same core as the desktop app
(`src/core`), with a touch interface in Qt Quick (`src/mobile`). Released as
an APK on GitHub (`NixSCP-<version>-android-arm64.apk`, Android 9+, 64-bit
ARM); not on Google Play yet.

## What works

- Saved sites: search as you type (name, host, user; Enter connects to the
  only match), Favourites (star) and Recent on top, folders that fold.
  Long press: Connect, Edit, Duplicate, favourite, Delete. Add (SFTP, SCP,
  FTP, FTPS, WebDAV, WebDAVS, S3), import a `WinSCP.ini` (menu). Passwords
  are stored as on desktop (WinSCP format); an empty password is asked for
  when connecting. Favourites, recent sites and folded folders are the
  phone's own (`mobile.ini`).
- Connecting with host key verification (the app's own `known_hosts`),
  password / passphrase / keyboard-interactive prompts, and Cancel.
- Two panes, the phone and the server: side by side from 600 dp wide
  (unfolded Fold, tablets), as Phone / Server tabs on phones. The phone pane
  needs "All files access" (Android 11+; the storage permission on 9/10) and
  asks for it.
- Each pane: path bar (tap: parent folders, Go to folder), filter as you
  type, sort by name / date / size (reversible), hidden files, pull down to
  refresh; these view settings are remembered per side.
- Long press (or tap the icon) selects; several files at once: Download (to
  the phone's Download folder; "Copy to" the phone pane's folder when it
  shows another), Upload to the server's folder, Share, Delete, Rename, Open.
- Transfers show progress, speed and the file count, with Cancel (a
  cancelled download leaves a `.filepart` that a new download resumes). They
  go on in the background (foreground service with a notification); the
  first "connection lost" question after the app was away is answered with
  Retry.
- Server files: Edit (the built-in editor, or an editor app chosen in
  Settings; saved changes are uploaded back), Download as archive (the
  selection packed on the server with tar or zip and saved to Download;
  servers without commands or without zip: packed on the phone as .zip),
  Extract here (.tar.*, .zip, with tar / unzip on the server), Properties
  (rights as checkboxes or octal, owner, group, recursively).
- Terminal (SFTP, SCP sites): an interactive shell on a pseudo-terminal
  (xterm-256color: top, htop, vi, less), on its own SSH connection, with
  libvterm (`third_party/libvterm`, MIT) for the screen; extra keys (Esc,
  Tab, Ctrl, Alt, arrows, Home/End, PgUp/PgDn, symbols), paste, text size,
  drag to scroll back. It stays open when leaving the page.
- Tabs: several sessions at once (+ opens another site); each keeps its
  folder, transfers and terminal, and their results are reported from any
  tab.
- New file (server: created and opened in the editor; phone: empty file).
- Transfer queue: downloads and uploads of every tab wait in one queue and
  run on a second connection per tab, so browsing goes on meanwhile; with
  the app in the background they go on too (the notification shows the
  queue and its progress). The Transfers page: cancel, retry, clear.
- Thumbnails (phone images; server images up to 3 MB, fetched on the
  second connection) and Preview (images with pinch zoom, text read only).
- Copy / Cut and Paste between folders of the server (done on the server).
- Find files below the current folder (WinSCP's Find files, every
  protocol); a result opens its folder.
- Recently visited folders per site in the path menu.
- Share -> NixSCP from any app: choose a site and folder, Upload here.
- Home-screen shortcut per site (long press -> Add to home screen).
- Check connection (long press): name, port, the server's greeting.
- Bookmarks of server folders per site (WinSCP's, in the configuration),
  in the path menu.
- SSH keys: generate (Ed25519, RSA 4096, optional passphrase) or pick a
  .ppk / OpenSSH key; copy, share or install the public key on the
  connected server; choose a key for a site.
- Settings: connection timeout and keepalive (for every site opened on the
  phone), the editor, sort and hidden files, archive format, delete and
  overwrite confirmations, theme (system / light / dark).
- Android 9+ (API 28), edge-to-edge on Android 15.

- NixSCP Pro: sign-in in the browser, back to the app through a loopback
  callback (as on desktop, no server change); the NixSCP password (first
  device: created, with a recovery key; others: unlocked); settings sync
  (core/CloudSync, end-to-end encrypted; the phone's open tabs are not
  synced), conflicts, license notices. The device is named by its model.
- The encrypted configuration: with Pro, nixscp.ini and the phone's SSH
  private keys are encrypted (core/ConfigVault). NixSCP asks for the NixSCP
  password (or the recovery key, or the fingerprint) at start and after 5
  minutes in the background (1 / 5 / 15 / 60, or only at start). A
  connection uses a decrypted copy of its key in a private temporary file,
  removed when it closes. Fingerprint unlock keeps the password encrypted
  with an Android Keystore key that only a fingerprint releases.
- Teams: create, rename, delete, leave, join with an invite code; members
  (roles, remove), invites (create, share the code, revoke); share a site
  folder.
- A site's SSH key on the phone is the phone's own choice (mobile.ini): the
  site's key file is synced and is another device's path; the phone always
  uses its own known_hosts.

OpenSSL is built as libcrypto_3.so / libssl_3.so (build-deps.sh): Qt for
Android loads it under those names for HTTPS (NixSCP Pro).

## Build

```sh
packaging/android/setup-sdk.sh       # JDK 17, Android SDK/NDK/emulator, Qt 6.8.3 for Android (android-sdk/, ~7 GB)
packaging/android/build-deps.sh      # OpenSSL 3.5 and libssh 0.11 for arm64-v8a and x86_64
source android-sdk/env.sh

# emulator (x86_64), debug
$QT_ANDROID_X86_64/bin/qt-cmake -S . -B builds/android-x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT -DANDROID_NDK_ROOT=$ANDROID_NDK_ROOT -DQT_HOST_PATH=$QT_HOST_PATH -DNIXSCP_BUILD_TESTS=OFF
cmake --build builds/android-x86_64 --target apk
# -> builds/android-x86_64/src/mobile/android-build/nixscp-mobile.apk

# phones (arm64), release, signed with the release key (kept outside the repository):
NIXSCP_ANDROID_KEYSTORE=/path/to/nixscp-release.jks NIXSCP_ANDROID_KEYSTORE_PASS=... packaging/build-android.sh
# -> bin/NixSCP-<version>-android-arm64.apk (the version code follows the version: 1.2.1 = 10201)
```

On Android the CMake project builds only the core and `src/mobile` (no desktop
windows, CLI or desktop tests). `packaging/android/package/` holds the
manifest (no Android backup of the configuration, keyboard handling, the
transfer service, FileProvider paths) and the launcher icon.

## Try it in the emulator

```sh
source android-sdk/env.sh
emulator -avd nixscp &                      # a Pixel 7 with Android 15, in a window
adb install -r bin/android/NixSCP-1.2.1-android-x86_64-emulator.apk
```

NixSCP then appears in the app drawer. From the emulator, `10.0.2.2` is this
computer (e.g. an SSH server listening on 127.0.0.1 here).

On a phone: download the release APK and open it, allowing installs from
that source when Android asks. A build signed with another key (a debug
build) cannot update it: uninstall first.
