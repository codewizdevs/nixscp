// Creates the protocol implementation for a session (TTerminal::DoOpen's
// FFileSystem selection in WinSCP source/core/Terminal.cpp)
#pragma once

#include "core/FileSystem.h"
#include "core/SessionData.h"

#include <memory>

// Returns nullptr when the protocol is not available in this build.
// Every protocol class takes (const SessionData &, UserInterface *).
std::unique_ptr<FileSystem> CreateFileSystem(const SessionData & Data, UserInterface * UI);
