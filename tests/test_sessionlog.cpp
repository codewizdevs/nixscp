// Tests of core/SessionLog (TSessionLog) and core/ActionsLog (TActionLog):
// line format, levels, masking of sensitive data, file name patterns,
// append/overwrite, rotation, the XML log schema; and a real SFTP session
// against the test SSH server producing both logs (skipped unless
// NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY are set, see test_sftp.cpp).
#include "core/ActionsLog.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/SessionLog.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QXmlStreamReader>

#include <cstdio>
#include <stdexcept>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(QString(Msg))); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto _a = (A); auto _b = (B); if (!(_a == _b)) { std::printf("FAIL %s:%d: %s == %s\n  got: %s\n  exp: %s\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(_a).toString()), qPrintable(QVariant::fromValue(_b).toString())); std::fflush(stdout); Failures++; } } while (0)

static QStringList ReadLines(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QStringList();
  }
  QString Text = QString::fromUtf8(F.readAll());
  if (Text.endsWith(QLatin1Char('\n')))
  {
    Text.chop(1);
  }
  return Text.isEmpty() ? QStringList() : Text.split(QLatin1Char('\n'));
}

static QString ReadText(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? QString::fromUtf8(F.readAll()) : QString();
}

// "<mark> yyyy-mm-dd hh:nn:ss.zzz <text>"
static const QRegularExpression LineFormat(QStringLiteral("^([<>!.*]) (\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{3}) (.*)$"));

static QString LineText(const QString & Line)
{
  QRegularExpressionMatch M = LineFormat.match(Line);
  return M.hasMatch() ? M.captured(3) : QString();
}

static SessionLogOptions Options(const QString & FileName, int Level = 0)
{
  SessionLogOptions O;
  O.Logging = true;
  O.FileName = FileName;
  O.Protocol = Level;
  return O;
}

static void TestExpandFileName()
{
  SessionData Data;
  Data.HostName = QStringLiteral("example.com");
  Data.UserName = QStringLiteral("joe");
  Data.Name = QStringLiteral("folder/My site");
  QDateTime Started(QDate(2026, 3, 4), QTime(5, 6, 7));
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("/l/!Y-!M-!D-!T.log"), Started, &Data), QStringLiteral("/l/2026-03-04-050607.log"));
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("!@_!S"), Started, &Data), QStringLiteral("example.com_folder_My_site"));
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("!s!@"), Started, nullptr), QStringLiteral("nosessionnohost"));
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("a!!b!xc!"), Started, nullptr), QStringLiteral("a!b!xc!"));
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("p!P"), Started, nullptr), QStringLiteral("p%1").arg(QCoreApplication::applicationPid()));
  qputenv("NIXSCP_TEST_LOGDIR", "/var/x");
  CHECK_EQ(SessionLog::ExpandFileName(QStringLiteral("$NIXSCP_TEST_LOGDIR/!y.log"), Started, nullptr), QStringLiteral("/var/x/2026.log"));
  CHECK_EQ(SessionLog::PartFileName(QStringLiteral("/a.log"), 0), QStringLiteral("/a.log"));
  CHECK_EQ(SessionLog::PartFileName(QStringLiteral("/a.log"), 3), QStringLiteral("/a.log.3"));
  CHECK(SessionLog::DefaultLogFileName().endsWith(QStringLiteral("/nixscp/!S.log")));
  CHECK(SessionLog::DefaultActionsLogFileName().endsWith(QStringLiteral("/nixscp/!S.xml")));
}

static void TestFormatAndLevels(const QString & Dir)
{
  QString FileName = Dir + QStringLiteral("/format.log");
  {
    auto Log = std::make_shared<SessionLog>(Options(FileName, 0));
    CHECK(Log->Open());
    CHECK_EQ(Log->CurrentFileName(), FileName);
    Log->Add(llOutput, QStringLiteral("output"));
    Log->Add(llInput, QStringLiteral("input"));
    Log->Add(llStdError, QStringLiteral("stderr"));
    Log->Add(llMessage, QStringLiteral("first\r\n\nthird\n"));
    Log->AddException(QStringLiteral("Failed"), QStringLiteral("More"));
    Log->Add(llMessage, QString());
    Log->SetName(QStringLiteral("Background 1"));
    Log->Add(llMessage, QStringLiteral("named"));
    // thread hooks honour the level
    SessionLog::SetCurrent(Log);
    CHECK(SessionLog::Current() == Log.get());
    CHECK(SessionLog::IsLogging(0));
    CHECK(SessionLog::IsLogging(-1));
    CHECK(!SessionLog::IsLogging(1));
    Log->SetName(QString());
    SessionLog::LogEvent(QStringLiteral("normal event"));
    SessionLog::LogEvent(1, QStringLiteral("debug event"));
    SessionLog::LogLine(llOutput, QStringLiteral("normal line"), 0);
    SessionLog::LogLine(llOutput, QStringLiteral("debug2 line"), 2);
    SessionLog::LogProtocolLine(QStringLiteral("> USER joe"));
    SessionLog::LogProtocolLine(QStringLiteral("< 331 Password required"));
    SessionLog::LogProtocolLine(QStringLiteral("plain"));
    SessionLog::LogQuery(QStringLiteral("Overwrite?"), QStringLiteral("a\nb"));
    SessionLog::LogAnswer(qaRetry);
    SessionLog::SetCurrent(nullptr);
    CHECK(SessionLog::Current() == nullptr);
    SessionLog::LogEvent(QStringLiteral("not logged"));
  }
  QStringList Lines = ReadLines(FileName);
  for (const QString & Line : Lines)
  {
    CHECK_MSG(LineFormat.match(Line).hasMatch(), Line);
  }
  QStringList Expected = {
    QStringLiteral("< output"), QStringLiteral("> input"), QStringLiteral("! stderr"),
    QStringLiteral(". first"), QStringLiteral(". "), QStringLiteral(". third"),
    QStringLiteral("* (ExtException) Failed"), QStringLiteral("* More"),
    QStringLiteral(". [Background 1] named"),
    QStringLiteral(". normal event"), QStringLiteral("< normal line"),
    QStringLiteral("> USER joe"), QStringLiteral("< 331 Password required"), QStringLiteral(". plain"),
    QStringLiteral(". Asking user:"), QStringLiteral(". Overwrite? (a,b)"), QStringLiteral(". Answer: Retry")};
  CHECK_EQ(Lines.size(), Expected.size());
  for (qsizetype Index = 0; (Index < Lines.size()) && (Index < Expected.size()); Index++)
  {
    QString Actual = Lines[Index].left(1) + QLatin1Char(' ') + LineText(Lines[Index]);
    CHECK_EQ(Actual, Expected[Index]);
  }

  // Debug 2 logs everything
  QString DebugName = Dir + QStringLiteral("/debug.log");
  {
    auto Log = std::make_shared<SessionLog>(Options(DebugName, 2));
    SessionLog::SetCurrent(Log);
    SessionLog::LogEvent(2, QStringLiteral("debug2 event"));
    SessionLog::LogEvent(1, QStringLiteral("debug1 event"));
    SessionLog::SetCurrent(nullptr);
  }
  CHECK_EQ(ReadLines(DebugName).size(), 2);

  // Reduced: only the level -1 lines
  QString ReducedName = Dir + QStringLiteral("/reduced.log");
  {
    auto Log = std::make_shared<SessionLog>(Options(ReducedName, -1));
    SessionLog::SetCurrent(Log);
    SessionLog::LogEvent(QStringLiteral("event"));
    SessionLog::LogLine(llOutput, QStringLiteral("output"), 0);
    SessionLog::SetCurrent(nullptr);
  }
  CHECK_EQ(ReadLines(ReducedName).size(), 1);

  // Not logging: no file
  SessionLogOptions Off = Options(Dir + QStringLiteral("/off.log"));
  Off.Logging = false;
  SessionLog OffLog(Off);
  OffLog.Add(llMessage, QStringLiteral("x"));
  CHECK(!OffLog.Logging());
  CHECK(!QFileInfo::exists(Dir + QStringLiteral("/off.log")));

  // Error opening: logging is turned off
  SessionLog Bad(Options(QStringLiteral("/proc/no/such/dir/x.log")));
  QString Error;
  CHECK(!Bad.Open(&Error));
  CHECK(Error.contains(QStringLiteral("Can't open log file")));
  CHECK(!Bad.Logging());
  Bad.Add(llMessage, QStringLiteral("x"));
}

static void TestSensitive(const QString & Dir)
{
  SessionData Data;
  Data.HostName = QStringLiteral("host.example");
  Data.UserName = QStringLiteral("joe");
  Data.Password = QStringLiteral("s3cr3t");
  Data.FSProtocol = fsFTP;
  Data.PortNumber = 21;
  Data.ProxyMethod = pmSocks5;
  Data.ProxyHost = QStringLiteral("proxy");
  Data.ProxyPassword = QStringLiteral("pr0xy");

  QString Masked = Dir + QStringLiteral("/masked.log");
  {
    auto Log = std::make_shared<SessionLog>(Options(Masked), &Data);
    Log->AddSystemInfo(QStringLiteral("nixscp ftp://joe:***@host.example"));
    Log->AddSystemInfo(QStringLiteral("again"));   // once per log
    Log->AddStartupInfo(Data);
    SessionLog::SetCurrent(Log);
    CHECK_EQ(SessionLog::MaskCommand(QStringLiteral("PASS s3cr3t"), QStringLiteral("s3cr3t")), QStringLiteral("PASS ******"));
    CHECK_EQ(SessionLog::LogSensitive(QStringLiteral("s3cr3t")), QStringLiteral("Yes"));
    CHECK_EQ(SessionLog::LogSensitive(QString()), QStringLiteral("No"));
    SessionLog::SetCurrent(nullptr);
  }
  QString Text = ReadText(Masked);
  CHECK(!Text.contains(QStringLiteral("s3cr3t")));
  CHECK(!Text.contains(QStringLiteral("pr0xy")));
  CHECK(Text.contains(QStringLiteral("Session name: joe@host.example (Ad-Hoc site)")));
  CHECK(Text.contains(QStringLiteral("Hostname: host.example (Port: 21)")));
  CHECK(Text.contains(QStringLiteral("Username: joe (Password: Yes, Key file: No, Passphrase: No)")));
  CHECK(Text.contains(QStringLiteral("Transfer Protocol: FTP")));
  CHECK(Text.contains(QStringLiteral("FTPS: None")));
  CHECK(Text.contains(QStringLiteral("Proxy: SOCKS5")));
  CHECK(Text.contains(QStringLiteral("Passwd: Yes")));
  CHECK(Text.contains(QStringLiteral("NixSCP Version")));
  CHECK(Text.contains(QStringLiteral("Log level: Normal")));
  CHECK(Text.contains(QStringLiteral("Command-line: nixscp ftp://joe:***@host.example")));
  CHECK(!Text.contains(QStringLiteral("again")));
  CHECK(Text.contains(QStringLiteral("Time zone: Current: GMT")));
  CHECK(Text.contains(SessionLog::Separator()));
  // the log file is private
  CHECK_EQ(int(QFileInfo(Masked).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)), 0);

  QString Plain = Dir + QStringLiteral("/sensitive.log");
  {
    SessionLogOptions O = Options(Plain, 1);
    O.Sensitive = true;
    O.MaxSize = 10 * 1024 * 1024;
    O.MaxCount = 3;
    auto Log = std::make_shared<SessionLog>(O, &Data);
    Log->AddSystemInfo();
    Log->AddStartupInfo(Data);
    SessionLog::SetCurrent(Log);
    CHECK_EQ(SessionLog::MaskCommand(QStringLiteral("PASS s3cr3t"), QStringLiteral("s3cr3t")), QStringLiteral("PASS s3cr3t"));
    SessionLog::SetCurrent(nullptr);
  }
  Text = ReadText(Plain);
  CHECK(Text.contains(QStringLiteral("(Password: s3cr3t,")));
  CHECK(Text.contains(QStringLiteral("Passwd: pr0xy")));
  CHECK(Text.contains(QStringLiteral("Log level: Debug 1, Logging passwords, Rotating after: 10M, Keeping at most 3 logs")));

  CHECK_EQ(SessionLog::MaskCommandLine({QStringLiteral("nixscp"), QStringLiteral("sftp://joe:pw@host/"), QStringLiteral("-password=x y")}),
    QStringLiteral("nixscp sftp://joe:***@host/ -password=***"));
}

static void TestAppendOverwrite(const QString & Dir)
{
  QString FileName = Dir + QStringLiteral("/append.log");
  {
    SessionLog Log(Options(FileName));
    Log.Add(llMessage, QStringLiteral("one"));
  }
  {
    SessionLog Log(Options(FileName));
    Log.Add(llMessage, QStringLiteral("two"));
  }
  CHECK_EQ(ReadLines(FileName).size(), 2);
  {
    SessionLogOptions O = Options(FileName);
    O.Append = false;
    SessionLog Log(O);
    Log.Add(llMessage, QStringLiteral("three"));
  }
  QStringList Lines = ReadLines(FileName);
  CHECK_EQ(Lines.size(), 1);
  CHECK_EQ(LineText(Lines.value(0)), QStringLiteral("three"));

  // Two logs of the same file share it (no overwriting each other)
  QString Shared = Dir + QStringLiteral("/shared.log");
  {
    SessionLogOptions O = Options(Shared);
    O.Append = false;
    SessionLog A(O);
    SessionLog B(O);
    A.Add(llMessage, QStringLiteral("a1"));
    B.Add(llMessage, QStringLiteral("b1"));
    A.Add(llMessage, QStringLiteral("a2"));
  }
  CHECK_EQ(ReadLines(Shared).size(), 3);

  // Lines from many threads stay whole
  QString Threads = Dir + QStringLiteral("/threads.log");
  {
    auto Log = std::make_shared<SessionLog>(Options(Threads));
    QList<QThread *> List;
    for (int T = 0; T < 4; T++)
    {
      List.append(QThread::create([Log, T]()
      {
        SessionLog::SetCurrent(Log);
        for (int I = 0; I < 200; I++)
        {
          SessionLog::LogEvent(QStringLiteral("thread %1 line %2").arg(T).arg(I));
        }
        SessionLog::SetCurrent(nullptr);
      }));
      List.last()->start();
    }
    for (QThread * T : List)
    {
      T->wait();
      delete T;
    }
  }
  QStringList ThreadLines = ReadLines(Threads);
  CHECK_EQ(ThreadLines.size(), 800);
  for (const QString & Line : ThreadLines)
  {
    CHECK_MSG(LineText(Line).startsWith(QStringLiteral("thread ")), Line);
  }
}

static void TestRotation(const QString & Dir)
{
  QString FileName = Dir + QStringLiteral("/rotate.log");
  SessionLogOptions O = Options(FileName);
  O.MaxSize = 1000;
  O.MaxCount = 2;
  {
    SessionLog Log(O);
    for (int Index = 0; Index < 100; Index++)
    {
      Log.Add(llMessage, QStringLiteral("line %1 ").arg(Index) + QString(40, QLatin1Char('x')));
    }
  }
  CHECK(QFileInfo::exists(FileName));
  CHECK(QFileInfo::exists(FileName + QStringLiteral(".1")));
  CHECK(QFileInfo::exists(FileName + QStringLiteral(".2")));
  CHECK(!QFileInfo::exists(FileName + QStringLiteral(".3")));
  CHECK(QFileInfo(FileName).size() < 1000);
  CHECK(QFileInfo(FileName + QStringLiteral(".1")).size() < 1000);
  // the newest lines are in the base file, the older ones in .1
  QStringList Last = ReadLines(FileName);
  CHECK(!Last.isEmpty() && LineText(Last.last()).startsWith(QStringLiteral("line 99 ")));
  QStringList Previous = ReadLines(FileName + QStringLiteral(".1"));
  CHECK(!Previous.isEmpty() && !Last.isEmpty() &&
    (LineText(Previous.last()).section(QLatin1Char(' '), 1, 1).toInt() + 1 == LineText(Last.first()).section(QLatin1Char(' '), 1, 1).toInt()));

  // Keep all (MaxCount 0)
  QString AllName = Dir + QStringLiteral("/rotate-all.log");
  O.FileName = AllName;
  O.MaxCount = 0;
  {
    SessionLog Log(O);
    for (int Index = 0; Index < 100; Index++)
    {
      Log.Add(llMessage, QString(60, QLatin1Char('y')));
    }
  }
  CHECK(QFileInfo::exists(AllName + QStringLiteral(".5")));

  // An appended file that is already too large is rotated when opened
  QString BigName = Dir + QStringLiteral("/big.log");
  {
    QFile F(BigName);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(QByteArray(5000, 'z'));
  }
  O.FileName = BigName;
  O.MaxCount = 1;
  {
    SessionLog Log(O);
    Log.Add(llMessage, QStringLiteral("fresh"));
  }
  CHECK_EQ(QFileInfo(BigName + QStringLiteral(".1")).size(), qint64(5000));
  CHECK_EQ(ReadLines(BigName).size(), 1);
}

//---------------------------------------------------------------------------
// XML log: validation against WinSCP's documented schema
// (https://winscp.net/eng/docs/logging_xml)

static const QSet<QString> & ActionNames()
{
  static const QSet<QString> Result = {
    QStringLiteral("upload"), QStringLiteral("download"), QStringLiteral("touch"), QStringLiteral("chmod"),
    QStringLiteral("mkdir"), QStringLiteral("rm"), QStringLiteral("mv"), QStringLiteral("cp"), QStringLiteral("call"),
    QStringLiteral("ls"), QStringLiteral("stat"), QStringLiteral("checksum"), QStringLiteral("cwd"), QStringLiteral("difference")};
  return Result;
}

static const QSet<QString> & ParameterNames()
{
  static const QSet<QString> Result = {
    QStringLiteral("filename"), QStringLiteral("destination"), QStringLiteral("size"), QStringLiteral("permissions"),
    QStringLiteral("modification"), QStringLiteral("command"), QStringLiteral("output"), QStringLiteral("erroroutput"),
    QStringLiteral("exitcode"), QStringLiteral("algorithm"), QStringLiteral("checksum"), QStringLiteral("cwd"),
    QStringLiteral("action"), QStringLiteral("type")};
  return Result;
}

struct XmlAction
{
  QString Name;
  bool Recursive = false;
  QList<QPair<QString, QString>> Parameters;
  bool Success = false;
  QStringList Messages;
  int Files = 0;

  QString Param(const QString & AName) const
  {
    for (const auto & P : Parameters)
    {
      if (P.first == AName)
      {
        return P.second;
      }
    }
    return QString();
  }
};

struct XmlLog
{
  bool Valid = false;
  QString Error;
  QString SessionName;
  QList<XmlAction> Actions;
  QStringList Groups;
  QList<QStringList> Failures;
};

static const QRegularExpression TimestampFormat(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$"));

static bool ParseFile(QXmlStreamReader & Xml, QString & Error)
{
  // <file><filename/>?<type/><size/>?<modification/>?<permissions/>?<owner/>?<group/>?</file>
  while (Xml.readNextStartElement())
  {
    QString Name = Xml.name().toString();
    static const QSet<QString> Allowed = {QStringLiteral("filename"), QStringLiteral("type"), QStringLiteral("size"),
      QStringLiteral("modification"), QStringLiteral("permissions"), QStringLiteral("owner"), QStringLiteral("group")};
    if (!Allowed.contains(Name) || !Xml.attributes().hasAttribute(QStringLiteral("value")))
    {
      Error = QStringLiteral("bad file element %1").arg(Name);
      return false;
    }
    Xml.skipCurrentElement();
  }
  return true;
}

static bool ParseAction(QXmlStreamReader & Xml, XmlAction & Action, QString & Error)
{
  Action.Name = Xml.name().toString();
  QString Recursive = Xml.attributes().value(QStringLiteral("recursive")).toString();
  Action.Recursive = (Recursive == QStringLiteral("true"));
  bool HaveResult = false;
  while (Xml.readNextStartElement())
  {
    QString Name = Xml.name().toString();
    if (Name == QStringLiteral("result"))
    {
      QString Success = Xml.attributes().value(QStringLiteral("success")).toString();
      if ((Success != QStringLiteral("true")) && (Success != QStringLiteral("false")))
      {
        Error = QStringLiteral("bad success attribute");
        return false;
      }
      Action.Success = (Success == QStringLiteral("true"));
      while (Xml.readNextStartElement())
      {
        if (Xml.name() != QStringLiteral("message"))
        {
          Error = QStringLiteral("unexpected %1 in result").arg(Xml.name().toString());
          return false;
        }
        Action.Messages.append(Xml.readElementText());
      }
      HaveResult = true;
    }
    else if (HaveResult)
    {
      Error = QStringLiteral("%1 after result").arg(Name);
      return false;
    }
    else if (Name == QStringLiteral("files"))
    {
      while (Xml.readNextStartElement())
      {
        if ((Xml.name() != QStringLiteral("file")) || !ParseFile(Xml, Error))
        {
          if (Error.isEmpty()) Error = QStringLiteral("bad files");
          return false;
        }
        Action.Files++;
      }
    }
    else if (Name == QStringLiteral("file"))
    {
      if (!ParseFile(Xml, Error))
      {
        return false;
      }
      Action.Files++;
    }
    else if (ParameterNames().contains(Name))
    {
      Action.Parameters.append(qMakePair(Name, Xml.attributes().value(QStringLiteral("value")).toString()));
      Xml.skipCurrentElement();
    }
    else
    {
      Error = QStringLiteral("unknown element %1 in %2").arg(Name, Action.Name);
      return false;
    }
  }
  if (!HaveResult)
  {
    Error = QStringLiteral("%1 without result").arg(Action.Name);
    return false;
  }
  return true;
}

static bool ParseItems(QXmlStreamReader & Xml, XmlLog & Log, bool InGroup)
{
  while (Xml.readNextStartElement())
  {
    QString Name = Xml.name().toString();
    if (ActionNames().contains(Name))
    {
      XmlAction Action;
      if (!ParseAction(Xml, Action, Log.Error))
      {
        return false;
      }
      Log.Actions.append(Action);
    }
    else if (Name == QStringLiteral("failure"))
    {
      QStringList Messages;
      while (Xml.readNextStartElement())
      {
        if (Xml.name() != QStringLiteral("message"))
        {
          Log.Error = QStringLiteral("bad failure");
          return false;
        }
        Messages.append(Xml.readElementText());
      }
      Log.Failures.append(Messages);
    }
    else if ((Name == QStringLiteral("group")) && !InGroup)
    {
      if (!TimestampFormat.match(Xml.attributes().value(QStringLiteral("start")).toString()).hasMatch())
      {
        Log.Error = QStringLiteral("bad group start");
        return false;
      }
      Log.Groups.append(Xml.attributes().value(QStringLiteral("name")).toString());
      if (!ParseItems(Xml, Log, true))
      {
        return false;
      }
    }
    else
    {
      Log.Error = QStringLiteral("unexpected element %1").arg(Name);
      return false;
    }
  }
  return true;
}

static XmlLog ParseXmlLog(const QString & FileName)
{
  XmlLog Log;
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    Log.Error = QStringLiteral("cannot open");
    return Log;
  }
  QByteArray Data = F.readAll();
  if (!Data.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<session "))
  {
    Log.Error = QStringLiteral("bad header");
    return Log;
  }
  QXmlStreamReader Xml(Data);
  if (!Xml.readNextStartElement() || (Xml.name() != QStringLiteral("session")) ||
      (Xml.namespaceUri() != QStringLiteral("http://winscp.net/schema/session/1.0")))
  {
    Log.Error = QStringLiteral("bad root");
    return Log;
  }
  Log.SessionName = Xml.attributes().value(QStringLiteral("name")).toString();
  if (!TimestampFormat.match(Xml.attributes().value(QStringLiteral("start")).toString()).hasMatch())
  {
    Log.Error = QStringLiteral("bad start");
    return Log;
  }
  if (!ParseItems(Xml, Log, false))
  {
    return Log;
  }
  Xml.readNext();
  while (!Xml.atEnd() && (Xml.tokenType() != QXmlStreamReader::EndDocument))
  {
    if (Xml.tokenType() == QXmlStreamReader::StartElement)
    {
      Log.Error = QStringLiteral("content after session");
      return Log;
    }
    Xml.readNext();
  }
  if (Xml.hasError())
  {
    Log.Error = Xml.errorString();
    return Log;
  }
  Log.Valid = true;
  return Log;
}

static void TestActionsLog(const QString & Dir)
{
  CHECK_EQ(ActionsLog::XmlAttributeEscape(QStringLiteral("a&b<c>\"d\"\ne\r\x01")), QStringLiteral("a&amp;b&lt;c&gt;&quot;d&quot;&#10;e&#x01;"));
  CHECK_EQ(ActionsLog::XmlEscape(QStringLiteral("a\nb\tc")), QStringLiteral("a\nb\tc"));
  CHECK(TimestampFormat.match(ActionsLog::StandardTimestamp()).hasMatch());

  SessionData Data;
  Data.HostName = QStringLiteral("h");
  Data.UserName = QStringLiteral("u&<\"");
  QString Pattern = Dir + QStringLiteral("/actions-!@.xml");
  QString FileName = Dir + QStringLiteral("/actions-h.xml");
  {
    auto Log = std::make_shared<ActionsLog>(Pattern, &Data);
    CHECK(Log->Open());
    CHECK_EQ(Log->CurrentFileName(), FileName);
    ActionsLog::SetCurrent(Log);
    {
      SessionAction Action(laUpload, QStringLiteral("/local/a.txt"), QStringLiteral("/remote/a.txt"));
      Action.Commit(true, 1234);
    }
    {
      // skipped by the user: not recorded
      SessionAction Action(laDownload, QStringLiteral("/remote/skipped"), QStringLiteral("/local/skipped"));
      Action.Commit(false, 1);
    }
    {
      // error, retried, succeeded
      SessionAction Action(laMkdir, QStringLiteral("/remote/dir"));
      SessionAction::NoteError(QStringLiteral("Temporary"));
      Action.Commit();
    }
    {
      // error, then skipped: failed with the error
      SessionAction Action(laRm, QStringLiteral("/remote/x & y"));
      SessionAction::NoteError(QStringLiteral("Permission denied."), QStringLiteral("Error code: 3\nError message from server: Permission denied"));
      Action.Commit(false);
    }
    {
      // outer action still open while the inner ones finish: recorded in start order
      SessionAction Outer(laRm, QStringLiteral("/remote/tree"));
      Outer.Recursive();
      {
        SessionAction Inner(laRm, QStringLiteral("/remote/tree/file"));
        Inner.Commit();
      }
      Outer.Commit();
    }
    try
    {
      SessionAction Action(laMv, QStringLiteral("/a"), QStringLiteral("/b"));
      throw std::runtime_error("abort");
    }
    catch (const std::exception &)
    {
    }
    {
      SessionAction Action(laChmod, QStringLiteral("/remote/a.txt"));
      Action.Rights(0644);
      Action.Rollback(ExtException(QStringLiteral("Cannot change properties"), QStringLiteral("No such file")));
    }
    {
      SessionAction Action(laCall);
      Action.Command(QStringLiteral("ls -l"));
      Action.Destination(QStringLiteral("/home"));
      Action.AddOutput(QStringLiteral("line1"), false);
      Action.AddOutput(QStringLiteral("line2"), false);
      Action.AddOutput(QStringLiteral("warning"), true);
      Action.ExitCode(0);
      Action.Commit();
    }
    {
      RemoteFile File;
      File.FileName = QStringLiteral("f.txt");
      File.Size = 10;
      File.Modification = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), QTimeZone::UTC);
      File.Permissions = 0644;
      File.Owner = QStringLiteral("joe");
      RemoteFile Sub;
      Sub.FileName = QStringLiteral("sub");
      Sub.IsDirectory = true;
      Sub.FileType = RemoteFile::Directory;
      Sub.Permissions = 0755;
      SessionAction Ls(laLs);
      Ls.Destination(QStringLiteral("/remote"));
      Ls.FileList({File, Sub});
      Ls.Commit();
      SessionAction Stat(laStat, QStringLiteral("/remote/f.txt"));
      Stat.File(File);
      Stat.Commit();
      SessionAction Sum(laChecksum, QStringLiteral("/remote/f.txt"));
      Sum.Checksum(QStringLiteral("sha-256"), QStringLiteral("abcd"));
      Sum.Commit();
      SessionAction Diff(laDifference);
      Diff.Difference(QStringLiteral("uploadupdate"), false, QStringLiteral("/l/f.txt"), 10, File.Modification,
        QStringLiteral("/remote/f.txt"), 9, File.Modification);
      Diff.Commit();
    }
    Log->BeginGroup(QStringLiteral("put \"a b\""));
    {
      SessionAction Action(laUpload, QStringLiteral("/local/g"), QStringLiteral("/remote/g"));
      Action.Commit(true, 1);
    }
    Log->AddFailure(QStringList{QStringLiteral("Something failed"), QStringLiteral("Detail")});
    Log->EndGroup();
    {
      // no log on the thread: nothing recorded, no crash
      ActionsLog::SetCurrent(nullptr);
      SessionAction Action(laUpload, QStringLiteral("/x"), QStringLiteral("/y"));
      CHECK(!Action.Active());
      Action.Commit();
      ActionsLog::SetCurrent(Log);
    }
    // a group still open is closed with the session
    Log->BeginGroup(QStringLiteral("exit"));
    ActionsLog::SetCurrent(nullptr);
  }
  XmlLog Log = ParseXmlLog(FileName);
  CHECK_MSG(Log.Valid, Log.Error);
  CHECK_EQ(Log.SessionName, QStringLiteral("u&<\"@h"));
  QStringList Names;
  for (const XmlAction & A : Log.Actions)
  {
    Names.append(A.Name + (A.Success ? QStringLiteral("+") : QStringLiteral("-")));
  }
  CHECK_EQ(Names.join(QLatin1Char(' ')),
    QStringLiteral("upload+ mkdir+ rm- rm+ rm+ mv- chmod- call+ ls+ stat+ checksum+ difference+ upload+"));
  if (Log.Actions.size() == 13)
  {
    const XmlAction & Upload = Log.Actions[0];
    CHECK_EQ(Upload.Parameters.size(), 3);
    CHECK_EQ(Upload.Parameters.value(0).first, QStringLiteral("filename"));
    CHECK_EQ(Upload.Param(QStringLiteral("filename")), QStringLiteral("/local/a.txt"));
    CHECK_EQ(Upload.Param(QStringLiteral("destination")), QStringLiteral("/remote/a.txt"));
    CHECK_EQ(Upload.Param(QStringLiteral("size")), QStringLiteral("1234"));
    CHECK(Log.Actions[1].Messages.isEmpty());
    CHECK_EQ(Log.Actions[2].Param(QStringLiteral("filename")), QStringLiteral("/remote/x & y"));
    CHECK_EQ(Log.Actions[2].Messages, (QStringList{QStringLiteral("Permission denied."), QStringLiteral("Error code: 3"),
      QStringLiteral("Error message from server: Permission denied")}));
    CHECK(Log.Actions[3].Recursive);
    CHECK_EQ(Log.Actions[3].Param(QStringLiteral("filename")), QStringLiteral("/remote/tree"));
    CHECK_EQ(Log.Actions[4].Param(QStringLiteral("filename")), QStringLiteral("/remote/tree/file"));
    CHECK(Log.Actions[5].Messages.isEmpty());
    CHECK_EQ(Log.Actions[6].Param(QStringLiteral("permissions")), QStringLiteral("rw-r--r--"));
    CHECK_EQ(Log.Actions[6].Messages, (QStringList{QStringLiteral("Cannot change properties"), QStringLiteral("No such file")}));
    CHECK_EQ(Log.Actions[7].Param(QStringLiteral("output")), QStringLiteral("line1\nline2"));
    CHECK_EQ(Log.Actions[7].Param(QStringLiteral("erroroutput")), QStringLiteral("warning"));
    CHECK_EQ(Log.Actions[7].Param(QStringLiteral("exitcode")), QStringLiteral("0"));
    CHECK_EQ(Log.Actions[8].Files, 2);
    CHECK_EQ(Log.Actions[9].Files, 1);
    CHECK_EQ(Log.Actions[10].Param(QStringLiteral("checksum")), QStringLiteral("abcd"));
    CHECK_EQ(Log.Actions[11].Param(QStringLiteral("action")), QStringLiteral("uploadupdate"));
  }
  CHECK_EQ(Log.Groups, (QStringList{QStringLiteral("put \"a b\""), QStringLiteral("exit")}));
  CHECK_EQ(Log.Failures.size(), 1);
  QString Text = ReadText(FileName);
  CHECK(Text.contains(QStringLiteral("      <modification value=\"2026-01-02T03:04:05.000Z\" />")));
  CHECK(Text.contains(QStringLiteral("  <upload>\n    <filename value=\"/local/a.txt\" />\n    <destination value=\"/remote/a.txt\" />\n"
    "    <size value=\"1234\" />\n    <result success=\"true\" />\n  </upload>\n")));
  CHECK(Text.contains(QStringLiteral("  <group name=\"put &quot;a b&quot;\" start=\"")));
  CHECK(Text.contains(QStringLiteral("    <upload>\n      <filename value=\"/local/g\" />")));
  CHECK(Text.endsWith(QStringLiteral("  </group>\n</session>\n")));

  // Open error
  ActionsLog Bad(QStringLiteral("/proc/no/such/x.xml"), nullptr);
  QString Error;
  CHECK(!Bad.Open(&Error));
  CHECK(!Bad.Logging());
}

//---------------------------------------------------------------------------
// /log & co. are not saved (TConfiguration::TemporaryLogging)

static void TestTemporaryLogging(const QString & Dir)
{
  Configuration & Config = Configuration::Instance();
  QString Ini = Dir + QStringLiteral("/temporary.ini");
  Config.SetIniFileName(Ini);
  Config.Load();
  CHECK(!Config.Logging);
  SessionLog::BeginTemporaryLogging(Config);
  Config.Logging = true;
  Config.LogFileName = Dir + QStringLiteral("/temporary-!S.log");
  Config.LogProtocol = 2;
  SessionLog::EndTemporaryLogging(Config);
  Config.ConfirmDeleting = false;   // something else to save
  CHECK(Config.Save());
  QString Text = ReadText(Ini);
  CHECK(Text.contains(QStringLiteral("ConfirmDeleting=0")));
  CHECK(!Text.contains(QStringLiteral("temporary-")));
  CHECK(!Text.contains(QStringLiteral("LogProtocol=2")));
  // still in effect for this run
  CHECK(Config.Logging);
  CHECK_EQ(Config.LogProtocol, 2);
  // confirmed in Preferences: permanent
  SessionLog::ClearTemporaryLogging();
  CHECK(Config.Save());
  Text = ReadText(Ini);
  CHECK(Text.contains(QStringLiteral("temporary-!S.log")));
  CHECK(Text.contains(QStringLiteral("LogProtocol=2")));
  Config.Logging = false;
  Config.LogProtocol = 0;
  CHECK(Config.Save());
}

//---------------------------------------------------------------------------
// A real session

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  QList<unsigned int> QueryAnswers;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyAccept;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override
  {
    return false;
  }
  void DisplayBanner(const QString &, const QString &) override
  {
  }
  unsigned int QueryUser(const QString &, const QString &, unsigned int Answers) override
  {
    QMutexLocker Lock(&Mutex);
    unsigned int Answer = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaAbort) : QueryAnswers.takeFirst();
    return ((Answer & Answers) != 0) ? Answer : static_cast<unsigned int>(qaAbort);
  }
};

static OperationResult WaitFor(const std::function<void(Terminal::Callback)> & Start)
{
  OperationResult Result;
  QEventLoop Loop;
  bool Finished = false;
  Start([&](const OperationResult & R)
  {
    Result = R;
    Finished = true;
    Loop.quit();
  });
  QTimer::singleShot(120000, &Loop, &QEventLoop::quit);
  if (!Finished)
  {
    Loop.exec();
  }
  return Result;
}

static void TestRealSession(const QString & Dir, TFSProtocol Protocol)
{
  bool Sftp = (Protocol == fsSFTPonly);
  std::printf("real session: %s\n", Sftp ? "SFTP" : "SCP");
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QByteArray Key = qgetenv("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || Key.isEmpty())
  {
    std::printf("SKIP real session (NIXSCP_TEST_SSH_PORT/NIXSCP_TEST_SSH_KEY not set)\n");
    return;
  }
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(QStringLiteral("nul"));
  Config.Load();
  Config.Logging = true;
  Config.LogProtocol = 1;
  Config.LogFileName = Dir + QStringLiteral("/session-!S.log");
  Config.LogFileAppend = false;
  Config.LogActions = true;
  Config.ActionsLogFileName = Dir + QStringLiteral("/session-!@.xml");

  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = QString::fromLocal8Bit(Key);
  Data.Password = QStringLiteral("not-the-password");
  Data.FSProtocol = Protocol;
  Data.KnownHostsFile = Dir + QStringLiteral("/known_hosts");
  Data.Name = Sftp ? QStringLiteral("logtest") : QStringLiteral("logtest-scp");
  Data.TryAgent = false;

  QTemporaryDir RemoteDir(QDir::tempPath() + QStringLiteral("/nixscp-logtest-XXXXXX"));
  QString Remote = RemoteDir.path();
  QString LocalDir = Dir + QStringLiteral("/local");
  QDir().mkpath(LocalDir);
  QString LocalFile = LocalDir + QStringLiteral("/up.txt");
  {
    QFile F(LocalFile);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write("hello log\n");
  }

  TestUserInterface UI;
  {
    Terminal Term(Data, &UI);
    OperationResult R = WaitFor([&](Terminal::Callback Done) { Term.Open(Done); });
    CHECK_MSG(R.Ok, R.Message + QLatin1Char(' ') + R.MoreMessages);
    if (R.Ok)
    {
      CopyParam Param;
      R = WaitFor([&](Terminal::Callback Done) { Term.CopyToRemote({LocalFile}, Remote, Param, false, Done); });
      CHECK_MSG(R.Ok, R.Message);
      R = WaitFor([&](Terminal::Callback Done) { Term.CreateDirectory(Remote + QStringLiteral("/sub"), 0755, Done); });
      CHECK_MSG(R.Ok, R.Message);
      // mkdir of an existing directory fails; Skip
      UI.QueryAnswers = {qaSkip};
      R = WaitFor([&](Terminal::Callback Done) { Term.CreateDirectory(Remote + QStringLiteral("/sub"), 0755, Done); });
      R = WaitFor([&](Terminal::Callback Done) { Term.RenameFile(Remote + QStringLiteral("/up.txt"), Remote + QStringLiteral("/sub/moved.txt"), Done); });
      CHECK_MSG(R.Ok, R.Message);
      R = WaitFor([&](Terminal::Callback Done) { Term.ChangeFileProperties({Remote + QStringLiteral("/sub/moved.txt")}, 0600, false, Done); });
      CHECK_MSG(R.Ok, R.Message);
      QString DownDir = Dir + QStringLiteral("/down-") + Data.Name;
      QDir().mkpath(DownDir);
      R = WaitFor([&](Terminal::Callback Done) { Term.CopyToLocal({Remote + QStringLiteral("/sub/moved.txt")}, DownDir, Param, false, Done); });
      CHECK_MSG(R.Ok, R.Message);
      CHECK(QFileInfo::exists(DownDir + QStringLiteral("/moved.txt")));
      R = WaitFor([&](Terminal::Callback Done) { Term.DeleteFiles({Remote + QStringLiteral("/sub")}, Done); });
      CHECK_MSG(R.Ok, R.Message);
    }
  }
  Config.Logging = false;
  Config.LogActions = false;

  QString LogName = Dir + QStringLiteral("/session-%1.log").arg(Data.Name);
  QString Text = ReadText(LogName);
  CHECK(!Text.isEmpty());
  for (const QString & Line : ReadLines(LogName))
  {
    CHECK_MSG(LineFormat.match(Line).hasMatch(), Line);
  }
  CHECK(Text.contains(QStringLiteral("Session name: %1 (Site)").arg(Data.Name)));
  CHECK(Text.contains(QStringLiteral("Log level: Debug 1")));
  CHECK(Text.contains(QStringLiteral("Username: %1 (Password: Yes, Key file: Yes").arg(Data.UserName)));
  CHECK(!Text.contains(QStringLiteral("not-the-password")));
  CHECK(Text.contains(QStringLiteral("Connecting to %1 port %2").arg(Data.HostName).arg(Data.PortNumber)));
  CHECK(Text.contains(QStringLiteral("Host key fingerprint is:")));
  CHECK(Text.contains(QStringLiteral("Access granted")));
  if (Sftp)
  {
    CHECK(Text.contains(QStringLiteral("SFTP version 3 negotiated.")));
    CHECK(Text.contains(QStringLiteral("Type: SSH_FXP_MKDIR, Path: %1/sub").arg(Remote)));
    CHECK(Text.contains(QStringLiteral("Copying \"%1\" to remote directory started.").arg(LocalFile)));
    CHECK(Text.contains(QStringLiteral("Transfer done: '%1' => '%2/up.txt' [10]").arg(LocalFile, Remote)));
  }
  else
  {
    // shell commands and their output
    CHECK(Text.contains(QStringLiteral("Using SCP protocol.")));
    CHECK(Text.contains(QRegularExpression(QStringLiteral("\n> [-0-9: .]+ mkdir \"%1/sub\"").arg(QRegularExpression::escape(Remote)))));
    CHECK(Text.contains(QRegularExpression(QStringLiteral("\n< [-0-9: .]+ WinSCP: this is end-of-file:0"))));
    CHECK(Text.contains(QRegularExpression(QStringLiteral("\n! [-0-9: .]+ mkdir: "))));
  }
  CHECK(Text.contains(QStringLiteral("Asking user:")));
  CHECK(Text.contains(QStringLiteral("Answer: Skip")));
  // libssh's own logging is routed into the log
  CHECK(Text.contains(QStringLiteral("ssh_")));

  XmlLog Log = ParseXmlLog(Dir + QStringLiteral("/session-%1.xml").arg(Data.HostName));
  CHECK_MSG(Log.Valid, Log.Error);
  CHECK_EQ(Log.SessionName, Data.Name);
  QStringList Names;
  for (const XmlAction & A : Log.Actions)
  {
    Names.append(A.Name + (A.Success ? QStringLiteral("+") : QStringLiteral("-")));
  }
  CHECK_EQ(Names.join(QLatin1Char(' ')), QStringLiteral("upload+ mkdir+ mkdir- mv+ chmod+ download+ rm+ rm+"));
  if (Log.Actions.size() == 8)
  {
    CHECK_EQ(Log.Actions[0].Param(QStringLiteral("filename")), LocalFile);
    CHECK_EQ(Log.Actions[0].Param(QStringLiteral("destination")), Remote + QStringLiteral("/up.txt"));
    CHECK_EQ(Log.Actions[0].Param(QStringLiteral("size")), QStringLiteral("10"));
    CHECK(!Log.Actions[2].Messages.isEmpty());
    CHECK_EQ(Log.Actions[3].Param(QStringLiteral("destination")), Remote + QStringLiteral("/sub/moved.txt"));
    CHECK_EQ(Log.Actions[4].Param(QStringLiteral("permissions")), QStringLiteral("rw-------"));
    CHECK_EQ(Log.Actions[6].Param(QStringLiteral("filename")), Remote + QStringLiteral("/sub"));
    CHECK_EQ(Log.Actions[7].Param(QStringLiteral("filename")), Remote + QStringLiteral("/sub/moved.txt"));
  }
}

int main(int argc, char ** argv)
{
  QTemporaryDir Scratch;
  // keep the user's configuration out of reach
  qputenv("XDG_CONFIG_HOME", (Scratch.path() + QStringLiteral("/config")).toLocal8Bit());
  qputenv("XDG_STATE_HOME", (Scratch.path() + QStringLiteral("/state")).toLocal8Bit());
  QCoreApplication App(argc, argv);
  QCoreApplication::setApplicationVersion(QStringLiteral("9.9.9"));
  QString Dir = Scratch.path();

  TestExpandFileName();
  TestFormatAndLevels(Dir);
  TestSensitive(Dir);
  TestAppendOverwrite(Dir);
  TestRotation(Dir);
  TestActionsLog(Dir);
  TestTemporaryLogging(Dir);
  TestRealSession(Dir, fsSFTPonly);
  TestRealSession(Dir, fsSCPonly);

  if (qEnvironmentVariableIsSet("NIXSCP_TEST_KEEP"))
  {
    Scratch.setAutoRemove(false);
    std::printf("kept %s\n", qPrintable(Dir));
  }
  if (Failures > 0)
  {
    std::printf("%d failure(s)\n", Failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
