# NixSCP for Android

A phone companion built on the same core as the desktop app (`src/core`), with
a touch interface in Qt Quick (`src/mobile`). Status: **prototype**, not
released.

## What works

- Saved sites: add (SFTP, SCP, FTP, FTPS, WebDAV, WebDAVS), delete (long
  press), import a `WinSCP.ini` (menu). Passwords are stored as on desktop
  (WinSCP format); an empty password is asked for when connecting.
- Connecting with host key verification (the app's own `known_hosts`) and
  password / passphrase / keyboard-interactive prompts.
- Remote files: browse, Open (with another app), Share, Download (to the
  public Downloads/NixSCP folder, shown by the Files app), Upload
  (Android's file picker), Rename, Delete, New folder; WinSCP's own
  questions (overwrite, reconnect...) as sheets.
- Transfers go on in the background (foreground service with a
  notification); a connection Android cut while the file picker was open is
  reconnected automatically.
- Light and dark mode, Android 9+ (API 28), edge-to-edge on Android 15.

Not yet: NixSCP Pro sign-in and sync (needs an app-link sign-in on the
server), the encrypted configuration, Teams, a built-in text editor,
tablets' two-panel layout.

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

# phones (arm64), release: use $QT_ANDROID_ARM64 and -DCMAKE_BUILD_TYPE=Release,
# then zipalign + apksigner (a debug key for testing; the release key later)
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

On a phone: allow installing from unknown sources and open
`bin/android/NixSCP-1.2.1-android-arm64-test.apk` (signed with the local debug
key, for testing only).
