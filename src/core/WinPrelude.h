// Included first in every source file of the Windows build (CMakeLists.txt).
// windows.h maps Win32 functions to macros (CreateDirectory -> CreateDirectoryW,
// DeleteFile, RemoveDirectory, SetCurrentDirectory...) that would rename
// NixSCP's own methods of the same names. It is included here once, before
// anything else, and those macros are removed; later includes are no-ops.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#undef CopyFile
#undef CreateDirectory
#undef DeleteFile
#undef GetCurrentDirectory
#undef GetMessage
#undef GetUserName
#undef MoveFile
#undef PostMessage
#undef RemoveDirectory
#undef SendMessage
#undef SetCurrentDirectory
#undef GetObject
#undef DrawText
#undef LoadIcon
#undef ERROR
#undef IGNORE
#undef DELETE
#undef interface
#endif
