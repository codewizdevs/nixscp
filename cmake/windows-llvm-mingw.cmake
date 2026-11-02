# Cross-compiling NixSCP for Windows (x86_64) on Linux with llvm-mingw.
#
#   cmake -S . -B builds/windows -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake -DCMAKE_BUILD_TYPE=Release
#
# Expects the Windows SDK folder (default: <source>/win-sdk, or -DNIXSCP_WIN_SDK=...):
#   llvm-mingw/            llvm-mingw toolchain (the LLVM version Qt was built with)
#   qt/<ver>/llvm-mingw_64 Qt for Windows, llvm-mingw build (aqtinstall)
#   deps/                  OpenSSL and libssh built with this toolchain
# Qt's host tools (moc, rcc, uic) come from the Linux Qt of the same version.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT NIXSCP_WIN_SDK)
  get_filename_component(NIXSCP_WIN_SDK "${CMAKE_CURRENT_LIST_DIR}/../win-sdk" ABSOLUTE)
endif()
set(NIXSCP_WIN_SDK "${NIXSCP_WIN_SDK}" CACHE PATH "Windows SDK folder (llvm-mingw, Qt, deps)")
# Passed on to try_compile projects
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES NIXSCP_WIN_SDK)

set(LLVM_MINGW "${NIXSCP_WIN_SDK}/llvm-mingw")
set(CMAKE_C_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${LLVM_MINGW}/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${LLVM_MINGW}/bin/llvm-ranlib" CACHE FILEPATH "")

file(GLOB NIXSCP_WIN_QT LIST_DIRECTORIES true "${NIXSCP_WIN_SDK}/qt/*/llvm-mingw_64")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/x86_64-w64-mingw32" "${NIXSCP_WIN_SDK}/deps" ${NIXSCP_WIN_QT})
set(CMAKE_PREFIX_PATH "${NIXSCP_WIN_SDK}/deps" ${NIXSCP_WIN_QT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config: only the Windows libraries
set(ENV{PKG_CONFIG_LIBDIR} "${NIXSCP_WIN_SDK}/deps/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# Qt host tools from the Linux Qt
if(NOT QT_HOST_PATH)
  set(QT_HOST_PATH "/usr" CACHE PATH "Qt for the build machine (moc, rcc, uic)")
endif()
if(NOT QT_HOST_PATH_CMAKE_DIR AND EXISTS "/usr/lib/x86_64-linux-gnu/cmake/Qt6HostInfo")
  # Debian/Ubuntu keep the CMake files of the host Qt under the multiarch dir
  set(QT_HOST_PATH_CMAKE_DIR "/usr/lib/x86_64-linux-gnu/cmake" CACHE PATH "")
endif()
