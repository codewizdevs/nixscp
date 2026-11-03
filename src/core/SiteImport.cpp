// Port of WinSCP site import (see SiteImport.h for the origin of each part)
#include "core/SiteImport.h"
#include "core/IniStorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>

static const QString AnonymousUserName = QStringLiteral("anonymous");
static const QString AnonymousPassword = QStringLiteral("anonymous@example.com");
// GUIConfiguration::PuttySession: session WinSCP itself creates in PuTTY
static const QString PuttyTemporarySession = QStringLiteral("WinSCP temporary session");

//---------------------------------------------------------------------------
// ImportSessionList

void ImportSessionList::Add(std::shared_ptr<SessionData> Data, bool ASelected)
{
  Sessions.append(Data);
  Selected.append(ASelected);
  KnownHostLines.append(QStringList());
}

std::shared_ptr<SessionData> ImportSessionList::FindByName(const QString & Name) const
{
  for (const auto & Data : Sessions)
  {
    if (Data->Name.compare(Name, Qt::CaseInsensitive) == 0)
    {
      return Data;
    }
  }
  return nullptr;
}

QList<std::shared_ptr<SessionData>> ImportSessionList::SelectedSessions() const
{
  QList<std::shared_ptr<SessionData>> Result;
  for (int Index = 0; Index < Sessions.size(); Index++)
  {
    if (Selected[Index])
    {
      Result.append(Sessions[Index]);
    }
  }
  return Result;
}

void ImportSessionList::SelectSessionsToImport(const StoredSessionList & Dest, bool SshOnly)
{
  for (int Index = 0; Index < Sessions.size(); Index++)
  {
    const SessionData & Data = *Sessions[Index];
    Selected[Index] =
      (!SshOnly || (Data.NormalizedPuttyProtocol() == QStringLiteral("ssh"))) &&
      (Dest.FindByName(Data.Name) == nullptr);
  }
}

// TStoredSessionList(true) with DefaultSettings of the stored sessions
static std::shared_ptr<SessionData> NewImportSession(const StoredSessionList & Stored)
{
  auto Data = std::make_shared<SessionData>(const_cast<StoredSessionList &>(Stored).DefaultSettings());
  Data->Name.clear();
  // only settings NixSCP understands (no unknown "Default Settings" keys)
  Data->DiscardUnknownSettings();
  return Data;
}

// TConfiguration::SelectSessionsToImportIfAny
static void SelectSessionsToImportIfAny(ImportSessionList & List, const StoredSessionList & Stored, const QString & NoSessionsError)
{
  if (List.Count() > 0)
  {
    List.SelectSessionsToImport(Stored, true);
  }
  else
  {
    List.Error = NoSessionsError;
  }
}

//---------------------------------------------------------------------------
// Locations

// The user's profile folder; on Windows %USERPROFILE%, as OpenSSH for Windows
static QString HomeFolder()
{
#ifdef Q_OS_WIN
  QString Profile = qEnvironmentVariable("USERPROFILE");
  if (!Profile.isEmpty())
  {
    return QDir::fromNativeSeparators(Profile);
  }
#endif
  return QDir::homePath();
}

QString SiteImport::OpensshFolder()
{
  // GetOpensshFolder: profile folder + ".ssh"
  return HomeFolder() + QStringLiteral("/.ssh");
}

QString SiteImport::OpensshConfigFile()
{
  return OpensshFolder() + QStringLiteral("/config");
}

QString SiteImport::KnownHostsFile()
{
  return OpensshFolder() + QStringLiteral("/known_hosts");
}

QString SiteImport::PuttyFolder()
{
  // Unix PuTTY: ~/.putty, unless $PUTTYDIR (unix/storage.c make_filename)
  QString PuttyDir = qEnvironmentVariable("PUTTYDIR");
  return PuttyDir.isEmpty() ? (HomeFolder() + QStringLiteral("/.putty")) : PuttyDir;
}

QString SiteImport::PuttySessionsFolder()
{
  return PuttyFolder() + QStringLiteral("/sessions");
}

QString SiteImport::PuttyHostKeysFile()
{
  return PuttyFolder() + QStringLiteral("/sshhostkeys");
}

QString SiteImport::FilezillaFolder()
{
  // FileZilla 3 on Linux: $XDG_CONFIG_HOME/filezilla, legacy ~/.filezilla
  QString ConfigHome = qEnvironmentVariable("XDG_CONFIG_HOME");
#ifdef Q_OS_WIN
  // Windows: %APPDATA%\FileZilla
  if (ConfigHome.isEmpty() && !qEnvironmentVariable("APPDATA").isEmpty())
  {
    return QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA")) + QStringLiteral("/FileZilla");
  }
#endif
  if (ConfigHome.isEmpty())
  {
    ConfigHome = HomeFolder() + QStringLiteral("/.config");
  }
  QString Result = ConfigHome + QStringLiteral("/filezilla");
  QString Legacy = HomeFolder() + QStringLiteral("/.filezilla");
  if (!QFileInfo::exists(Result + QStringLiteral("/sitemanager.xml")) && QFileInfo::exists(Legacy + QStringLiteral("/sitemanager.xml")))
  {
    Result = Legacy;
  }
  return Result;
}

static QStringList ReadLines(const QString & FileName, bool * Ok = nullptr)
{
  QStringList Result;
  QFile F(FileName);
  bool Opened = F.open(QIODevice::ReadOnly);
  if (Opened)
  {
    QByteArray Data = F.readAll();
    if (Data.startsWith("\xEF\xBB\xBF"))
    {
      Data.remove(0, 3);
    }
    Result = QString::fromUtf8(Data).split(QLatin1Char('\n'));
    for (QString & Line : Result)
    {
      if (Line.endsWith(QLatin1Char('\r')))
      {
        Line.chop(1);
      }
    }
  }
  if (Ok != nullptr)
  {
    *Ok = Opened;
  }
  return Result;
}

//---------------------------------------------------------------------------
// OpenSSH

static bool IsValidOpensshLine(const QString & Line)
{
  return !Line.isEmpty() && (Line[0] != QLatin1Char('#'));
}

bool SiteImport::ParseOpensshDirective(const QString & ALine, QString & Directive, QString & Value)
{
  QString Line = ALine.trimmed();
  bool Result = IsValidOpensshLine(Line);
  if (Result)
  {
    if (Line.startsWith(QLatin1Char('"')))
    {
      Line.remove(0, 1);
      qsizetype P = Line.indexOf(QLatin1Char('"'));
      Result = (P >= 0);
      if (Result)
      {
        Directive = Line.left(P);
        Line = Line.mid(P + 1).trimmed();
      }
    }
    else
    {
      static const QString Whitespaces = QStringLiteral(" \t");
      qsizetype P = 0;
      while ((P < Line.size()) && !Whitespaces.contains(Line[P]) && (Line[P] != QLatin1Char('=')))
      {
        P++;
      }
      Result = (P > 0) && (P < Line.size());
      if (Result)
      {
        Directive = Line.left(P);
        Line.remove(0, P);
        bool EqualSeen = false;
        while (!Line.isEmpty() && (Whitespaces.contains(Line[0]) || (!EqualSeen && (Line[0] == QLatin1Char('=')))))
        {
          if (Line[0] == QLatin1Char('='))
          {
            EqualSeen = true;
          }
          Line.remove(0, 1);
        }
      }
    }
    Value = Line;
    Result = Result && !Value.isEmpty();
  }
  return Result;
}

QString SiteImport::CutOpensshToken(QString & S)
{
  const QChar NoQuote = QChar(0);
  QChar Quote = NoQuote;
  QString Result;
  qsizetype P = 0;
  while (P < S.size())
  {
    QChar C = S[P];
    if ((C == QLatin1Char('\\')) && (P + 1 < S.size()) &&
        ((S[P + 1] == QLatin1Char('\'')) || (S[P + 1] == QLatin1Char('"')) || (S[P + 1] == QLatin1Char('\\')) ||
         ((Quote == NoQuote) && (S[P + 1] == QLatin1Char(' ')))))
    {
      Result += S[P + 1];
      P++;
    }
    else if ((Quote == NoQuote) && ((C == QLatin1Char(' ')) || (C == QLatin1Char('\t'))))
    {
      break;
    }
    else if ((Quote == NoQuote) && ((C == QLatin1Char('\'')) || (C == QLatin1Char('"'))))
    {
      Quote = C;
    }
    else if ((Quote != NoQuote) && (Quote == C))
    {
      Quote = NoQuote;
    }
    else
    {
      Result += C;
    }
    P++;
  }
  S = S.mid(P).trimmed();
  return Result;
}

QString SiteImport::ConvertPathFromOpenssh(const QString & Path)
{
  QString Result = Path;
  if ((Result == QStringLiteral("~")) || Result.startsWith(QStringLiteral("~/")))
  {
    Result = HomeFolder() + Result.mid(1);
  }
  // ssh_config TOKENS: %d = local home directory, %% = literal %
  Result.replace(QStringLiteral("%d"), HomeFolder());
  Result.replace(QStringLiteral("%%"), QStringLiteral("%"));
  return Result;
}

static void InsertOpensshIncludes(QStringList & Lines, int Depth)
{
  // TConfiguration::SelectOpensshSessionsForImport: Include directives
  // are replaced with the contents of the included files
  for (int Index = 0; Index < Lines.size(); Index++)
  {
    QString Directive, Args;
    if (SiteImport::ParseOpensshDirective(Lines[Index], Directive, Args) &&
        (Directive.compare(QStringLiteral("Include"), Qt::CaseInsensitive) == 0))
    {
      QStringList Included;
      while (!Args.isEmpty())
      {
        QString IncludePath = SiteImport::ConvertPathFromOpenssh(SiteImport::CutOpensshToken(Args));
        // If path is not absolute, it is relative to .ssh/
        if (QFileInfo(IncludePath).isRelative())
        {
          IncludePath = SiteImport::OpensshFolder() + QLatin1Char('/') + IncludePath;
        }
        QFileInfo Info(IncludePath);
        QStringList Files;
        if (Info.fileName().contains(QLatin1Char('*')) || Info.fileName().contains(QLatin1Char('?')))
        {
          QDir Dir = Info.dir();
          for (const QString & Name : Dir.entryList({Info.fileName()}, QDir::Files, QDir::Name))
          {
            Files.append(Dir.filePath(Name));
          }
        }
        else if (Info.isFile())
        {
          Files.append(IncludePath);
        }
        for (const QString & File : Files)
        {
          if (Depth < 16)
          {
            QStringList FileLines = ReadLines(File);
            InsertOpensshIncludes(FileLines, Depth + 1);
            Included += FileLines;
          }
        }
      }
      Lines.removeAt(Index);
      for (int Index2 = 0; Index2 < Included.size(); Index2++)
      {
        Lines.insert(Index + Index2, Included[Index2]);
      }
      Index += static_cast<int>(Included.size()) - 1;
    }
  }
}

QStringList SiteImport::LoadOpensshConfig(const QString & FileName, QString * Error)
{
  bool Ok = false;
  QStringList Lines = ReadLines(FileName, &Ok);
  if (!Ok)
  {
    if (Error != nullptr)
    {
      // OPENSSH_CONFIG_NOT_FOUND
      *Error = QStringLiteral("OpenSSH config file not found.");
    }
    return QStringList();
  }
  InsertOpensshIncludes(Lines, 0);
  return Lines;
}

static bool OpensshBoolValue(const QString & Value)
{
  return (Value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0);
}

static bool MatchesOpensshPattern(const QString & Pattern, const QString & Name)
{
  // The WinSCP TFileMasks match does more, but host patterns only use * and ?
  QRegularExpression Re(QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion),
    QRegularExpression::CaseInsensitiveOption);
  return Re.match(Name).hasMatch();
}

// Nesting of ProxyJump hosts being resolved (ImportFromOpenssh recursion)
static thread_local int ProxyJumpDepth = 0;

void SiteImport::ImportFromOpenssh(SessionData & Data, const QStringList & Lines)
{
  bool SkippingSection = false;
  QSet<QString> UsedDirectives;
  for (const QString & Line : Lines)
  {
    QString Directive, Args;
    if (!ParseOpensshDirective(Line, Directive, Args))
    {
      continue;
    }
    if (Directive.compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0)
    {
      SkippingSection = true;
      while (!Args.isEmpty())
      {
        QString M = CutOpensshToken(Args);
        bool Negated = !M.isEmpty() && (M[0] == QLatin1Char('!'));
        if (Negated)
        {
          M.remove(0, 1);
        }
        if (MatchesOpensshPattern(M, Data.Name))
        {
          if (Negated)
          {
            // Skip even if matched by other positive patterns
            SkippingSection = true;
            break;
          }
          // Keep looking, in case if negated
          SkippingSection = false;
        }
      }
    }
    else if (Directive.compare(QStringLiteral("Match"), Qt::CaseInsensitive) == 0)
    {
      SkippingSection = true;
    }
    else if (!SkippingSection && !UsedDirectives.contains(Directive.toLower()))
    {
      QString Value = CutOpensshToken(Args);
      // All the directives we support allow only one token
      if (Args.isEmpty())
      {
        if (Directive.compare(QStringLiteral("AddressFamily"), Qt::CaseInsensitive) == 0)
        {
          if (Value.compare(QStringLiteral("inet"), Qt::CaseInsensitive) == 0)
          {
            Data.AddressFamily = afIPv4;
          }
          else if (Value.compare(QStringLiteral("inet6"), Qt::CaseInsensitive) == 0)
          {
            Data.AddressFamily = afIPv6;
          }
          else
          {
            Data.AddressFamily = afAuto;
          }
        }
        else if (Directive.compare(QStringLiteral("BindAddress"), Qt::CaseInsensitive) == 0)
        {
          Data.SourceAddress = Value;
        }
        else if (Directive.compare(QStringLiteral("Compression"), Qt::CaseInsensitive) == 0)
        {
          Data.Compression = OpensshBoolValue(Value);
        }
        else if (Directive.compare(QStringLiteral("ForwardAgent"), Qt::CaseInsensitive) == 0)
        {
          Data.AgentFwd = OpensshBoolValue(Value);
        }
        else if (Directive.compare(QStringLiteral("GSSAPIAuthentication"), Qt::CaseInsensitive) == 0)
        {
          Data.AuthGSSAPI = OpensshBoolValue(Value);
        }
        else if (Directive.compare(QStringLiteral("GSSAPIDelegateCredentials"), Qt::CaseInsensitive) == 0)
        {
          Data.AuthGSSAPIKEX = OpensshBoolValue(Value);
        }
        else if (Directive.compare(QStringLiteral("Hostname"), Qt::CaseInsensitive) == 0)
        {
          Data.HostName = Value;
        }
        else if (Directive.compare(QStringLiteral("IdentityFile"), Qt::CaseInsensitive) == 0)
        {
          Data.PublicKeyFile = ConvertPathFromOpenssh(Value);
        }
        else if (Directive.compare(QStringLiteral("CertificateFile"), Qt::CaseInsensitive) == 0)
        {
          Data.DetachedCertificate = ConvertPathFromOpenssh(Value);
        }
        else if (Directive.compare(QStringLiteral("KbdInteractiveAuthentication"), Qt::CaseInsensitive) == 0)
        {
          Data.AuthKI = OpensshBoolValue(Value);
        }
        else if (Directive.compare(QStringLiteral("Port"), Qt::CaseInsensitive) == 0)
        {
          bool Ok = false;
          int Port = Value.toInt(&Ok);
          if (Ok && (Port > 0) && (Port <= 65535))
          {
            Data.PortNumber = Port;
          }
        }
        else if (Directive.compare(QStringLiteral("User"), Qt::CaseInsensitive) == 0)
        {
          Data.UserName = Value;
        }
        else if (Directive.compare(QStringLiteral("ProxyJump"), Qt::CaseInsensitive) == 0)
        {
          // multiple jumps are not supported
          // (nesting limited: "Host *" with a ProxyJump, or two hosts jumping
          // through each other, would otherwise recurse without end)
          if (!Value.contains(QLatin1Char(',')) && (Value.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0) &&
              (ProxyJumpDepth < 8))
          {
            SessionData JumpData;
            JumpData.PortNumber = -1;
            int ParsedInfo = 0;
            JumpData.ParseUrl(Value, ParsedInfo);
            if (!JumpData.HostName.isEmpty())
            {
              // user and port given in ProxyJump take precedence over the
              // jump host's own Host block (as in OpenSSH)
              QString JumpUser = JumpData.UserName;
              int JumpPort = JumpData.PortNumber;
              if (JumpData.PortNumber < 0)
              {
                JumpData.PortNumber = SshPortNumber;
              }
              JumpData.Name = JumpData.HostName;
              ProxyJumpDepth++;
              ImportFromOpenssh(JumpData, Lines);
              ProxyJumpDepth--;
              if (!JumpUser.isEmpty())
              {
                JumpData.UserName = JumpUser;
              }
              if (JumpPort > 0)
              {
                JumpData.PortNumber = JumpPort;
              }
              Data.Tunnel = true;
              Data.TunnelHostName = JumpData.HostName;
              Data.TunnelPortNumber = JumpData.PortNumber;
              Data.TunnelUserName = JumpData.UserName;
              Data.TunnelPassword = JumpData.Password;
              Data.TunnelPublicKeyFile = JumpData.PublicKeyFile;
            }
          }
        }
        UsedDirectives.insert(Directive.toLower());
      }
    }
  }
}

ImportSessionList SiteImport::SelectOpensshSessionsForImport(const StoredSessionList & Stored, const QString & ConfigFile)
{
  ImportSessionList Result;
  QString Error;
  QStringList Lines = LoadOpensshConfig(ConfigFile, &Error);
  if (!Error.isEmpty())
  {
    Result.Error = QStringLiteral("%1\n(%2)").arg(Error, ConfigFile);
    return Result;
  }
  // TStoredSessionList::ImportFromOpenssh
  QSet<QString> Hosts;
  for (const QString & Line : Lines)
  {
    QString Directive, Value;
    if (ParseOpensshDirective(Line, Directive, Value) && (Directive.compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0))
    {
      while (!Value.isEmpty())
      {
        QString Name = CutOpensshToken(Value);
        if (!Name.isEmpty() && !Hosts.contains(Name.toLower()) &&
            !Name.contains(QLatin1Char('*')) && !Name.contains(QLatin1Char('?')) && !Name.startsWith(QLatin1Char('!')))
        {
          auto Data = NewImportSession(Stored);
          Data->Name = Name;
          Data->HostName = Name;
          ImportFromOpenssh(*Data, Lines);
          Data->Name = SessionData::MakeValidName(Name);
          Result.Add(Data);
          Hosts.insert(Name.toLower());
        }
      }
    }
  }
  // OPENSSH_CONFIG_NO_SITES
  SelectSessionsToImportIfAny(Result, Stored,
    QStringLiteral("%1\n(%2)").arg(QStringLiteral("No Host directives for specific hosts found in OpenSSH config file."), ConfigFile));
  return Result;
}

bool SiteImport::AnyOpensshSessionForImport(const StoredSessionList & Stored)
{
  return (SelectOpensshSessionsForImport(Stored).Count() > 0);
}

//---------------------------------------------------------------------------
// PuTTY (Unix storage: one file per session, "Key=Value" lines)

static QString PuttyUnmungeName(const QString & Name)
{
  // unix/storage.c: names are %-escaped except for [A-Za-z0-9+-.@_]
  QByteArray Out;
  QByteArray In = Name.toLatin1();
  for (int I = 0; I < In.size(); I++)
  {
    if ((In[I] == '%') && (I + 2 < In.size()))
    {
      Out += QByteArray::fromHex(In.mid(I + 1, 2));
      I += 2;
    }
    else
    {
      Out += In[I];
    }
  }
  return QString::fromUtf8(Out);
}

ImportSessionList SiteImport::SelectPuttySessionsForImport(const StoredSessionList & Stored, const QString & SessionsFolder)
{
  ImportSessionList Result;
  QDir Dir(SessionsFolder);
  for (const QString & FileName : Dir.entryList(QDir::Files, QDir::Name))
  {
    QString Name = PuttyUnmungeName(FileName);
    if ((Name == StoredSessionList::DefaultSettingsName()) || (Name == PuttyTemporarySession))
    {
      continue;
    }
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("Session"), true);
    for (const QString & Line : ReadLines(Dir.filePath(FileName)))
    {
      qsizetype P = Line.indexOf(QLatin1Char('='));
      if (P > 0)
      {
        QString Key = Line.left(P);
        QString Value = Line.mid(P + 1);
        if (Key == QStringLiteral("ProxyTelnetCommand"))
        {
          Storage.WriteStringRaw(Key, Value);
        }
        else
        {
          Storage.WriteString(Key, Value);
        }
      }
    }
    auto Data = NewImportSession(Stored);
    bool DefaultTcpNoDelay = Data->TcpNoDelay;
    // PuTTY proxy types above NixSCP's: 6 = SSH (tunnel), 7/8 = SSH exec/subsystem
    int PuttyProxyMethod = Storage.ReadInteger(QStringLiteral("ProxyMethod"), 0);
    if (PuttyProxyMethod > pmCmd)
    {
      Storage.DeleteValue(QStringLiteral("ProxyMethod"));
    }
    Data->Load(Storage);
    // PuTTY defaults to TcpNoDelay, but psftp/pscp ignore it
    Data->TcpNoDelay = DefaultTcpNoDelay;
    if ((PuttyProxyMethod == 6) && !Data->Tunnel)
    {
      Data->Tunnel = true;
      Data->TunnelPortNumber = Data->ProxyPort;
      Data->TunnelUserName = Data->ProxyUsername;
      Data->TunnelHostName = Data->ProxyHost;
      Data->TunnelPassword = Data->ProxyPassword;
      SessionData Defaults;
      Data->ProxyMethod = Defaults.ProxyMethod;
      Data->ProxyHost = Defaults.ProxyHost;
      Data->ProxyPort = Defaults.ProxyPort;
      Data->ProxyUsername = Defaults.ProxyUsername;
      Data->ProxyPassword = Defaults.ProxyPassword;
      Data->ProxyTelnetCommand = Defaults.ProxyTelnetCommand;
      Data->ProxyLocalCommand = Defaults.ProxyLocalCommand;
    }
    Data->Name = Name;
    bool Ssh = (Data->NormalizedPuttyProtocol() == QStringLiteral("ssh"));
    // PuTTY terminal settings are of no use to NixSCP
    Data->DiscardUnknownSettings();
    Result.Add(Data);
    Result.Selected.last() = Ssh && (Stored.FindByName(Name) == nullptr);
  }
  if (Result.Count() == 0)
  {
    // PUTTY_NO_SITES2 (sessions folder instead of registry key)
    Result.Error = QStringLiteral("No sites found in %1 sites folder (%2).").arg(QStringLiteral("PuTTY"), SessionsFolder);
  }
  return Result;
}

bool SiteImport::AnyPuttySessionForImport(const StoredSessionList & Stored)
{
  return (SelectPuttySessionsForImport(Stored).Count() > 0);
}

//---------------------------------------------------------------------------
// FileZilla

namespace
{
struct XmlNode
{
  QString Name;
  QMap<QString, QString> Attributes;
  QString Text;
  QStringList TextParts;
  QList<std::shared_ptr<XmlNode>> Children;

  const XmlNode * Find(const QString & AName) const
  {
    for (const auto & Child : Children)
    {
      if (Child->Name == AName)
      {
        return Child.get();
      }
    }
    return nullptr;
  }
};
}

static std::shared_ptr<XmlNode> ParseXmlFile(const QString & FileName, QString * Error)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot open file \"%1\".").arg(FileName);
    }
    return nullptr;
  }
  QXmlStreamReader Reader(&F);
  auto Root = std::make_shared<XmlNode>();
  QList<XmlNode *> Stack;
  Stack.append(Root.get());
  while (!Reader.atEnd())
  {
    Reader.readNext();
    if (Reader.isStartElement())
    {
      auto Node = std::make_shared<XmlNode>();
      Node->Name = Reader.name().toString();
      for (const QXmlStreamAttribute & Attribute : Reader.attributes())
      {
        Node->Attributes.insert(Attribute.name().toString(), Attribute.value().toString());
      }
      Stack.last()->Children.append(Node);
      Stack.append(Node.get());
    }
    else if (Reader.isEndElement())
    {
      if (Stack.size() > 1)
      {
        Stack.removeLast();
      }
    }
    else if (Reader.isCharacters())
    {
      QString Text = Reader.text().toString();
      Stack.last()->Text += Text;
      if (!Text.trimmed().isEmpty())
      {
        Stack.last()->TextParts.append(Text.trimmed());
      }
    }
  }
  if (Reader.hasError())
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("%1 (%2, line %3)").arg(Reader.errorString(), FileName).arg(Reader.lineNumber());
    }
    return nullptr;
  }
  return Root;
}

static QString ReadXmlNode(const XmlNode * Node, const QString & Name, const QString & Default)
{
  const XmlNode * TheNode = Node->Find(Name);
  QString Result = (TheNode != nullptr) ? TheNode->Text.trimmed() : QString();
  return Result.isEmpty() ? Default : Result;
}

static int ReadXmlNode(const XmlNode * Node, const QString & Name, int Default)
{
  const XmlNode * TheNode = Node->Find(Name);
  if (TheNode == nullptr)
  {
    return Default;
  }
  bool Ok = false;
  int Result = TheNode->Text.trimmed().toInt(&Ok);
  return Ok ? Result : Default;
}

static const XmlNode * FindSettingsNode(const XmlNode * Node, const QString & Name)
{
  for (const auto & Child : Node->Children)
  {
    if ((Child->Name == QStringLiteral("Setting")) && (Child->Attributes.value(QStringLiteral("name")) == Name))
    {
      return Child.get();
    }
  }
  return nullptr;
}

static QString ReadSettingsNode(const XmlNode * Node, const QString & Name, const QString & Default)
{
  const XmlNode * TheNode = FindSettingsNode(Node, Name);
  QString Result = (TheNode != nullptr) ? TheNode->Text.trimmed() : QString();
  return Result.isEmpty() ? Default : Result;
}

static int ReadSettingsNode(const XmlNode * Node, const QString & Name, int Default)
{
  const XmlNode * TheNode = FindSettingsNode(Node, Name);
  if (TheNode == nullptr)
  {
    return Default;
  }
  bool Ok = false;
  int Result = TheNode->Text.trimmed().toInt(&Ok);
  return Ok ? Result : Default;
}

// TSessionData::ImportFromFilezilla
static void ImportSessionFromFilezilla(SessionData & Data, const XmlNode * Node, const QString & Path, const XmlNode * SettingsNode)
{
  Data.Name = SessionData::ComposePath(Path, SessionData::MakeValidName(ReadXmlNode(Node, QStringLiteral("Name"), Data.Name)));
  Data.HostName = ReadXmlNode(Node, QStringLiteral("Host"), Data.HostName);
  Data.PortNumber = ReadXmlNode(Node, QStringLiteral("Port"), Data.PortNumber);

  // ServerProtocol enum
  switch (ReadXmlNode(Node, QStringLiteral("Protocol"), 0))
  {
    case 1: // SFTP
      Data.FSProtocol = fsSFTP;
      break;
    case 3: // FTPS
      Data.FSProtocol = fsFTP;
      Data.Ftps = ftpsImplicit;
      break;
    case 4: // FTPES
      Data.FSProtocol = fsFTP;
      Data.Ftps = ftpsExplicitTls;
      break;
    case 0: // FTP
    default: // UNKNOWN, HTTP, HTTPS, INSECURE_FTP
      Data.FSProtocol = fsFTP;
      break;
  }

  // LogonType enum
  if (ReadXmlNode(Node, QStringLiteral("Logontype"), 0) == 0) // ANONYMOUS
  {
    Data.UserName = AnonymousUserName;
    Data.Password = AnonymousPassword;
  }
  else
  {
    Data.UserName = ReadXmlNode(Node, QStringLiteral("User"), Data.UserName);
    Data.FtpAccount = ReadXmlNode(Node, QStringLiteral("Account"), Data.FtpAccount);
    if (const XmlNode * PassNode = Node->Find(QStringLiteral("Pass")))
    {
      QString APassword = PassNode->Text.trimmed();
      if (PassNode->Attributes.value(QStringLiteral("encoding")).compare(QStringLiteral("base64"), Qt::CaseInsensitive) == 0)
      {
        APassword = QString::fromUtf8(QByteArray::fromBase64(APassword.toLatin1()));
      }
      Data.Password = APassword;
    }
  }

  Data.PublicKeyFile = ReadXmlNode(Node, QStringLiteral("Keyfile"), Data.PublicKeyFile);

  int DefaultTimeDifference = static_cast<int>(Data.TimeDifference * 86400.0);
  Data.TimeDifference = ReadXmlNode(Node, QStringLiteral("TimezoneOffset"), DefaultTimeDifference) / 86400.0;
  Data.TimeDifferenceAuto = (Data.TimeDifference == 0);

  QString PasvMode = ReadXmlNode(Node, QStringLiteral("PasvMode"), QString());
  if (PasvMode.compare(QStringLiteral("MODE_PASSIVE"), Qt::CaseInsensitive) == 0)
  {
    Data.FtpPasvMode = true;
  }
  else if (PasvMode.compare(QStringLiteral("MODE_ACTIVE"), Qt::CaseInsensitive) == 0)
  {
    Data.FtpPasvMode = false;
  }
  else if (SettingsNode != nullptr)
  {
    Data.FtpPasvMode = (ReadSettingsNode(SettingsNode, QStringLiteral("Use Pasv mode"), 1) != 0);
  }

  QString EncodingType = ReadXmlNode(Node, QStringLiteral("EncodingType"), QString());
  if (EncodingType.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0)
  {
    Data.NotUtf = asAuto;
  }
  else if (EncodingType.compare(QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0)
  {
    Data.NotUtf = asOff;
  }

  Data.Note = ReadXmlNode(Node, QStringLiteral("Comments"), Data.Note);
  Data.LocalDirectory = ReadXmlNode(Node, QStringLiteral("LocalDir"), Data.LocalDirectory);

  // "<type> <prefix size> <size> <segment> <size> <segment>..."
  QString RemoteDir = ReadXmlNode(Node, QStringLiteral("RemoteDir"), QString());
  if (!RemoteDir.isEmpty())
  {
    auto CutToSpace = [](QString & S)
    {
      qsizetype P = S.indexOf(QLatin1Char(' '));
      QString Result = (P >= 0) ? S.left(P) : S;
      S = (P >= 0) ? S.mid(P + 1) : QString();
      return Result;
    };
    CutToSpace(RemoteDir); // type
    int PrefixSize = CutToSpace(RemoteDir).toInt(); // prefix size
    if (PrefixSize > 0)
    {
      RemoteDir.remove(0, PrefixSize);
    }
    QString Directory = QStringLiteral("/");
    while (!RemoteDir.isEmpty())
    {
      int SegmentSize = CutToSpace(RemoteDir).toInt();
      if (SegmentSize <= 0)
      {
        break;
      }
      QString Segment = RemoteDir.left(SegmentSize);
      Directory = Directory.endsWith(QLatin1Char('/')) ? (Directory + Segment) : (Directory + QLatin1Char('/') + Segment);
      RemoteDir.remove(0, SegmentSize + 1);
    }
    Data.RemoteDirectory = Directory;
  }

  Data.SynchronizeBrowsing = (ReadXmlNode(Node, QStringLiteral("SyncBrowsing"), Data.SynchronizeBrowsing ? 1 : 0) != 0);

  if (SettingsNode != nullptr)
  {
    if (Data.IsSsh() && Data.PublicKeyFile.isEmpty())
    {
      QString KeyFiles = ReadSettingsNode(SettingsNode, QStringLiteral("SFTP keyfiles"), QString());
      qsizetype P = KeyFiles.indexOf(QLatin1Char('\n'));
      QString KeyFile = ((P >= 0) ? KeyFiles.left(P) : KeyFiles).trimmed();
      KeyFiles = (P >= 0) ? KeyFiles.mid(P + 1).trimmed() : QString();
      // If there are more keys, ignore them, as we do not know which one to use
      if (!KeyFile.isEmpty() && KeyFiles.isEmpty())
      {
        Data.PublicKeyFile = KeyFile;
      }
    }

    bool BypassProxy = (ReadXmlNode(Node, QStringLiteral("BypassProxy"), 0) != 0);
    if (!BypassProxy)
    {
      int FtpProxyType = ReadSettingsNode(SettingsNode, QStringLiteral("FTP Proxy type"), -1);
      if (FtpProxyType > 0)
      {
        switch (FtpProxyType)
        {
          case 1: Data.FtpProxyLogonType = 2; break;
          case 2: Data.FtpProxyLogonType = 1; break;
          case 3: Data.FtpProxyLogonType = 3; break;
          default: Data.FtpProxyLogonType = 0; break; // custom
        }
        Data.ProxyHost = ReadSettingsNode(SettingsNode, QStringLiteral("FTP Proxy host"), Data.ProxyHost);
        Data.ProxyUsername = ReadSettingsNode(SettingsNode, QStringLiteral("FTP Proxy user"), Data.ProxyUsername);
        Data.ProxyPassword = ReadSettingsNode(SettingsNode, QStringLiteral("FTP Proxy password"), Data.ProxyPassword);
      }
      else
      {
        int ProxyType = ReadSettingsNode(SettingsNode, QStringLiteral("Proxy type"), -1);
        if (ProxyType >= 0)
        {
          switch (ProxyType)
          {
            case 1: Data.ProxyMethod = pmHTTP; break;
            case 2: Data.ProxyMethod = pmSocks5; break;
            case 3: Data.ProxyMethod = pmSocks4; break;
            default: Data.ProxyMethod = pmNone; break;
          }
          Data.ProxyHost = ReadSettingsNode(SettingsNode, QStringLiteral("Proxy host"), Data.ProxyHost);
          Data.ProxyPort = ReadSettingsNode(SettingsNode, QStringLiteral("Proxy port"), Data.ProxyPort);
          Data.ProxyUsername = ReadSettingsNode(SettingsNode, QStringLiteral("Proxy user"), Data.ProxyUsername);
          Data.ProxyPassword = ReadSettingsNode(SettingsNode, QStringLiteral("Proxy password"), Data.ProxyPassword);
        }
      }
    }
  }
}

// TStoredSessionList::ImportLevelFromFilezilla
static void ImportLevelFromFilezilla(ImportSessionList & List, const StoredSessionList & Stored,
  const XmlNode * Node, const QString & Path, const XmlNode * SettingsNode)
{
  for (const auto & Child : Node->Children)
  {
    if (Child->Name == QStringLiteral("Server"))
    {
      auto Data = NewImportSession(Stored);
      ImportSessionFromFilezilla(*Data, Child.get(), Path, SettingsNode);
      List.Add(Data);
    }
    else if (Child->Name == QStringLiteral("Folder"))
    {
      QString Name = SessionData::MakeValidName(Child->TextParts.join(QLatin1Char(' '))).trimmed();
      ImportLevelFromFilezilla(List, Stored, Child.get(), SessionData::ComposePath(Path, Name), SettingsNode);
    }
  }
}

ImportSessionList SiteImport::SelectFilezillaSessionsForImport(const StoredSessionList & Stored, const QString & Folder)
{
  ImportSessionList Result;
  QString SiteManagerFile = Folder + QStringLiteral("/sitemanager.xml");
  QString ConfigurationFile = Folder + QStringLiteral("/filezilla.xml");
  if (!QFileInfo::exists(SiteManagerFile))
  {
    // FILEZILLA_SITE_MANAGER_NOT_FOUND
    Result.Error = QStringLiteral("FileZilla site manager file not found (%1).").arg(SiteManagerFile);
    return Result;
  }
  std::shared_ptr<XmlNode> Configuration;
  const XmlNode * SettingsNode = nullptr;
  if (QFileInfo::exists(ConfigurationFile))
  {
    Configuration = ParseXmlFile(ConfigurationFile, nullptr);
    if (Configuration != nullptr)
    {
      if (const XmlNode * FileZilla3 = Configuration->Find(QStringLiteral("FileZilla3")))
      {
        SettingsNode = FileZilla3->Find(QStringLiteral("Settings"));
      }
    }
  }
  QString Error;
  std::shared_ptr<XmlNode> Document = ParseXmlFile(SiteManagerFile, &Error);
  if (Document == nullptr)
  {
    Result.Error = Error;
    return Result;
  }
  if (const XmlNode * FileZilla3 = Document->Find(QStringLiteral("FileZilla3")))
  {
    if (const XmlNode * Servers = FileZilla3->Find(QStringLiteral("Servers")))
    {
      ImportLevelFromFilezilla(Result, Stored, Servers, QString(), SettingsNode);
    }
  }
  // FILEZILLA_NO_SITES
  SelectSessionsToImportIfAny(Result, Stored, QStringLiteral("No sites found in FileZilla site manager file (%1).").arg(SiteManagerFile));
  return Result;
}

bool SiteImport::AnyFilezillaSessionForImport(const StoredSessionList & Stored)
{
  return (SelectFilezillaSessionsForImport(Stored).Count() > 0);
}

//---------------------------------------------------------------------------
// INI file (WinSCP.ini or a .reg export of WinSCP's registry settings)

ImportSessionList SiteImport::SelectSessionsForImport(const StoredSessionList & Stored, const QString & IniFileName)
{
  ImportSessionList Result;
  if (IniFileName.isEmpty())
  {
    // INI_SELECT
    Result.Error = QStringLiteral("Browse for INI file to import sites from.");
    return Result;
  }
  IniFile File;
  if (!File.Load(IniFileName))
  {
    Result.Error = QStringLiteral("Cannot open file \"%1\".").arg(IniFileName);
    return Result;
  }
  // TStoredSessionList::Load with the import list's DefaultSettings = Stored's
  StoredSessionList Imported;
  Imported.Load(File);
  IniStorage Storage(&File);
  bool HasOwnDefaults = Storage.OpenSubKey(QStringLiteral("Sessions"), false) && Storage.HasSubKey(StoredSessionList::DefaultSettingsName());
  for (const auto & Data : Imported.Sessions())
  {
    auto Session = std::make_shared<SessionData>(*Data);
    if (!HasOwnDefaults)
    {
      // sites of a file without "Default Settings" inherit ours
      IniFile SiteFile;
      IniStorage SiteStorage(&SiteFile);
      SiteStorage.OpenSubKey(QStringLiteral("Site"), true);
      Data->Save(SiteStorage, &Imported.DefaultSettings());
      *Session = const_cast<StoredSessionList &>(Stored).DefaultSettings();
      Session->Load(SiteStorage);
      Session->Name = Data->Name;
    }
    // agent access is granted on this computer only, never by an imported file
    Session->AgentAccess = 0;
    Result.Add(Session);
  }
  // INI_NO_SITES
  SelectSessionsToImportIfAny(Result, Stored, QStringLiteral("No sites found in \"%1\".").arg(IniFileName));
  return Result;
}

//---------------------------------------------------------------------------
// Host keys

static QByteArray SshUint32(quint32 Value)
{
  QByteArray Result(4, '\0');
  Result[0] = static_cast<char>((Value >> 24) & 0xFF);
  Result[1] = static_cast<char>((Value >> 16) & 0xFF);
  Result[2] = static_cast<char>((Value >> 8) & 0xFF);
  Result[3] = static_cast<char>(Value & 0xFF);
  return Result;
}

static QByteArray SshString(const QByteArray & S)
{
  return SshUint32(static_cast<quint32>(S.size())) + S;
}

static QByteArray SshMpint(QByteArray Magnitude)
{
  while (!Magnitude.isEmpty() && (Magnitude[0] == '\0'))
  {
    Magnitude.remove(0, 1);
  }
  if (!Magnitude.isEmpty() && (static_cast<unsigned char>(Magnitude[0]) & 0x80))
  {
    Magnitude.prepend('\0');
  }
  return SshString(Magnitude);
}

static bool HexToBytes(QString Hex, QByteArray & Bytes)
{
  Hex = Hex.trimmed();
  if (Hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
  {
    Hex.remove(0, 2);
  }
  if (Hex.isEmpty())
  {
    return false;
  }
  for (QChar C : Hex)
  {
    if (!C.isDigit() && ((C.toLower() < QLatin1Char('a')) || (C.toLower() > QLatin1Char('f'))))
    {
      return false;
    }
  }
  if (Hex.size() % 2 != 0)
  {
    Hex.prepend(QLatin1Char('0'));
  }
  Bytes = QByteArray::fromHex(Hex.toLatin1());
  return true;
}

static QByteArray PadLeft(const QByteArray & Bytes, int Size)
{
  QByteArray Result = Bytes;
  while (!Result.isEmpty() && (Result.size() > Size) && (Result[0] == '\0'))
  {
    Result.remove(0, 1);
  }
  if (Result.size() < Size)
  {
    Result.prepend(QByteArray(Size - Result.size(), '\0'));
  }
  return Result;
}

QString SiteImport::KnownHostName(const QString & HostName, int PortNumber)
{
  if (PortNumber == SshPortNumber)
  {
    return HostName;
  }
  return QStringLiteral("[%1]:%2").arg(HostName).arg(PortNumber);
}

QString SiteImport::PuttyHostKeyToKnownHostsLine(const QString & KeyName, const QString & Value, QString * Error)
{
  auto Fail = [Error](const QString & Message)
  {
    if (Error != nullptr)
    {
      *Error = Message;
    }
    return QString();
  };
  qsizetype At = KeyName.indexOf(QLatin1Char('@'));
  qsizetype Colon = (At >= 0) ? KeyName.indexOf(QLatin1Char(':'), At) : -1;
  if ((At <= 0) || (Colon < 0))
  {
    return Fail(QStringLiteral("Invalid host key name \"%1\".").arg(KeyName));
  }
  QString Type = KeyName.left(At);
  bool Ok = false;
  int Port = KeyName.mid(At + 1, Colon - At - 1).toInt(&Ok);
  QString Host = KeyName.mid(Colon + 1);
  if (!Ok || Host.isEmpty())
  {
    return Fail(QStringLiteral("Invalid host key name \"%1\".").arg(KeyName));
  }
  QStringList Parts = Value.trimmed().split(QLatin1Char(','));
  QByteArray Blob;
  QString SshId;
  QList<QByteArray> Numbers;
  auto ParseNumbers = [&](int From)
  {
    for (int Index = From; Index < Parts.size(); Index++)
    {
      QByteArray Bytes;
      if (!HexToBytes(Parts[Index], Bytes))
      {
        return false;
      }
      Numbers.append(Bytes);
    }
    return true;
  };
  if (Type == QStringLiteral("rsa2"))
  {
    SshId = QStringLiteral("ssh-rsa");
    if ((Parts.size() != 2) || !ParseNumbers(0))
    {
      return Fail(QStringLiteral("Invalid RSA host key."));
    }
    Blob = SshString(SshId.toLatin1()) + SshMpint(Numbers[0]) + SshMpint(Numbers[1]);
  }
  else if (Type == QStringLiteral("dss"))
  {
    SshId = QStringLiteral("ssh-dss");
    if ((Parts.size() != 4) || !ParseNumbers(0))
    {
      return Fail(QStringLiteral("Invalid DSA host key."));
    }
    Blob = SshString(SshId.toLatin1());
    for (const QByteArray & N : Numbers)
    {
      Blob += SshMpint(N);
    }
  }
  else if (Type.startsWith(QStringLiteral("ecdsa-sha2-")))
  {
    SshId = Type;
    QString Curve = Type.mid(11);
    int FieldBytes = (Curve == QStringLiteral("nistp256")) ? 32 : ((Curve == QStringLiteral("nistp384")) ? 48 : ((Curve == QStringLiteral("nistp521")) ? 66 : 0));
    // value: "<curve>,0x<x>,0x<y>"
    if ((FieldBytes == 0) || (Parts.size() != 3) || !ParseNumbers(1))
    {
      return Fail(QStringLiteral("Invalid ECDSA host key."));
    }
    QByteArray Point = QByteArray(1, '\x04') + PadLeft(Numbers[0], FieldBytes) + PadLeft(Numbers[1], FieldBytes);
    Blob = SshString(SshId.toLatin1()) + SshString(Curve.toLatin1()) + SshString(Point);
  }
  else if ((Type == QStringLiteral("ssh-ed25519")) || (Type == QStringLiteral("ssh-ed448")))
  {
    SshId = Type;
    int FieldBytes = (Type == QStringLiteral("ssh-ed25519")) ? 32 : 57;
    // value: "0x<x>,0x<y>"; encoded point: little endian y, top bit = x & 1
    if ((Parts.size() != 2) || !ParseNumbers(0))
    {
      return Fail(QStringLiteral("Invalid EdDSA host key."));
    }
    QByteArray Y = PadLeft(Numbers[1], FieldBytes);
    std::reverse(Y.begin(), Y.end());
    const QByteArray & X = Numbers[0];
    if (!X.isEmpty() && (X.back() & 1))
    {
      Y[FieldBytes - 1] = static_cast<char>(Y[FieldBytes - 1] | 0x80);
    }
    Blob = SshString(SshId.toLatin1()) + SshString(Y);
  }
  else
  {
    return Fail(QStringLiteral("Unsupported host key type \"%1\".").arg(Type));
  }
  return QStringLiteral("%1 %2 %3").arg(KnownHostName(Host, Port), SshId, QString::fromLatin1(Blob.toBase64()));
}

QMap<QString, QString> SiteImport::LoadPuttyHostKeys(const QString & FileName)
{
  // unix/storage.c: "<key name> <value>" lines
  QMap<QString, QString> Result;
  for (const QString & Line : ReadLines(FileName))
  {
    qsizetype P = Line.indexOf(QLatin1Char(' '));
    if (P > 0)
    {
      Result.insert(Line.left(P), Line.mid(P + 1).trimmed());
    }
  }
  return Result;
}

QMap<QString, QString> SiteImport::LoadIniHostKeys(const QString & IniFileName)
{
  QMap<QString, QString> Result;
  IniFile File;
  if (File.Load(IniFileName))
  {
    IniStorage Storage(&File);
    if (Storage.OpenSubKey(QStringLiteral("SshHostKeys"), false))
    {
      for (const QString & Name : Storage.GetValueNames())
      {
        Result.insert(Name, Storage.ReadStringRaw(Name, QString()));
      }
    }
  }
  return Result;
}

// Key of a known_hosts line for duplicate detection: key type + key data
struct KnownHostEntry
{
  QStringList Hosts;
  QString Key;
};

static bool ParseKnownHostsLine(const QString & ALine, KnownHostEntry & Entry)
{
  QString Line = ALine.trimmed();
  if (!IsValidOpensshLine(Line) || Line.startsWith(QLatin1Char('@')))
  {
    return false;
  }
  QStringList Fields = Line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  if (Fields.size() < 3)
  {
    return false;
  }
  Entry.Hosts = Fields[0].split(QLatin1Char(','), Qt::SkipEmptyParts);
  Entry.Key = Fields[1] + QLatin1Char(' ') + Fields[2];
  return true;
}

int SiteImport::AppendKnownHosts(const QString & TargetKnownHostsFile, const QStringList & Lines, QString * Error)
{
  QList<KnownHostEntry> Existing;
  QByteArray Contents;
  {
    QFile F(TargetKnownHostsFile);
    if (F.open(QIODevice::ReadOnly))
    {
      Contents = F.readAll();
    }
  }
  for (const QString & Line : QString::fromUtf8(Contents).split(QLatin1Char('\n')))
  {
    KnownHostEntry Entry;
    if (ParseKnownHostsLine(Line, Entry))
    {
      Existing.append(Entry);
    }
  }
  QStringList NewLines;
  for (const QString & Line : Lines)
  {
    KnownHostEntry Entry;
    if (!ParseKnownHostsLine(Line, Entry))
    {
      continue;
    }
    bool Found = false;
    for (const KnownHostEntry & E : Existing)
    {
      if (E.Key == Entry.Key)
      {
        bool AllHosts = true;
        for (const QString & Host : Entry.Hosts)
        {
          AllHosts = AllHosts && E.Hosts.contains(Host, Qt::CaseInsensitive);
        }
        if (AllHosts)
        {
          Found = true;
          break;
        }
      }
    }
    if (!Found)
    {
      NewLines.append(Line.trimmed());
      Existing.append(Entry);
    }
  }
  if (NewLines.isEmpty())
  {
    return 0;
  }
  QFileInfo Info(TargetKnownHostsFile);
  if (!Info.dir().exists())
  {
    QDir().mkpath(Info.absolutePath());
    QFile::setPermissions(Info.absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }
  bool New = !Info.exists();
  QFile F(TargetKnownHostsFile);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Append))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot write file \"%1\".").arg(TargetKnownHostsFile);
    }
    return 0;
  }
  if (New)
  {
    F.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  QByteArray Out;
  if (!Contents.isEmpty() && !Contents.endsWith('\n'))
  {
    Out += '\n';
  }
  Out += NewLines.join(QLatin1Char('\n')).toUtf8() + '\n';
  F.write(Out);
  return static_cast<int>(NewLines.size());
}

int SiteImport::ImportHostKeys(const QMap<QString, QString> & HostKeys, const QList<std::shared_ptr<SessionData>> & Sessions,
  const QString & TargetKnownHostsFile)
{
  QStringList Lines;
  for (const auto & Session : Sessions)
  {
    QString HostKeyName = QStringLiteral("@%1:%2").arg(Session->PortNumber).arg(Session->HostNameExpanded());
    for (auto It = HostKeys.cbegin(); It != HostKeys.cend(); ++It)
    {
      if (It.key().endsWith(HostKeyName, Qt::CaseInsensitive))
      {
        // WinSCP may keep several keys separated by ";"
        for (const QString & Value : It.value().split(QLatin1Char(';'), Qt::SkipEmptyParts))
        {
          QString Line = PuttyHostKeyToKnownHostsLine(It.key(), Value);
          if (!Line.isEmpty())
          {
            Lines.append(Line);
          }
        }
      }
    }
  }
  return AppendKnownHosts(TargetKnownHostsFile, Lines);
}

//---------------------------------------------------------------------------
// known_hosts

ImportSessionList SiteImport::SelectKnownHostsSessionsForImport(const StoredSessionList & Stored, const QStringList & Lines,
  const QString & TargetKnownHostsFile)
{
  // TStoredSessionList::ImportFromKnownHosts: one entry per host:port,
  // selected when it has a key not known yet
  ImportSessionList Result;
  QList<KnownHostEntry> Known;
  for (const QString & Line : ReadLines(TargetKnownHostsFile))
  {
    KnownHostEntry Entry;
    if (ParseKnownHostsLine(Line, Entry))
    {
      Known.append(Entry);
    }
  }
  for (const QString & ALine : Lines)
  {
    KnownHostEntry Entry;
    if (!ParseKnownHostsLine(ALine, Entry))
    {
      continue;
    }
    QString HostNameStr = Entry.Hosts.isEmpty() ? QString() : Entry.Hosts.first();
    // hashed host names cannot be turned back into host names
    if (HostNameStr.isEmpty() || HostNameStr.startsWith(QLatin1Char('|')))
    {
      continue;
    }
    int PortNumber = -1;
    if (HostNameStr.startsWith(QLatin1Char('[')))
    {
      qsizetype P = HostNameStr.indexOf(QStringLiteral("]:"));
      if (P > 0)
      {
        PortNumber = HostNameStr.mid(P + 2).toInt();
        HostNameStr = HostNameStr.mid(1, P - 1);
      }
      else
      {
        HostNameStr = HostNameStr.mid(1).remove(QLatin1Char(']'));
      }
    }
    QString NameStr = HostNameStr;
    if (PortNumber >= 0)
    {
      // FormatKnownHostName
      NameStr = QStringLiteral("%1:%2").arg(HostNameStr).arg(PortNumber);
    }
    int Index = -1;
    for (int I = 0; I < Result.Count(); I++)
    {
      if (Result.Sessions[I]->Name.compare(NameStr, Qt::CaseInsensitive) == 0)
      {
        Index = I;
        break;
      }
    }
    if (Index < 0)
    {
      auto Data = NewImportSession(Stored);
      Data->Name = NameStr;
      Data->HostName = HostNameStr;
      if (PortNumber >= 0)
      {
        Data->PortNumber = PortNumber;
      }
      Result.Add(Data, false);
      Index = Result.Count() - 1;
    }
    std::shared_ptr<SessionData> Data = Result.Sessions[Index];
    QString Algorithm = Entry.Key.section(QLatin1Char(' '), 0, 0);
    Data->HostKey = Data->HostKey.isEmpty() ? Algorithm : (Data->HostKey + QStringLiteral(", ") + Algorithm);
    Result.KnownHostLines[Index].append(ALine.trimmed());
    // If there's at least one unknown key for this host, select it
    bool IsKnown = false;
    for (const KnownHostEntry & E : Known)
    {
      if ((E.Key == Entry.Key) && E.Hosts.contains(Entry.Hosts.first(), Qt::CaseInsensitive))
      {
        IsKnown = true;
        break;
      }
    }
    if (!IsKnown)
    {
      Result.Selected[Index] = true;
    }
  }
  if (Result.Count() == 0)
  {
    // KNOWN_HOSTS_NO_SITES
    Result.Error = QStringLiteral("No host keys found in known_hosts.");
  }
  return Result;
}

ImportSessionList SiteImport::SelectKnownHostsSessionsForImportFromFile(const StoredSessionList & Stored, const QString & FileName,
  const QString & TargetKnownHostsFile)
{
  bool Ok = false;
  QStringList Lines = ReadLines(FileName, &Ok);
  if (!Ok)
  {
    ImportSessionList Result;
    // KNOWN_HOSTS_NOT_FOUND
    Result.Error = QStringLiteral("%1\n(%2)").arg(QStringLiteral("OpenSSH known_hosts file not found."), FileName);
    return Result;
  }
  ImportSessionList Result = SelectKnownHostsSessionsForImport(Stored, Lines, TargetKnownHostsFile);
  if (!Result.Error.isEmpty())
  {
    Result.Error = QStringLiteral("%1\n(%2)").arg(Result.Error, FileName);
  }
  return Result;
}

int SiteImport::ImportSelectedKnownHosts(const ImportSessionList & KnownHosts, const QString & TargetKnownHostsFile)
{
  QStringList Lines;
  for (int Index = 0; Index < KnownHosts.Count(); Index++)
  {
    if (KnownHosts.Selected[Index])
    {
      Lines += KnownHosts.KnownHostLines[Index];
    }
  }
  return AppendKnownHosts(TargetKnownHostsFile, Lines);
}

void SiteImport::SelectKnownHostsForSelectedSessions(ImportSessionList & KnownHosts, const ImportSessionList & Sessions)
{
  for (int Index = 0; Index < Sessions.Count(); Index++)
  {
    if (Sessions.Selected[Index])
    {
      const SessionData & Session = *Sessions.Sessions[Index];
      QString Key = Session.HostName;
      if (Session.PortNumber != Session.DefaultPort())
      {
        Key = QStringLiteral("%1:%2").arg(Key).arg(Session.PortNumber);
      }
      for (int KnownIndex = 0; KnownIndex < KnownHosts.Count(); KnownIndex++)
      {
        if (KnownHosts.Sessions[KnownIndex]->Name.compare(Key, Qt::CaseInsensitive) == 0)
        {
          KnownHosts.Selected[KnownIndex] = true;
        }
      }
    }
  }
}

//---------------------------------------------------------------------------
// Windows paths

bool SiteImport::IsWindowsPath(const QString & Path)
{
  static const QRegularExpression DriveRe(QStringLiteral("^[A-Za-z]:([\\\\/]|$)"));
  return
    DriveRe.match(Path).hasMatch() ||
    Path.startsWith(QStringLiteral("\\\\")) ||
    (Path.startsWith(QLatin1Char('%')) && Path.contains(QLatin1Char('\\')));
}

bool SiteImport::HasWindowsPaths(const SessionData & Data)
{
#ifdef Q_OS_WIN
  // Native paths there: nothing to remap
  Q_UNUSED(Data);
  return false;
#endif
  return
    IsWindowsPath(Data.PublicKeyFile) || IsWindowsPath(Data.DetachedCertificate) ||
    IsWindowsPath(Data.TunnelPublicKeyFile) || IsWindowsPath(Data.TlsCertificateFile) ||
    IsWindowsPath(Data.LocalDirectory) || IsWindowsPath(Data.OtherLocalDirectory);
}

QString SiteImport::RemapWindowsPath(const QString & Path, bool IsDirectory)
{
  if (!IsWindowsPath(Path))
  {
    return Path;
  }
  QString Result = SessionData::ExpandEnvironmentVariables(Path);
  Result.replace(QLatin1Char('\\'), QLatin1Char('/'));
  // C:/Users/<name>/..., C:/Documents and Settings/<name>/... = home folder
  static const QRegularExpression ProfileRe(QStringLiteral("^[A-Za-z]:/(Users|Documents and Settings)/[^/]+(/|$)"),
    QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch Match = ProfileRe.match(Result);
  if (Match.hasMatch())
  {
    Result = HomeFolder() + QLatin1Char('/') + Result.mid(Match.capturedLength());
  }
  if (Result.endsWith(QLatin1Char('/')) && (Result.size() > 1))
  {
    Result.chop(1);
  }
  QFileInfo Info(Result);
  if (!IsWindowsPath(Result) && Info.exists() && (Info.isDir() == IsDirectory))
  {
    return Result;
  }
  if (!IsDirectory)
  {
    // the key may have been copied to ~/.ssh (possibly converted from .ppk)
    QString Name = QFileInfo(Result).fileName();
    QStringList Candidates = {OpensshFolder() + QLatin1Char('/') + Name, HomeFolder() + QLatin1Char('/') + Name};
    if (Name.endsWith(QStringLiteral(".ppk"), Qt::CaseInsensitive))
    {
      Candidates.append(OpensshFolder() + QLatin1Char('/') + Name.left(Name.size() - 4));
    }
    for (const QString & Candidate : Candidates)
    {
      if (QFileInfo(Candidate).isFile())
      {
        return Candidate;
      }
    }
  }
  return QString();
}

bool SiteImport::RemapWindowsPaths(SessionData & Data)
{
  bool Changed = false;
  auto Remap = [&Changed](QString & Path, bool IsDirectory)
  {
    QString NewPath = RemapWindowsPath(Path, IsDirectory);
    if (NewPath != Path)
    {
      Path = NewPath;
      Changed = true;
    }
  };
  Remap(Data.PublicKeyFile, false);
  Remap(Data.DetachedCertificate, false);
  Remap(Data.TunnelPublicKeyFile, false);
  Remap(Data.TlsCertificateFile, false);
  Remap(Data.LocalDirectory, true);
  Remap(Data.OtherLocalDirectory, true);
  return Changed;
}
