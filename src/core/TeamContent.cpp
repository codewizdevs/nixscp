#include "core/TeamContent.h"
#include "core/CloudSettings.h"
#include "core/SessionData.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <limits>

namespace TeamContent
{

namespace
{

const QString MetaSection = QStringLiteral("NixSCP\\Team");
const QString NameKey = QStringLiteral("Name");
const QString SitesPrefix = QStringLiteral("Sessions\\");
const QString PasswordKey = QStringLiteral("PasswordPlain");
constexpr int MaxNameLength = 100;
constexpr int MaxText = 4096;
constexpr int MaxNote = 64 * 1024;

enum TKind { kHost, kUser, kSecret, kText, kNote, kBool, kInt, kProtocol, kHexDouble, kHostKey };

struct Rule
{
  const char * Key;
  TKind Kind;
  int Min = 0;
  int Max = 0;
};

// The allow-list: what makes a site reachable and how it is shown. Everything
// else stays on each computer: proxies, tunnels, port forwards, shell and SFTP
// server commands, post-login commands, recycle bin, key and certificate files,
// agents and agent forwarding, algorithm lists, SSH bug switches, the logical
// host name. A new setting is not in this table, so it is not shared.
const Rule Rules[] = {
  {"HostName", kHost},
  {"PortNumber", kInt, 0, 65535},
  {"UserName", kUser},
  {"PasswordPlain", kSecret},
  {"FSProtocol", kProtocol},
  {"Ftps", kInt, 0, 3},
  {"RemoteDirectory", kText},
  {"LocalDirectory", kText},
  {"OtherLocalDirectory", kText},
  {"Note", kNote},
  {"NoteOnly", kBool},
  {"NoteTeamEdit", kBool},
  {"NoteId", kText},
  {"NoteBy", kText},
  {"NoteAt", kText},
  {"Color", kInt, 0, 0xFFFFFF},
  {"SshHostKey", kHostKey},
  {"EOLType", kInt, 0, 2},
  {"Utf", kInt, 0, 2},
  {"ConsiderDST", kInt, 0, 2},
  {"TimeDifference", kHexDouble},
  {"TimeDifferenceAuto", kBool},
  {"TrimVMSVersions", kBool},
  {"VMSAllRevisions", kBool},
  {"SynchronizeBrowsing", kBool},
  {"UpdateDirectories", kBool},
  {"CacheDirectories", kBool},
  {"CacheDirectoryChanges", kBool},
  {"PreserveDirectoryChanges", kBool},
  {"ResolveSymlinks", kBool},
  {"FollowDirectorySymlinks", kBool},
  {"SFTPMaxVersion", kInt, -1, 6},
  {"SFTPMaxPacketSize", kInt, 0, 1024 * 1024 * 1024},
  {"SFTPDownloadQueue", kInt, 0, 1024},
  {"SFTPUploadQueue", kInt, 0, 1024},
  {"SFTPListingQueue", kInt, 0, 1024},
  {"SFTPRealPath", kInt, 0, 2},
  {"UsePosixRename", kBool},
  {"FtpPasvMode", kBool},
  {"FtpUseMlsd", kInt, 0, 2},
  {"FtpAccount", kText},
  {"FtpPingType", kInt, 0, 3},
  {"FtpPingInterval", kInt, 0, 86400},
  {"FtpTransferActiveImmediately2", kInt, 0, 2},
  {"FtpListAll", kInt, 0, 2},
  {"FtpHost", kInt, 0, 2},
  {"FtpWorkFromCwd", kInt, 0, 2},
  {"Timeout", kInt, 1, 86400},
  {"PingType", kInt, 0, 2},
  {"PingInterval", kInt, 0, 86400},
  {"PingIntervalSecs", kInt, 0, 86400},
  {"AddressFamily", kInt, 0, 2},
  {"TcpNoDelay", kBool},
  {"SendBuf", kInt, 0, 64 * 1024 * 1024},
  {"Compression", kBool},
  {"S3DefaultRegion", kText},
  {"S3UrlStyle", kInt, 0, 1},
  {"S3MaxKeys", kInt, 0, 2},
  {"S3RequesterPays", kBool},
  {"WebDavLiberalEscaping", kBool},
};

const Rule * FindRule(const QString & Key)
{
  static const QHash<QString, const Rule *> Table = []()
  {
    QHash<QString, const Rule *> Result;
    for (const Rule & Item : Rules)
    {
      Result.insert(QString::fromLatin1(Item.Key).toLower(), &Item);
    }
    return Result;
  }();
  return Table.value(Key.toLower(), nullptr);
}

bool HasControl(const QString & Text, bool AllowLines)
{
  for (QChar C : Text)
  {
    if ((C.unicode() < 0x20) || (C.unicode() == 0x7F))
    {
      if (!AllowLines || ((C != QLatin1Char('\n')) && (C != QLatin1Char('\r')) && (C != QLatin1Char('\t'))))
      {
        return true;
      }
    }
  }
  return false;
}

// Host and user names are expanded when connecting ($VAR, ${VAR}, %VAR%, ~): from
// a team that would send the member's environment to a server of the admin's
// choice. A leading '-' would become an option of a terminal program.
bool IsExpandable(const QString & Text)
{
  return Text.contains(QLatin1Char('$')) || Text.contains(QLatin1Char('%')) || Text.startsWith(QLatin1Char('~')) ||
    Text.startsWith(QLatin1Char('-'));
}

bool IsValidValue(const Rule & Item, const QString & Raw)
{
  switch (Item.Kind)
  {
    case kBool:
      return (Raw == QStringLiteral("0")) || (Raw == QStringLiteral("1"));
    case kInt:
    {
      bool Ok = false;
      int Value = Raw.toInt(&Ok);
      return Ok && (Value >= Item.Min) && (Value <= Item.Max);
    }
    case kProtocol:
      return QStringList({QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("5"), QStringLiteral("6"),
        QStringLiteral("7")}).contains(Raw);
    case kHexDouble:
    {
      static const QRegularExpression Pattern(QStringLiteral("^[0-9A-Fa-f]{16}$"));
      return Pattern.match(Raw).hasMatch();
    }
    default:
      break;
  }
  QString Value = UnMungeStr(Raw);
  switch (Item.Kind)
  {
    case kHost:
    {
      static const QRegularExpression Refused(QStringLiteral("[\\s@\\\\\"'`;|&<>/]"));
      return !Value.isEmpty() && (Value.size() <= 255) && !HasControl(Value, false) && !IsExpandable(Value) && !Refused.match(Value).hasMatch();
    }
    case kUser:
      return (Value.size() <= 255) && !HasControl(Value, false) && !IsExpandable(Value);
    case kSecret:
      return Value.size() <= MaxText;
    case kNote:
      return (Value.size() <= MaxNote) && !HasControl(Value, true);
    case kHostKey:
    {
      // Exact fingerprints may travel. Anything else could be a policy: "*" and
      // "acceptnew" turn the check off, whatever else SecureShell comes to accept
      static const QRegularExpression Fingerprint(QStringLiteral(
        "^(?:(?:ssh-[a-z0-9-]+|ecdsa-sha2-[a-z0-9-]+)(?: [0-9]+)? )?"
        "(?:SHA256:[A-Za-z0-9+/]{43}=?|(?:MD5:)?(?:[0-9a-fA-F]{2}:){15}[0-9a-fA-F]{2})$"));
      if (Value.isEmpty() || (Value.size() > MaxText))
      {
        return false;
      }
      for (const QString & Entry : Value.split(QLatin1Char(';')))
      {
        if (!Fingerprint.match(Entry.trimmed()).hasMatch())
        {
          return false;
        }
      }
      return true;
    }
    default:
      return (Value.size() <= MaxText) && !HasControl(Value, false);
  }
}

bool IsValidSiteName(const QString & Name)
{
  if (Name.isEmpty() || (Name.size() > 1024) || HasControl(Name, false) ||
      (Name.compare(StoredSessionList::DefaultSettingsName(), Qt::CaseInsensitive) == 0))
  {
    return false;
  }
  for (const QString & Part : Name.split(QLatin1Char('/')))
  {
    if (Part.isEmpty() || (Part == QStringLiteral(".")) || (Part == QStringLiteral("..")))
    {
      return false;
    }
  }
  return true;
}

// One site's settings as text, for telling changed sites apart
QByteArray SiteText(const IniFile & Sites, const QString & Section)
{
  QStringList Keys = Sites.Keys(Section);
  std::sort(Keys.begin(), Keys.end());
  QByteArray Result;
  for (const QString & Key : Keys)
  {
    Result += Key.toUtf8() + '=' + Sites.ReadRaw(Section, Key).toUtf8() + '\n';
  }
  return Result;
}

}

QString Fold(const QString & Text)
{
  return Text.toCaseFolded();
}

QString Skeleton(const QString & Text)
{
  static const QHash<char16_t, char16_t> Lookalikes = []()
  {
    QHash<char16_t, char16_t> Result;
    // Cyrillic and Greek letters (folded) that read as Latin, and digits that read as letters
    const char16_t Pairs[][2] = {
      {u'\u0430', u'a'}, {u'\u0432', u'b'}, {u'\u0435', u'e'}, {u'\u043A', u'k'}, {u'\u043C', u'm'}, {u'\u043D', u'h'}, {u'\u043E', u'o'},
      {u'\u0440', u'p'}, {u'\u0441', u'c'}, {u'\u0442', u't'}, {u'\u0443', u'y'}, {u'\u0445', u'x'}, {u'\u0456', u'l'}, {u'\u0458', u'j'},
      {u'\u0455', u's'}, {u'\u04BB', u'h'}, {u'\u0501', u'd'}, {u'\u051B', u'q'}, {u'\u051D', u'w'},
      {u'\u03B1', u'a'}, {u'\u03B2', u'b'}, {u'\u03B5', u'e'}, {u'\u03B9', u'l'}, {u'\u03BA', u'k'}, {u'\u03BD', u'v'}, {u'\u03BF', u'o'},
      {u'\u03C1', u'p'}, {u'\u03C4', u't'}, {u'\u03C5', u'u'}, {u'\u03C7', u'x'}, {u'\u03C9', u'w'},
      {u'0', u'o'}, {u'1', u'l'}, {u'i', u'l'},
    };
    for (const auto & Pair : Pairs)
    {
      Result.insert(Pair[0], Pair[1]);
    }
    return Result;
  }();
  // Folded first: what is the same for Fold stays the same here
  QString Decomposed = Fold(Fold(Text).normalized(QString::NormalizationForm_KD));
  QString Result;
  Result.reserve(Decomposed.size());
  for (QChar C : Decomposed)
  {
    QChar::Category Category = C.category();
    if ((Category == QChar::Mark_NonSpacing) || (Category == QChar::Mark_SpacingCombining) || (Category == QChar::Mark_Enclosing) ||
        (Category == QChar::Other_Format))
    {
      continue;
    }
    Result += QChar(Lookalikes.value(C.unicode(), C.unicode()));
  }
  return Result;
}

QString NormalizeName(const QString & Name)
{
  QString Result;
  for (QChar C : Name)
  {
    QChar::Category Category = C.category();
    if (C.isSpace())
    {
      Result += QLatin1Char(' ');
    }
    else if ((Category != QChar::Other_Control) && (Category != QChar::Other_Format) &&
             (C != QLatin1Char('<')) && (C != QLatin1Char('>')))
    {
      // no markup either: labels and message boxes show rich text when they see some
      Result += C;
    }
  }
  Result = Result.simplified();
  if (Result.size() > MaxNameLength)
  {
    Result.truncate(Result.at(MaxNameLength - 1).isHighSurrogate() ? MaxNameLength - 1 : MaxNameLength);
    Result = Result.trimmed();
  }
  return Result;
}

bool IsSharedKey(const QString & Key)
{
  return FindRule(Key) != nullptr;
}

QString SiteSection(const QString & Relative)
{
  return SitesPrefix + MungeStr(Relative, false);
}

QString SiteName(const QString & Section)
{
  return UnMungeStr(Section.mid(SitesPrefix.size()));
}

bool AddSite(IniFile & Sites, const QString & Relative, const SessionData & Data)
{
  static const SessionData FactoryDefaults;
  if (!IsValidSiteName(Relative))
  {
    return false;
  }
  const QString Section = QStringLiteral("Site");
  IniFile All;
  {
    IniStorage Storage(&All);
    Storage.OpenSubKey(Section, true);
    Data.Save(Storage, &FactoryDefaults);
    // Stored with a random padding; the decoded value is the same on every save
    if (!Data.Password.isEmpty())
    {
      Storage.WriteString(PasswordKey, Data.Password);
    }
  }
  const QString Target = SiteSection(Relative);
  Sites.AddSection(Target);
  for (const QString & Key : All.Keys(Section))
  {
    const Rule * Item = FindRule(Key);
    QString Raw = All.ReadRaw(Section, Key);
    if (Item == nullptr)
    {
      continue;
    }
    if (IsValidValue(*Item, Raw))
    {
      Sites.WriteRaw(Target, Key, Raw);
    }
    else if (Item->Kind == kHost)
    {
      Sites.EraseSection(Target);
      return false;
    }
  }
  return true;
}

QByteArray Encode(const QString & TeamName, const IniFile & Sites)
{
  IniFile Content = Sites;
  Content.WriteRaw(MetaSection, NameKey, MungeStr(TeamName, true));
  return CloudSettings::Canonical(Content);
}

bool Decode(const QByteArray & Content, QString & TeamName, IniFile & Sites, int * Dropped)
{
  IniFile File;
  int Refused = 0;
  Sites.Clear();
  if (!File.LoadFromData(Content) || !File.SectionExists(MetaSection))
  {
    return false;
  }
  TeamName = NormalizeName(UnMungeStr(File.ReadRaw(MetaSection, NameKey)));
  for (const QString & Section : File.Sections())
  {
    if (Section == MetaSection)
    {
      continue;
    }
    QString Name = SiteName(Section);
    if (!Section.startsWith(SitesPrefix) || Section.mid(SitesPrefix.size()).contains(QLatin1Char('\\')) || !IsValidSiteName(Name))
    {
      Refused++;
      continue;
    }
    QString Target = SiteSection(Name);
    bool Usable = true;
    IniFile Site;
    Site.AddSection(Target);
    for (const QString & Key : File.Keys(Section))
    {
      const Rule * Item = FindRule(Key);
      QString Raw = File.ReadRaw(Section, Key);
      if ((Item != nullptr) && IsValidValue(*Item, Raw))
      {
        Site.WriteRaw(Target, QString::fromLatin1(Item->Key), Raw);
      }
      else
      {
        Refused++;
        // a site whose address was refused is not the site the team meant
        Usable = Usable && !((Item != nullptr) && (Item->Kind == kHost));
      }
    }
    if (Usable)
    {
      Sites.Overlay(Site);
    }
  }
  if (Dropped != nullptr)
  {
    *Dropped = Refused;
  }
  return true;
}

QString Hash(const IniFile & Sites)
{
  return CloudSettings::Hash(CloudSettings::Canonical(Sites));
}

QHash<QString, QString> SiteHashes(const IniFile & Sites)
{
  QHash<QString, QString> Result;
  for (const QString & Section : Sites.Sections())
  {
    if (Section.startsWith(SitesPrefix))
    {
      Result.insert(SiteName(Section), CloudSettings::Hash(SiteText(Sites, Section)).left(32));
    }
  }
  return Result;
}

std::shared_ptr<SessionData> Merge(const IniFile & Sites, const QString & Section, const SessionData * Local, const QString & Name)
{
  const QString Work = QStringLiteral("Site");
  IniFile Site;
  IniStorage Storage(&Site);
  Storage.OpenSubKey(Work, true);
  if (Local != nullptr)
  {
    Local->Save(Storage, nullptr);
    for (const QString & Key : Site.Keys(Work))
    {
      // the stored form of the password is not in the allow-list under its own name
      if (IsSharedKey(Key) || (Key.compare(QStringLiteral("Password"), Qt::CaseInsensitive) == 0))
      {
        Site.DeleteKey(Work, Key);
      }
    }
  }
  for (const QString & Key : Sites.Keys(Section))
  {
    Site.WriteRaw(Work, Key, Sites.ReadRaw(Section, Key));
  }
  // Forced, on top of what Decode refuses: nothing in a team's site is ever expanded
  Site.WriteRaw(Work, QStringLiteral("NoExpansion"), QStringLiteral("1"));
  auto Result = std::make_shared<SessionData>();
  Result->Load(Storage);
  Result->Name = Name;
  return Result;
}

}
