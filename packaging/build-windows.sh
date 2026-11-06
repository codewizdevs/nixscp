#!/usr/bin/env bash
# Cross-compiles NixSCP for Windows (x86_64) on Linux and packages it:
#   bin/windows/NixSCP/                         portable folder
#   bin/windows/NixSCP-<version>-windows-x64.zip
#   bin/windows/NixSCP-<version>-setup.exe      installer (when makensis is installed)
#
# Needs the Windows SDK folder win-sdk/ (see cmake/windows-llvm-mingw.cmake):
# llvm-mingw, Qt for Windows (llvm-mingw build) and deps/ with OpenSSL and libssh.
# Build that OpenSSL with no-autoload-config: otherwise it reads an openssl.cnf
# from its built-in path, which on Windows can be a folder other users can write to.
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${NIXSCP_WIN_SDK:-$SOURCE_DIR/win-sdk}"
BUILD_DIR="${BUILD_DIR:-$SOURCE_DIR/builds/windows}"
OUT_DIR="${OUT_DIR:-$SOURCE_DIR/bin/windows}"
JOBS="${JOBS:-$(nproc)}"

QT="$(ls -d "$SDK"/qt/*/llvm-mingw_64 | head -1)"
LLVM="$SDK/llvm-mingw"
[ -d "$QT" ] && [ -d "$LLVM" ] && [ -f "$SDK/deps/bin/libssh.dll" ] || { echo "build-windows: incomplete Windows SDK in $SDK" >&2; exit 1; }

# What the release is built from, as in .github/workflows/macos.yml: the sources
# in the SDK match their published hashes, and the libraries report these versions
QT_VERSION=6.10.2
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
LIBSSH_VERSION=0.11.5
LIBSSH_SHA256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c
[ "$(basename "$(dirname "$QT")")" = "$QT_VERSION" ] || { echo "build-windows: expected Qt $QT_VERSION in $SDK/qt" >&2; exit 1; }
echo "$OPENSSL_SHA256  $SDK/src/openssl-$OPENSSL_VERSION.tar.gz" | sha256sum --quiet -c -
echo "$LIBSSH_SHA256  $SDK/src/libssh-$LIBSSH_VERSION.tar.xz" | sha256sum --quiet -c -
grep -aq "OpenSSL $OPENSSL_VERSION " "$SDK/deps/bin/libcrypto-3-x64.dll" || { echo "build-windows: libcrypto is not OpenSSL $OPENSSL_VERSION" >&2; exit 1; }
grep -aq "libssh_$LIBSSH_VERSION" "$SDK/deps/bin/libssh.dll" || { echo "build-windows: libssh.dll is not libssh $LIBSSH_VERSION" >&2; exit 1; }

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$SOURCE_DIR/cmake/windows-llvm-mingw.cmake" -DNIXSCP_WIN_SDK="$SDK" \
  -DCMAKE_BUILD_TYPE=Release -DNIXSCP_BUILD_TESTS=OFF > /dev/null
cmake --build "$BUILD_DIR" -j"$JOBS"

VERSION="$(sed -n 's/^project(nixscp VERSION \([0-9.]*\).*/\1/p' "$SOURCE_DIR/CMakeLists.txt")"
STAGE="$OUT_DIR/NixSCP"
rm -rf "$STAGE"
mkdir -p "$STAGE/licenses"

# Programs (symbols stripped) and the libraries they need
for Exe in nixscp nixscp-cli; do
  "$LLVM/bin/llvm-strip" -o "$STAGE/$Exe.exe" "$BUILD_DIR/$Exe.exe"
done
cp "$QT"/bin/{Qt6Core,Qt6Gui,Qt6Widgets,Qt6Network,Qt6Svg}.dll "$STAGE/"
cp "$SDK"/deps/bin/{libssh,libcrypto-3-x64,libssl-3-x64}.dll "$STAGE/"
# The C++ runtime of the toolchain NixSCP is compiled with, not Qt's (built
# with an older llvm-mingw): newer libc++ keeps the older exports
cp "$LLVM"/x86_64-w64-mingw32/bin/{libc++,libunwind,libwinpthread-1}.dll "$STAGE/"
# Qt plugins: Windows platform and style, image formats (SVG icons), TLS backends
for Plugin in platforms/qwindows styles/qmodernwindowsstyle imageformats/qsvg imageformats/qico imageformats/qgif \
              imageformats/qjpeg iconengines/qsvgicon tls/qschannelbackend tls/qopensslbackend tls/qcertonlybackend; do
  mkdir -p "$STAGE/$(dirname "$Plugin")"
  cp "$QT/plugins/$Plugin.dll" "$STAGE/$Plugin.dll"
done

# Every function imported from a bundled DLL must be exported by it (Wine
# tolerates a missing one, Windows refuses to start the program)
python3 - "$LLVM/bin/llvm-readobj" "$STAGE" <<'PYEOF'
import os, subprocess, sys
Readobj, Stage = sys.argv[1], sys.argv[2]
def Read(Path, Option):
    return subprocess.run([Readobj, Option, Path], capture_output=True, text=True, check=True).stdout.splitlines()
Binaries = [os.path.join(Dir, Name) for Dir, _, Names in os.walk(Stage) for Name in Names if Name.lower().endswith(('.exe', '.dll'))]
Bundled = {os.path.basename(Path).lower(): Path for Path in Binaries if os.path.dirname(Path) == Stage}
Exports = {}
Missing = []
for Path in Binaries:
    Dll = None
    for Line in Read(Path, '--coff-imports'):
        Line = Line.strip()
        if Line.startswith('Name: '):
            Dll = Line[6:].lower()
        elif Line.startswith('Symbol: ') and Dll in Bundled:
            Symbol = Line[8:].split(' (')[0]
            if Dll not in Exports:
                Exports[Dll] = {L.strip()[6:] for L in Read(Bundled[Dll], '--coff-exports') if L.strip().startswith('Name: ')}
            if Symbol and Symbol not in Exports[Dll]:
                Missing.append(f'{os.path.relpath(Path, Stage)}: {Dll}!{Symbol}')
if Missing:
    print('build-windows: unresolved imports:\n  ' + '\n  '.join(Missing[:20]), file=sys.stderr)
    sys.exit(1)
print(f'build-windows: imports of {len(Binaries)} binaries resolved')
PYEOF

# Icon theme (Windows has none): Adwaita and the hicolor base theme it inherits
ICONS="${ICON_THEME_DIR:-/usr/share/icons}"
mkdir -p "$STAGE/icons/hicolor"
cp "$ICONS/hicolor/index.theme" "$STAGE/icons/hicolor/"
rsync -a --exclude cursors --exclude cursor.theme --exclude icon-theme.cache "$ICONS/Adwaita/" "$STAGE/icons/Adwaita/"

# Licenses
cp "$SOURCE_DIR/LICENSE" "$STAGE/LICENSE.txt"
cp "$SOURCE_DIR"/packaging/windows/licenses/*.txt "$STAGE/licenses/"
cp "$SDK/src/openssl-"*/LICENSE.txt "$STAGE/licenses/OpenSSL-LICENSE.txt"
cp "$SDK/src/libssh-"*/COPYING "$STAGE/licenses/libssh-COPYING.txt"
cp "$LLVM/LICENSE.TXT" "$STAGE/licenses/llvm-mingw-LICENSE.txt"
cp /usr/share/doc/adwaita-icon-theme/copyright "$STAGE/licenses/Adwaita-copyright.txt"
cp "$SOURCE_DIR/packaging/windows/nixscp.ico" "$STAGE/"

# Portable zip
ZIP="$OUT_DIR/NixSCP-$VERSION-windows-x64.zip"
rm -f "$ZIP"
(cd "$OUT_DIR" && python3 -I -c 'import shutil, sys; shutil.make_archive(sys.argv[1], "zip", ".", "NixSCP")' "${ZIP%.zip}")

# Installer
if command -v makensis > /dev/null; then
  SETUP="$OUT_DIR/NixSCP-$VERSION-setup.exe"
  # The uninstaller removes what was installed, by name, and no more
  UNINSTALL_LIST="$BUILD_DIR/uninstall-list.nsh"
  for Entry in "$STAGE"/*; do
    if [ -d "$Entry" ]; then
      echo "RMDir /r \"\$INSTDIR\\$(basename "$Entry")\""
    else
      echo "Delete \"\$INSTDIR\\$(basename "$Entry")\""
    fi
  done > "$UNINSTALL_LIST"
  makensis -V2 -DVERSION="$VERSION" -DSTAGE="$STAGE" -DOUTFILE="$SETUP" -DUNINSTALL_LIST="$UNINSTALL_LIST" "$SOURCE_DIR/packaging/windows/installer.nsi"
fi

ls -la "$OUT_DIR"
