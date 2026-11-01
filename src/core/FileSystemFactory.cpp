// Port of protocol selection in WinSCP source/core/Terminal.cpp (TTerminal::DoOpen)
#include "core/FileSystemFactory.h"
#include "core/SftpFileSystem.h"

// Protocols are added as their implementations land
#if __has_include("core/ScpFileSystem.h")
#include "core/ScpFileSystem.h"
#define NIXSCP_HAS_SCP
#endif
#if __has_include("core/FtpFileSystem.h")
#include "core/FtpFileSystem.h"
#define NIXSCP_HAS_FTP
#endif
#if __has_include("core/S3FileSystem.h")
#include "core/S3FileSystem.h"
#define NIXSCP_HAS_S3
#endif
#if __has_include("core/WebDavFileSystem.h")
#include "core/WebDavFileSystem.h"
#define NIXSCP_HAS_WEBDAV
#endif

std::unique_ptr<FileSystem> CreateFileSystem(const SessionData & Data, UserInterface * UI)
{
  switch (Data.FSProtocol)
  {
    case fsSFTP:
    case fsSFTPonly:
      return std::make_unique<SftpFileSystem>(Data, UI);
#ifdef NIXSCP_HAS_SCP
    case fsSCPonly:
      return std::make_unique<ScpFileSystem>(Data, UI);
#endif
#ifdef NIXSCP_HAS_FTP
    case fsFTP:
      return std::make_unique<FtpFileSystem>(Data, UI);
#endif
#ifdef NIXSCP_HAS_S3
    case fsS3:
      return std::make_unique<S3FileSystem>(Data, UI);
#endif
#ifdef NIXSCP_HAS_WEBDAV
    case fsWebDAV:
      return std::make_unique<WebDavFileSystem>(Data, UI);
#endif
    default:
      return nullptr;
  }
}
