// Tests of the FTP/FTPS completeness features of core/FtpFileSystem (WinSCP
// source/core/FtpFileSystem.cpp, filezilla/FtpControlSocket.cpp,
// AsyncProxySocketLayer.cpp, core/Terminal.cpp certificate cache):
// FtpsCertificates cache, post login commands, HOST/CLNT/CSID, raw command
// console, HASH/X<alg> checksums, SITE CPFR/CPTO, TYPE A, active mode
// external IP and port range, work from current directory, SIZE/MDTM file
// properties, IIS rename, SOCKS4A/SOCKS5/HTTP proxies, FTP proxy logon types,
// TLS client certificates, TLS versions, keepalives, SFTP server detection.
//
// The certificate cache part always runs. The rest is skipped unless
// NIXSCP_TEST_FTPX_PORT is set:
//   NIXSCP_TEST_FTPX_PORT    base port of the WP07 pyftpdlib servers (ftpx.py):
//                            +0 extras (XLOG, HASH, XSHA256, XMD5, XCRC, CSID,
//                            CLNT, SITE CPFR/CPTO), +1 the same without MLSD
//                            and HASH, +2 FTPS with client certificate,
//                            +3 FTPS TLS <= 1.2, +4 FTPS TLS 1.0, +5 IIS-like,
//                            +6 FTP proxy (fwuser/fwpass, ftp.target.test)
//   NIXSCP_TEST_FTPX_ROOT    local path of the servers' root (optional checks)
//   NIXSCP_TEST_FTPX_CERTS   directory with ca.crt, client-combined.pem
//                            (passphrase "certpass"), client.p12, client-sep.key
//                            + client-sep.crt, client-ed.key, client.csr
//   NIXSCP_TEST_PROXY_PORT   SOCKS4/4A + SOCKS5 (puser/ppass) + HTTP proxy that
//                            resolves ftp.nixscp.test to 127.0.0.1
//   NIXSCP_TEST_PROXY_LOG    its connection log
//   NIXSCP_TEST_SSH_PORT     an SSH server (SFTP server detection)
#include "core/Configuration.h"
#include "core/FtpFileSystem.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static void Log(const QString & Message)
{
  std::printf("  %s\n", qPrintable(Message));
  std::fflush(stdout);
}

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  QStringList PromptAnswers;
  QList<unsigned int> QueryAnswers;
  QList<PromptKind> PromptKinds;
  QStringList PromptInstructions;
  QStringList PromptTexts;
  QStringList Queries;
  QStringList Banners;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    PromptAnswers.clear();
    QueryAnswers.clear();
    PromptKinds.clear();
    PromptInstructions.clear();
    PromptTexts.clear();
    Queries.clear();
    Banners.clear();
  }

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyCancel;
  }

  bool PromptUser(PromptKind Kind, const QString &, const QString & Instructions, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    PromptKinds.append(Kind);
    PromptInstructions.append(Instructions);
    for (Prompt & P : Prompts)
    {
      PromptTexts.append(P.Text);
      if (PromptAnswers.isEmpty())
      {
        return false;
      }
      P.Result = PromptAnswers.takeFirst();
    }
    return true;
  }

  void DisplayBanner(const QString &, const QString & Banner) override
  {
    QMutexLocker Lock(&Mutex);
    Banners.append(Banner);
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    unsigned int Answer = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaCancel) : QueryAnswers.takeFirst();
    Log(QStringLiteral("QueryUser \"%1...\" -> %2").arg(Query.left(50).replace(QLatin1Char('\n'), QLatin1Char(' '))).arg(Answer));
    return Answer;
  }
};

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"")
    .arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static OperationResult WaitFor(const std::function<void(Terminal::Callback)> & Start, int TimeoutMs = 120000)
{
  QEventLoop Loop;
  OperationResult Result;
  bool Called = false;
  Start([&](const OperationResult & R)
  {
    Result = R;
    Called = true;
    Loop.quit();
  });
  if (!Called)
  {
    QTimer::singleShot(TimeoutMs, &Loop, [&]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
    Loop.exec();
  }
  return Result;
}

static OperationResult DoOpen(Terminal & T)
{
  return WaitFor([&](Terminal::Callback Cb) { T.Open(Cb); });
}

static OperationResult DoJob(Terminal & T, Terminal::WorkerJob Job)
{
  return WaitFor([&](Terminal::Callback Cb) { T.RunOnWorker(std::move(Job), Cb); });
}

static OperationResult DoList(Terminal & T, const QString & Path, RemoteFileList * Files = nullptr)
{
  QEventLoop Loop;
  OperationResult Result;
  T.ReadDirectory(Path, [&](const OperationResult & R, const QString &, const RemoteFileList & List)
  {
    Result = R;
    if (Files != nullptr)
    {
      *Files = List;
    }
    Loop.quit();
  });
  Loop.exec();
  return Result;
}

struct ConsoleResult
{
  OperationResult Result;
  QStringList Lines;
  QString Directory;
};

static ConsoleResult DoConsole(Terminal & T, const QString & Command)
{
  ConsoleResult C;
  QEventLoop Loop;
  T.AnyCommand(Command,
    [&](const QString & Line, bool) { C.Lines << Line; },
    [&](const OperationResult & R, int, const QString & Directory)
    {
      C.Result = R;
      C.Directory = Directory;
      Loop.quit();
    });
  Loop.exec();
  return C;
}

// Commands received by the server in this session (XLOG of ftpx.py)
static QStringList XLog(Terminal & T)
{
  ConsoleResult C = DoConsole(T, QStringLiteral("XLOG"));
  CHECK_MSG(C.Result.Ok, Describe(C.Result));
  QStringList Result;
  for (const QString & Line : std::as_const(C.Lines))
  {
    Result << Line.mid(4);
  }
  return Result;
}

static qsizetype IndexOfPrefix(const QStringList & Lines, const QString & Prefix, qsizetype From = 0)
{
  for (qsizetype Index = From; Index < Lines.size(); Index++)
  {
    if (Lines[Index].startsWith(Prefix))
    {
      return Index;
    }
  }
  return -1;
}

static void WriteLocal(const QString & FileName, const QByteArray & Data)
{
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
}

static QByteArray ReadLocal(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QByteArray("<missing>");
  }
  return F.readAll();
}

static OperationResult Upload(Terminal & T, const QString & LocalFile, const QString & RemoteDir)
{
  CopyParam Param;
  Param.PreserveTime = true;
  return WaitFor([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{LocalFile}, RemoteDir, Param, false, Cb); });
}

static OperationResult MakeDir(Terminal & T, const QString & Path)
{
  return WaitFor([&](Terminal::Callback Cb) { T.CreateDirectory(Path, 0755, Cb); });
}

static QString IniValue(const QString & Section, const QString & Key)
{
  IniFile Ini;
  Ini.Load(Configuration::Instance().IniFileName());
  return Ini.ReadRaw(Section, Key);
}

//---------------------------------------------------------------------------
static void TestCertificateCache()
{
  std::printf("[certificate cache: FtpsCertificates storage]\n");
  FtpFileSystem::CacheCertificate(QStringLiteral("example.com:21"), QStringLiteral("aa:bb:cc"), 18);
  // WinSCP format: "<sha256>;<failures as %2.2X>", in [FtpsCertificates]
  CHECK_MSG(IniValue(QStringLiteral("FtpsCertificates"), QStringLiteral("example.com:21")) == QStringLiteral("aa:bb:cc;12"),
    IniValue(QStringLiteral("FtpsCertificates"), QStringLiteral("example.com:21")));
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    CHECK(Storage.OpenSubKey(QStringLiteral("FtpsCertificates"), false));
    CHECK(Storage.ReadString(QStringLiteral("example.com:21"), QString()) == QStringLiteral("aa:bb:cc;12"));
  }
  CHECK(FtpFileSystem::IsCertificateCached(QStringLiteral("example.com:21"), QStringLiteral("11:22"), QStringLiteral("AA:BB:CC"), 18));
  CHECK(FtpFileSystem::IsCertificateCached(QStringLiteral("example.com:21"), QStringLiteral("aa:bb:cc"), QStringLiteral("dd"), 0x12));
  CHECK(!FtpFileSystem::IsCertificateCached(QStringLiteral("example.com:21"), QStringLiteral("11:22"), QStringLiteral("aa:bb:cc"), 19));
  CHECK(!FtpFileSystem::IsCertificateCached(QStringLiteral("example.com:21"), QStringLiteral("11:22"), QStringLiteral("aa:bb:dd"), 18));
  CHECK(!FtpFileSystem::IsCertificateCached(QStringLiteral("example.com:990"), QStringLiteral("11:22"), QStringLiteral("aa:bb:cc"), 18));
  // legacy format: the fingerprint is the value name
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    CHECK(Storage.OpenSubKey(QStringLiteral("FtpsCertificates"), true));
    Storage.WriteString(QStringLiteral("ee:ff"), QStringLiteral("1"));
  }
  CHECK(FtpFileSystem::IsCertificateCached(QStringLiteral("legacy.example.com:21"), QStringLiteral("ee:ff"), QStringLiteral("00"), 5));
  // overwriting keeps one value
  FtpFileSystem::CacheCertificate(QStringLiteral("example.com:21"), QStringLiteral("aa:bb:cc"), 20);
  CHECK(IniValue(QStringLiteral("FtpsCertificates"), QStringLiteral("example.com:21")) == QStringLiteral("aa:bb:cc;14"));
}

static void TestHelpers()
{
  std::printf("[legacy charset, VMS version trimming]\n");
  // Windows-1252 for non UTF-8 names (UTF-8 locale)
  QByteArray Cp1252("\x80 caf\xE9 \x9A\x81", 9);
  QString Decoded = FtpDecodeLegacy(Cp1252);
  CHECK_MSG(Decoded == QString::fromUtf8("\xE2\x82\xAC caf\xC3\xA9 \xC5\xA1\xC2\x81"), Decoded);
  CHECK(FtpEncodeLegacy(Decoded) == Cp1252);
  QByteArray All;
  for (int Byte = 0; Byte < 256; Byte++)
  {
    All.append(static_cast<char>(Byte));
  }
  CHECK(FtpEncodeLegacy(FtpDecodeLegacy(All)) == All);
  CHECK(FtpTrimVmsVersion(QStringLiteral("FILE.TXT;3")) == QStringLiteral("FILE.TXT"));
  CHECK(FtpTrimVmsVersion(QStringLiteral("A;B.TXT;12")) == QStringLiteral("A;B.TXT"));
  CHECK(FtpTrimVmsVersion(QStringLiteral("plain.txt")) == QStringLiteral("plain.txt"));
}

//---------------------------------------------------------------------------
struct Env
{
  TestUserInterface * UI;
  SessionData Base;
  int Port;
  QString Root;
  QString Certs;
  QString Local;
};

static SessionData Site(const Env & E, int Offset)
{
  SessionData Data = E.Base;
  Data.PortNumber = E.Port + Offset;
  return Data;
}

static void TestLoginSequence(Env & E)
{
  std::printf("[post login commands, HOST, CLNT, CSID, welcome banner]\n");
  {
    E.UI->Reset();
    SessionData Data = Site(E, 0);
    Data.PostLoginCommands = QStringLiteral("NOOP\r\nHELP NOOP\n\nSTAT");
    Data.FtpHost = asOn;
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(T.ServerInfo().contains(QStringLiteral("Server software: WP07 Test FTPd 1.2.3")), T.ServerInfo());
    CHECK(T.CanAnyCommand());
    QStringList Lines = XLog(T);
    Log(Lines.join(QStringLiteral(" | ")));
    qsizetype Host = IndexOfPrefix(Lines, QStringLiteral("HOST 127.0.0.1"));
    qsizetype User = IndexOfPrefix(Lines, QStringLiteral("USER test"));
    qsizetype Pass = IndexOfPrefix(Lines, QStringLiteral("PASS "));
    qsizetype Clnt = IndexOfPrefix(Lines, QStringLiteral("CLNT NixSCP-release-"));
    qsizetype Noop = IndexOfPrefix(Lines, QStringLiteral("NOOP"));
    qsizetype Help = IndexOfPrefix(Lines, QStringLiteral("HELP NOOP"));
    qsizetype Stat = IndexOfPrefix(Lines, QStringLiteral("STAT"));
    qsizetype Csid = IndexOfPrefix(Lines, QStringLiteral("CSID Name=NixSCP;Version="));
    qsizetype Pwd = IndexOfPrefix(Lines, QStringLiteral("PWD"), std::max<qsizetype>(Stat, 0));
    CHECK((Host >= 0) && (Host < User));
    CHECK((Pass > User) && (Clnt > Pass));
    CHECK((Noop > Clnt) && (Help > Noop) && (Stat > Help));
    CHECK((Csid > Stat) && (Pwd > Csid));
    // the welcome message is not shown by default (ShowFtpWelcomeMessage)
    CHECK(E.UI->Banners.isEmpty());
  }
  {
    // hidden option ShowFtpWelcomeMessage in Configuration\Interface
    E.UI->Reset();
    {
      IniStorage Storage(&Configuration::Instance().Ini());
      CHECK(Storage.OpenSubKey(QStringLiteral("Configuration"), true) && Storage.OpenSubKey(QStringLiteral("Interface"), true));
      Storage.WriteBool(QStringLiteral("ShowFtpWelcomeMessage"), true);
    }
    Terminal T(Site(E, 0), E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG((E.UI->Banners.size() == 1) && E.UI->Banners.value(0).contains(QStringLiteral("pyftpdlib")), E.UI->Banners.join(QLatin1Char('|')));
    IniStorage Storage(&Configuration::Instance().Ini());
    CHECK(Storage.OpenSubKey(QStringLiteral("Configuration"), false) && Storage.OpenSubKey(QStringLiteral("Interface"), false));
    Storage.DeleteValue(QStringLiteral("ShowFtpWelcomeMessage"));
  }
  std::printf("[post login command failing]\n");
  {
    E.UI->Reset();
    SessionData Data = Site(E, 0);
    Data.PostLoginCommands = QStringLiteral("BOGUS");
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && (R.Message + R.MoreMessages).contains(QStringLiteral("BOGUS")), Describe(R));
  }
}

static void TestConsoleChecksumCopy(Env & E)
{
  std::printf("[raw command console]\n");
  Terminal T(Site(E, 0), E.UI);
  OperationResult R = DoOpen(T);
  CHECK_MSG(R.Ok, Describe(R));
  QString Dir = QStringLiteral("/ex-%1").arg(QRandomGenerator::global()->bounded(100000));
  CHECK_MSG(MakeDir(T, Dir).Ok, Dir);
  ConsoleResult C = DoConsole(T, QStringLiteral("SYST"));
  CHECK_MSG(C.Result.Ok && (C.Lines.size() == 1) && C.Lines.value(0).startsWith(QStringLiteral("215 ")), C.Lines.join(QLatin1Char('|')));
  C = DoConsole(T, QStringLiteral("CWD ") + Dir.mid(1));
  CHECK_MSG(C.Result.Ok && (C.Directory == Dir), C.Directory);
  CHECK(T.CurrentDirectory() == Dir);
  C = DoConsole(T, QStringLiteral("FEAT"));
  CHECK_MSG(C.Result.Ok && (C.Lines.size() > 3) && C.Lines.value(0).startsWith(QStringLiteral("211-")), C.Lines.join(QLatin1Char('|')));
  C = DoConsole(T, QStringLiteral("BOGUS"));
  CHECK_MSG(!C.Result.Ok && !C.Result.Fatal && (C.Lines.size() == 1) && C.Lines.value(0).startsWith(QStringLiteral("500")), Describe(C.Result));
  // the session stays usable
  CHECK(DoList(T, Dir).Ok);

  std::printf("[checksums: HASH]\n");
  QByteArray Content("hello world\n");
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("hash file.txt")), Content);
  CHECK(Upload(T, QDir(E.Local).filePath(QStringLiteral("hash file.txt")), Dir).Ok);
  QString Remote = Dir + QStringLiteral("/hash file.txt");
  auto Results = std::make_shared<QStringList>();
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
    *Results << Ftp.ChecksumAlgorithms().join(QLatin1Char(','));
    *Results << QString::number(Ftp.CanCalculateChecksum());
    *Results << Ftp.CalculateFileChecksum(Remote, QStringLiteral("sha-256"));
    *Results << Ftp.CalculateFileChecksum(Remote, QStringLiteral("SHA1"));
    *Results << Ftp.CalculateFileChecksum(Remote, QStringLiteral("md5"));
    *Results << Ftp.CalculateFileChecksum(Remote, QStringLiteral("crc32"));
  });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK_MSG(Results->value(0) == QStringLiteral("sha-1,sha-256,md5,crc32"), Results->value(0));
  CHECK(Results->value(1) == QStringLiteral("1"));
  CHECK_MSG(Results->value(2) == QString::fromLatin1(QCryptographicHash::hash(Content, QCryptographicHash::Sha256).toHex()), Results->value(2));
  CHECK_MSG(Results->value(3) == QString::fromLatin1(QCryptographicHash::hash(Content, QCryptographicHash::Sha1).toHex()), Results->value(3));
  CHECK_MSG(Results->value(4) == QString::fromLatin1(QCryptographicHash::hash(Content, QCryptographicHash::Md5).toHex()), Results->value(4));
  CHECK_MSG(Results->value(5) == QStringLiteral("af083b2d"), Results->value(5));
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    dynamic_cast<FtpFileSystem &>(Fs).CalculateFileChecksum(Remote, QStringLiteral("whirlpool"));
  });
  CHECK_MSG(!R.Ok && R.Message.contains(QStringLiteral("is not supported")), Describe(R));
  QStringList Lines = XLog(T);
  CHECK(IndexOfPrefix(Lines, QStringLiteral("OPTS HASH SHA-256")) >= 0);
  CHECK(IndexOfPrefix(Lines, QStringLiteral("HASH \"") + Remote) >= 0);

  std::printf("[remote copy: SITE CPFR/CPTO]\n");
  auto CanCopy = std::make_shared<bool>(false);
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
    *CanCopy = Ftp.CanRemoteCopy();
    Ftp.CopyFile(Remote, Dir + QStringLiteral("/copy.txt"));
  });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK(*CanCopy);
  RemoteFileList Files;
  CHECK(DoList(T, Dir, &Files).Ok);
  bool Found = false;
  for (const RemoteFile & F : Files)
  {
    Found |= ((F.FileName == QStringLiteral("copy.txt")) && (F.Size == Content.size()));
  }
  CHECK(Found);
  if (!E.Root.isEmpty())
  {
    CHECK(ReadLocal(E.Root + Dir + QStringLiteral("/copy.txt")) == Content);
  }
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    dynamic_cast<FtpFileSystem &>(Fs).CopyFile(Dir + QStringLiteral("/missing.txt"), Dir + QStringLiteral("/copy2.txt"));
  });
  CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error copying file")), Describe(R));

  std::printf("[capabilities, append upload]\n");
  auto Caps = std::make_shared<QList<bool>>();
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("append1.txt")), QByteArray("123"));
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("append2.txt")), QByteArray("456"));
  CHECK(Upload(T, QDir(E.Local).filePath(QStringLiteral("append1.txt")), Dir).Ok);
  QString AppendSource = QDir(E.Local).filePath(QStringLiteral("append2.txt"));
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
  {
    *Caps << Fs.IsCapable(fcAnyCommand, false) << Fs.IsCapable(fcShellAnyCommand, true)
      << Fs.IsCapable(fcRemoteCopy, false) << Fs.IsCapable(fcCalculatingChecksum, false)
      << Fs.IsCapable(fcMoveOverExistingFile, false) << Fs.IsCapable(fcAppendUpload, false)
      << Fs.IsCapable(fcPreservingTimestampUpload, false) << Fs.IsCapable(fcSymbolicLink, true);
    TransferParams Params;
    Params.Append = true;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Fs.Upload(AppendSource, Dir + QStringLiteral("/append1.txt"), 0644, Params, Progress);
    Progress.Stop();
  });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK((*Caps == QList<bool>{true, false, true, true, true, true, true, false}));
  if (!E.Root.isEmpty())
  {
    CHECK(ReadLocal(E.Root + Dir + QStringLiteral("/append1.txt")) == QByteArray("123456"));
  }

  std::printf("[text mode: TYPE A]\n");
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("lf.txt")), QByteArray("m\nn\n"));
  CHECK(Upload(T, QDir(E.Local).filePath(QStringLiteral("lf.txt")), Dir).Ok);
  QString LocalAscii = QDir(E.Local).filePath(QStringLiteral("ascii-up.txt"));
  WriteLocal(LocalAscii, QByteArray("a\nb\r\nc\n"));
  QString DownloadedAscii = QDir(E.Local).filePath(QStringLiteral("ascii-down.txt"));
  R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
  {
    FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
    Ftp.SetAsciiTransfer(true);
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Ftp.Upload(LocalAscii, Dir + QStringLiteral("/ascii.txt"), 0644, Progress);
    // the server sends its LF files with CR/LF in ASCII mode
    Ftp.Download(Dir + QStringLiteral("/lf.txt"), DownloadedAscii, Progress);
    Ftp.SetAsciiTransfer(false);
    Progress.Stop();
  });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK_MSG(ReadLocal(DownloadedAscii) == QByteArray("m\nn\n"), QString::fromLatin1(ReadLocal(DownloadedAscii).toHex()));
  if (!E.Root.isEmpty())
  {
    CHECK_MSG(ReadLocal(E.Root + Dir + QStringLiteral("/ascii.txt")) == QByteArray("a\nb\nc\n"),
      QString::fromLatin1(ReadLocal(E.Root + Dir + QStringLiteral("/ascii.txt")).toHex()));
  }
  Lines = XLog(T);
  qsizetype TypeA = Lines.lastIndexOf(QStringLiteral("TYPE A"));
  qsizetype Stor = IndexOfPrefix(Lines, QStringLiteral("STOR ") + Dir + QStringLiteral("/ascii.txt"));
  CHECK((TypeA >= 0) && (Stor > TypeA));
}

static void TestActiveMode(Env & E)
{
  std::printf("[active mode: external IP address and port range]\n");
  Configuration & Config = Configuration::Instance();
  int Port = 41000 + static_cast<int>(QRandomGenerator::global()->bounded(2000));
  Config.LocalPortNumberMin = Port;
  Config.LocalPortNumberMax = Port;
  SessionData Data = Site(E, 0);
  Data.FtpPasvMode = false;
  {
    // the server cannot connect to the announced address
    Config.ExternalIpAddress = QStringLiteral("127.0.0.2");
    Terminal T(Data, E.UI);
    CHECK(DoOpen(T).Ok);
    OperationResult R = DoList(T, QStringLiteral("/"));
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    QStringList Lines = XLog(T);
    QString Expected = QStringLiteral("PORT 127,0,0,2,%1,%2").arg(Port >> 8).arg(Port & 0xFF);
    CHECK_MSG(Lines.contains(Expected), Lines.join(QLatin1Char('|')));
  }
  {
    // host name as the external address
    Config.ExternalIpAddress = QStringLiteral("localhost");
    Terminal T(Data, E.UI);
    CHECK(DoOpen(T).Ok);
    OperationResult R = DoList(T, QStringLiteral("/"));
    CHECK_MSG(R.Ok, Describe(R));
    QStringList Lines = XLog(T);
    QString Expected = QStringLiteral("PORT 127,0,0,1,%1,%2").arg(Port >> 8).arg(Port & 0xFF);
    CHECK_MSG(Lines.contains(Expected), Lines.join(QLatin1Char('|')));
  }
  Config.ExternalIpAddress.clear();
  Config.LocalPortNumberMin = 0;
  Config.LocalPortNumberMax = 0;
}

static void TestNoMlsd(Env & E)
{
  std::printf("[file properties with SIZE/MDTM (no MLST)]\n");
  {
    Terminal T(Site(E, 1), E.UI);
    CHECK(DoOpen(T).Ok);
    QString Dir = QStringLiteral("/lf-%1").arg(QRandomGenerator::global()->bounded(100000));
    CHECK(MakeDir(T, Dir).Ok);
    QString LocalFile = QDir(E.Local).filePath(QStringLiteral("props.txt"));
    WriteLocal(LocalFile, QByteArray(1234, 'x'));
    QDateTime Time = QDateTime::fromSecsSinceEpoch(1600000000 + 37);
    {
      QFile F(LocalFile);
      CHECK(F.open(QIODevice::ReadWrite));
      CHECK(F.setFileTime(Time, QFileDevice::FileModificationTime));
    }
    CHECK(Upload(T, LocalFile, Dir).Ok);
    auto File = std::make_shared<RemoteFile>();
    auto DirFile = std::make_shared<RemoteFile>();
    auto Exists = std::make_shared<bool>(true);
    OperationResult R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *File = Fs.ReadFile(Dir + QStringLiteral("/props.txt"));
      *DirFile = Fs.ReadFile(Dir);
      *Exists = Fs.FileExists(Dir + QStringLiteral("/missing.txt"), nullptr);
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(File->Size == 1234);
    CHECK(!File->IsDirectory);
    CHECK_MSG(File->Modification.toSecsSinceEpoch() == Time.toSecsSinceEpoch(), File->Modification.toString(Qt::ISODate));
    CHECK(DirFile->IsDirectory);
    CHECK(!*Exists);
    QStringList Lines = XLog(T);
    CHECK(IndexOfPrefix(Lines, QStringLiteral("SIZE ") + Dir + QStringLiteral("/props.txt")) >= 0);
    CHECK(IndexOfPrefix(Lines, QStringLiteral("MDTM ") + Dir + QStringLiteral("/props.txt")) >= 0);

    std::printf("[checksums: algorithm specific commands]\n");
    auto Results = std::make_shared<QStringList>();
    QByteArray Content(1234, 'x');
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
      *Results << Ftp.ChecksumAlgorithms().join(QLatin1Char(','));
      *Results << Ftp.CalculateFileChecksum(Dir + QStringLiteral("/props.txt"), QStringLiteral("md5"));
      *Results << Ftp.CalculateFileChecksum(Dir + QStringLiteral("/props.txt"), QStringLiteral("sha-256"));
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(Results->value(0) == QStringLiteral("sha-256,md5,crc32"), Results->value(0));
    CHECK(Results->value(1) == QString::fromLatin1(QCryptographicHash::hash(Content, QCryptographicHash::Md5).toHex()));
    CHECK(Results->value(2) == QString::fromLatin1(QCryptographicHash::hash(Content, QCryptographicHash::Sha256).toHex()));
  }
  std::printf("[work from current directory]\n");
  {
    SessionData Data = Site(E, 1);
    Data.FtpWorkFromCwd = asOn;
    Terminal T(Data, E.UI);
    CHECK(DoOpen(T).Ok);
    QString Dir = QStringLiteral("/wfc-%1").arg(QRandomGenerator::global()->bounded(100000));
    CHECK(MakeDir(T, Dir).Ok);
    CHECK(MakeDir(T, Dir + QStringLiteral("/sub")).Ok);
    QString LocalFile = QDir(E.Local).filePath(QStringLiteral("wfc.txt"));
    WriteLocal(LocalFile, QByteArray("work from cwd"));
    CHECK(Upload(T, LocalFile, Dir + QStringLiteral("/sub")).Ok);
    QString Downloaded = QDir(E.Local).filePath(QStringLiteral("wfc-down.txt"));
    OperationResult R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
    {
      Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
      Fs.Download(Dir + QStringLiteral("/sub/wfc.txt"), Downloaded, Progress);
      Progress.Stop();
      Fs.DeleteFile(Dir + QStringLiteral("/sub/wfc.txt"));
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadLocal(Downloaded) == QByteArray("work from cwd"));
    QStringList Lines = XLog(T);
    Log(Lines.mid(std::max<qsizetype>(0, Lines.size() - 14)).join(QStringLiteral(" | ")));
    CHECK(Lines.contains(QStringLiteral("CWD sub")));
    CHECK(Lines.contains(QStringLiteral("STOR wfc.txt")));
    CHECK(Lines.contains(QStringLiteral("RETR wfc.txt")));
    CHECK(Lines.contains(QStringLiteral("DELE wfc.txt")));
    CHECK(IndexOfPrefix(Lines, QStringLiteral("RETR /")) < 0);
  }
}

static void TestIis(Env & E)
{
  std::printf("[IIS: rename over an existing file]\n");
  Terminal T(Site(E, 5), E.UI);
  CHECK(DoOpen(T).Ok);
  QString Dir = QStringLiteral("/iis-%1").arg(QRandomGenerator::global()->bounded(100000));
  CHECK(MakeDir(T, Dir).Ok);
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("a.txt")), QByteArray("AAA"));
  WriteLocal(QDir(E.Local).filePath(QStringLiteral("b.txt")), QByteArray("B"));
  CHECK(Upload(T, QDir(E.Local).filePath(QStringLiteral("a.txt")), Dir).Ok);
  CHECK(Upload(T, QDir(E.Local).filePath(QStringLiteral("b.txt")), Dir).Ok);
  auto CanMove = std::make_shared<bool>(true);
  OperationResult R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
    *CanMove = Ftp.CanMoveOverExistingFile();
    Fs.RenameFile(Dir + QStringLiteral("/a.txt"), Dir + QStringLiteral("/b.txt"));
  });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK(!*CanMove);
  CHECK(T.ServerInfo().contains(QStringLiteral("Server: Windows_NT")));
  RemoteFileList Files;
  CHECK(DoList(T, Dir, &Files).Ok);
  CHECK((Files.size() == 1) && (Files.value(0).FileName == QStringLiteral("b.txt")) && (Files.value(0).Size == 3));
}

static void TestProxies(Env & E, int ProxyPort, const QString & ProxyLog)
{
  struct Case
  {
    TProxyMethod Method;
    const char * Kind;
    const char * User;
    const char * Password;
  };
  const Case Cases[] = {
    {pmSocks4, "socks4a", "", ""},
    {pmSocks5, "socks5", "puser", "ppass"},
    {pmHTTP, "http", "", ""},
  };
  for (const Case & C : Cases)
  {
    std::printf("[proxy: %s]\n", C.Kind);
    E.UI->Reset();
    SessionData Data = Site(E, 0);
    // resolvable by the proxy only
    Data.HostName = QStringLiteral("ftp.nixscp.test");
    Data.ProxyMethod = C.Method;
    Data.ProxyHost = QStringLiteral("127.0.0.1");
    Data.ProxyPort = ProxyPort;
    Data.ProxyUsername = QString::fromLatin1(C.User);
    Data.ProxyPassword = QString::fromLatin1(C.Password);
    QFile::resize(ProxyLog, 0);
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    if (!R.Ok)
    {
      continue;
    }
    R = DoList(T, QStringLiteral("/"));
    CHECK_MSG(R.Ok, Describe(R));
    QString LogText = QString::fromLatin1(ReadLocal(ProxyLog));
    CHECK_MSG(LogText.contains(QStringLiteral("%1 ftp.nixscp.test:%2").arg(QString::fromLatin1(C.Kind)).arg(Data.PortNumber)), LogText);
    // passive data connection through the proxy too
    CHECK_MSG(LogText.contains(QStringLiteral("%1 127.0.0.1:").arg(QString::fromLatin1(C.Kind))), LogText);
  }
  std::printf("[proxy: SOCKS4 refused target]\n");
  {
    SessionData Data = Site(E, 0);
    Data.HostName = QStringLiteral("ftp.nixscp.test");
    Data.PortNumber = 1;
    Data.ProxyMethod = pmSocks4;
    Data.ProxyHost = QStringLiteral("127.0.0.1");
    Data.ProxyPort = ProxyPort;
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && R.MoreMessages.contains(QStringLiteral("Proxy request failed")), Describe(R));
  }
}

static void TestFtpProxyLogonTypes(Env & E)
{
  const char * const Expected[9] = {
    "",
    "USER fwuser|PASS ****|SITE ftp.target.test|USER test|PASS ****",
    "USER fwuser|PASS ****|USER test@ftp.target.test|PASS ****",
    "OPEN ftp.target.test|USER test|PASS ****",
    "USER fwuser|PASS ****|USER test|PASS ****",
    "USER test@ftp.target.test|PASS ****",
    "USER fwuser@ftp.target.test|PASS ****|USER test|PASS ****",
    "USER test@ftp.target.test fwuser|PASS ****|ACCT fwpass",
    "USER test@fwuser@ftp.target.test|PASS ****",
  };
  for (int Type = 1; Type <= 8; Type++)
  {
    std::printf("[FTP proxy logon type %d]\n", Type);
    E.UI->Reset();
    SessionData Data = E.Base;
    Data.HostName = QStringLiteral("ftp.target.test");
    Data.PortNumber = 21;
    Data.ProxyMethod = pmNone;
    Data.ProxyHost = QStringLiteral("127.0.0.1");
    Data.ProxyPort = E.Port + 6;
    Data.ProxyUsername = QStringLiteral("fwuser");
    Data.ProxyPassword = QStringLiteral("fwpass");
    Data.FtpProxyLogonType = Type;
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, QStringLiteral("type %1: %2").arg(Type).arg(Describe(R)));
    if (!R.Ok)
    {
      continue;
    }
    CHECK(DoList(T, QStringLiteral("/")).Ok);
    QStringList Lines = XLog(T);
    QString Logon = Lines.mid(0, QString::fromLatin1(Expected[Type]).count(QLatin1Char('|')) + 1).join(QLatin1Char('|'));
    CHECK_MSG(Logon == QString::fromLatin1(Expected[Type]), Logon);
  }
}

static void TestTls(Env & E)
{
  QString Certs = E.Certs;
  std::printf("[TLS client certificate: CheckCertificate]\n");
  {
    auto Check = [](const QString & Path) -> QString
    {
      try
      {
        FtpFileSystem::CheckCertificate(Path);
        return QString();
      }
      catch (const ExtException & Ex)
      {
        return Ex.Message() + QLatin1Char('|') + Ex.MoreMessages();
      }
    };
    CHECK(Check(Certs + QStringLiteral("/client-sep.key")).isEmpty());
    CHECK(Check(Certs + QStringLiteral("/client-combined.pem")).isEmpty());
    CHECK(Check(Certs + QStringLiteral("/client.p12")).isEmpty());
    QString Error = Check(Certs + QStringLiteral("/missing.pem"));
    CHECK_MSG(Error.startsWith(QStringLiteral("Cannot open certificate")), Error);
    Error = Check(Certs + QStringLiteral("/client.csr"));
    CHECK_MSG(Error.startsWith(QStringLiteral("Cannot read certificate")), Error);
    Error = Check(Certs + QStringLiteral("/client-ed.key"));
    CHECK_MSG(Error.contains(QStringLiteral("does not contain a public key")), Error);
  }
  SessionData Data = Site(E, 2);
  Data.HostName = QStringLiteral("localhost");
  Data.Ftps = ftpsExplicitTls;
  // the test CA is not trusted: accept any certificate as configured
  Data.HostKey = QStringLiteral("*");
  std::printf("[TLS client certificate: none]\n");
  {
    E.UI->Reset();
    Terminal T(Data, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    CHECK(E.UI->Queries.isEmpty());
  }
  std::printf("[TLS client certificate: encrypted PEM, wrong passphrase first]\n");
  {
    E.UI->Reset();
    E.UI->PromptAnswers = {QStringLiteral("wrong"), QStringLiteral("certpass")};
    SessionData D = Data;
    D.TlsCertificateFile = Certs + QStringLiteral("/client-combined.pem");
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK((E.UI->PromptKinds.size() == 2) && (E.UI->PromptKinds.value(0) == UserInterface::pkPassphrase));
    CHECK(E.UI->PromptTexts.value(0) == QStringLiteral("&Passphrase for client certificate:"));
    CHECK(E.UI->PromptInstructions.value(0).isEmpty());
    CHECK(E.UI->PromptInstructions.value(1) == QStringLiteral("Error decoding certificate."));
    CHECK_MSG(T.ServerInfo().contains(QStringLiteral("Client certificate: wp07 client")), T.ServerInfo());
    // data connections present the certificate too
    CHECK(DoList(T, QStringLiteral("/")).Ok);
  }
  std::printf("[TLS client certificate: PKCS#12]\n");
  {
    E.UI->Reset();
    E.UI->PromptAnswers = {QStringLiteral("certpass")};
    SessionData D = Data;
    D.TlsCertificateFile = Certs + QStringLiteral("/client.p12");
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(E.UI->PromptKinds.size() == 1);
    CHECK(DoList(T, QStringLiteral("/")).Ok);
  }
  std::printf("[TLS client certificate: key with .crt next to it]\n");
  {
    E.UI->Reset();
    SessionData D = Data;
    D.TlsCertificateFile = Certs + QStringLiteral("/client-sep.key");
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(E.UI->PromptKinds.isEmpty());
  }
  std::printf("[TLS client certificate: passphrase prompt cancelled]\n");
  {
    E.UI->Reset();
    SessionData D = Data;
    D.TlsCertificateFile = Certs + QStringLiteral("/client-combined.pem");
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    // connects without the certificate, which the server refuses
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    CHECK(E.UI->PromptKinds.size() == 1);
  }

  std::printf("[TLS versions]\n");
  SessionData Tls12 = Site(E, 3);
  Tls12.HostName = QStringLiteral("localhost");
  Tls12.Ftps = ftpsExplicitTls;
  Tls12.HostKey = QStringLiteral("*");
  {
    Terminal T(Tls12, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok && T.ServerInfo().contains(QStringLiteral("Explicit encryption, TLSv1.2\n")), T.ServerInfo());
  }
  {
    SessionData D = Tls12;
    D.MinTlsVersion = tls13;
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && (R.Message == QStringLiteral("Can't establish TLS connection.")), Describe(R));
  }
  SessionData Tls10 = Site(E, 4);
  Tls10.HostName = QStringLiteral("localhost");
  Tls10.Ftps = ftpsExplicitTls;
  Tls10.HostKey = QStringLiteral("*");
  {
    // TLS 1.0 is below the default minimum (TLS 1.2)
    Terminal T(Tls10, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }
  {
    SessionData D = Tls10;
    D.MinTlsVersion = tls10;
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(T.ServerInfo().contains(QStringLiteral("Explicit encryption, TLSv1\n")), T.ServerInfo());
    CHECK(DoList(T, QStringLiteral("/")).Ok);
  }
  {
    // maximum below the server's minimum
    SessionData D = Site(E, 0);
    D.PortNumber = E.Port + 3;
    D.HostName = QStringLiteral("localhost");
    D.Ftps = ftpsExplicitTls;
    D.HostKey = QStringLiteral("*");
    D.MinTlsVersion = tls10;
    D.MaxTlsVersion = tls11;
    Terminal T(D, E.UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }

  std::printf("[server certificate: verification and cache]\n");
  {
    SessionData D = Tls12;
    D.HostKey.clear();
    QString SiteKey = QStringLiteral("localhost:%1").arg(D.PortNumber);
    E.UI->Reset();
    E.UI->QueryAnswers = {qaYes};
    auto Sha1 = std::make_shared<QString>();
    auto Sha256 = std::make_shared<QString>();
    {
      Terminal T(D, E.UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(E.UI->Queries.size() == 1);
      QString Query = E.UI->Queries.value(0);
      // CA not in the system store, host name verified
      // the server sends its CA, which is not trusted (X509 depth 1)
      CHECK_MSG(Query.contains(QStringLiteral("Self-signed certificate in certificate chain. The error occurred at a depth of 2 in the certificate chain.")), Query);
      CHECK(!Query.contains(QStringLiteral("IP address")));
      CHECK(!Query.contains(QStringLiteral("not issued for this server")));
      CHECK(Query.contains(QStringLiteral("NixSCP WP07 CA")));
      DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
      {
        FtpFileSystem & Ftp = dynamic_cast<FtpFileSystem &>(Fs);
        *Sha1 = Ftp.CertificateFingerprintSHA1();
        *Sha256 = Ftp.CertificateFingerprintSHA256();
      });
      CHECK(T.ServerInfo().contains(QStringLiteral("Certificate fingerprint (SHA-1): ") + *Sha1));
    }
    // X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN = 19
    QString Cached = IniValue(QStringLiteral("FtpsCertificates"), SiteKey);
    CHECK_MSG(Cached == *Sha256 + QStringLiteral(";13"), Cached);
    {
      E.UI->Reset();
      Terminal T(D, E.UI);
      CHECK(DoOpen(T).Ok);
      CHECK(E.UI->Queries.isEmpty());
    }
    {
      // different failures than cached: asked again
      IniStorage Storage(&Configuration::Instance().Ini());
      CHECK(Storage.OpenSubKey(QStringLiteral("FtpsCertificates"), false));
      Storage.WriteString(SiteKey, *Sha256 + QStringLiteral(";14"));
      E.UI->Reset();
      E.UI->QueryAnswers = {qaNo};
      Terminal T(D, E.UI);
      CHECK(DoOpen(T).Ok);
      CHECK(E.UI->Queries.size() == 1);
      // "No" does not store
      CHECK(IniValue(QStringLiteral("FtpsCertificates"), SiteKey) == Cached);
    }
    {
      // fingerprint configured for the site (SshHostKey)
      IniStorage Storage(&Configuration::Instance().Ini());
      CHECK(Storage.OpenSubKey(QStringLiteral("FtpsCertificates"), false));
      Storage.DeleteValue(SiteKey);
      E.UI->Reset();
      SessionData WithKey = D;
      WithKey.HostKey = QStringLiteral("00:11;") + Sha1->toUpper();
      Terminal T(WithKey, E.UI);
      CHECK(DoOpen(T).Ok);
      CHECK(E.UI->Queries.isEmpty());
    }
    {
      // IP address: the host name cannot be verified
      E.UI->Reset();
      SessionData Ip = D;
      Ip.HostName = QStringLiteral("127.0.0.1");
      Terminal T(Ip, E.UI);
      CHECK(!DoOpen(T).Ok);
      CHECK(E.UI->Queries.size() == 1);
      CHECK(E.UI->Queries.value(0).contains(QStringLiteral("When connecting using an IP address")));
    }
    {
      // CertificateStorage: the test CA trusted in addition to the system store
      std::printf("[server certificate: CertificateStorage]\n");
      IniStorage Storage(&Configuration::Instance().Ini());
      CHECK(Storage.OpenSubKey(QStringLiteral("Configuration"), true) && Storage.OpenSubKey(QStringLiteral("Interface"), true));
      Storage.WriteString(QStringLiteral("CertificateStorage"), Certs + QStringLiteral("/ca.crt"));
      E.UI->Reset();
      Terminal T(D, E.UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(E.UI->Queries.isEmpty());
      CHECK(T.ServerInfo().contains(QStringLiteral("Summary: Certificate is valid.")));
      Storage.DeleteValue(QStringLiteral("CertificateStorage"));
    }
  }
}

static void TestKeepalive(Env & E)
{
  std::printf("[keepalive: dummy commands and directory listing]\n");
  SessionData Data = Site(E, 0);
  Data.FtpPingType = fptDirectoryListing;
  Data.FtpPingInterval = 1;
  Terminal T(Data, E.UI);
  CHECK(DoOpen(T).Ok);
  QEventLoop Loop;
  QTimer::singleShot(6500, &Loop, &QEventLoop::quit);
  Loop.exec();
  QStringList Lines = XLog(T);
  qsizetype LastPwd = IndexOfPrefix(Lines, QStringLiteral("PWD"));
  int Dummy = 0;
  for (qsizetype Index = LastPwd + 1; Index < Lines.size(); Index++)
  {
    if ((Lines[Index] == QStringLiteral("PWD")) || (Lines[Index] == QStringLiteral("REST 0")) ||
        (Lines[Index] == QStringLiteral("TYPE A")) || (Lines[Index] == QStringLiteral("TYPE I")))
    {
      Dummy++;
    }
  }
  CHECK_MSG(Dummy >= 2, Lines.join(QLatin1Char('|')));
  CHECK_MSG(IndexOfPrefix(Lines, QStringLiteral("MLSD")) >= 0, Lines.join(QLatin1Char('|')));
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  // never touch the user's configuration
  QStandardPaths::setTestModeEnabled(true);
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));

  TestCertificateCache();
  TestHelpers();

  QByteArray PortStr = qgetenv("NIXSCP_TEST_FTPX_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_ftpex: server tests SKIPPED (set NIXSCP_TEST_FTPX_PORT [, NIXSCP_TEST_FTPX_ROOT, NIXSCP_TEST_FTPX_CERTS, "
      "NIXSCP_TEST_PROXY_PORT, NIXSCP_TEST_PROXY_LOG, NIXSCP_TEST_SSH_PORT])\n");
    std::printf("%s\n", (Failures == 0) ? "test_ftpex: all tests passed" : "test_ftpex: FAILURES");
    return (Failures == 0) ? 0 : 1;
  }
  TestUserInterface UI;
  Env E;
  E.UI = &UI;
  E.Port = PortStr.toInt();
  E.Root = qEnvironmentVariable("NIXSCP_TEST_FTPX_ROOT");
  E.Certs = qEnvironmentVariable("NIXSCP_TEST_FTPX_CERTS");
  E.Local = Scratch.path();
  E.Base.FSProtocol = fsFTP;
  E.Base.HostName = QStringLiteral("127.0.0.1");
  E.Base.UserName = QStringLiteral("test");
  E.Base.Password = QStringLiteral("secret");
  E.Base.Timeout = 10;

  // NIXSCP_TEST_FTPX_ONLY=<group> runs one group (login, console, active,
  // nomlsd, iis, proxy, ftpproxy, tls, keepalive, sftp)
  QString Only = qEnvironmentVariable("NIXSCP_TEST_FTPX_ONLY");
  auto Run = [&](const char * Group) { return Only.isEmpty() || (Only == QLatin1String(Group)); };
  if (Run("login")) TestLoginSequence(E);
  if (Run("console")) TestConsoleChecksumCopy(E);
  if (Run("active")) TestActiveMode(E);
  if (Run("nomlsd")) TestNoMlsd(E);
  if (Run("iis")) TestIis(E);
  int ProxyPort = qEnvironmentVariableIntValue("NIXSCP_TEST_PROXY_PORT");
  QString ProxyLog = qEnvironmentVariable("NIXSCP_TEST_PROXY_LOG");
  if ((ProxyPort > 0) && !ProxyLog.isEmpty() && Run("proxy"))
  {
    TestProxies(E, ProxyPort, ProxyLog);
  }
  if (Run("ftpproxy")) TestFtpProxyLogonTypes(E);
  if (!E.Certs.isEmpty() && Run("tls"))
  {
    TestTls(E);
  }
  if (Run("keepalive")) TestKeepalive(E);

  int SshPort = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT");
  if ((SshPort > 0) && Run("sftp"))
  {
    std::printf("[SFTP server detection]\n");
    SessionData Data = E.Base;
    Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    Data.PortNumber = SshPort;
    Data.Timeout = 5;
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && R.MoreMessages.contains(QStringLiteral("You cannot connect to an SFTP server using an FTP protocol")), Describe(R));
  }

  std::printf("%s\n", (Failures == 0) ? "test_ftpex: all tests passed" : "test_ftpex: FAILURES");
  return (Failures == 0) ? 0 : 1;
}
