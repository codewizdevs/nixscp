// Port of WinSCP source/core/SessionData.cpp (TSessionData::ParseUrl & co.)
// and source/core/FileMasks.cpp (MaskFileName)
#include "cli/SessionUrl.h"
#include "cli/Options.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"

#include <QFile>

static const QString PasswordMask = QStringLiteral("***");
static const QChar UrlParamSeparator = QLatin1Char(';');
static const QChar UrlParamValueSeparator = QLatin1Char('=');
static const QString UrlHostKeyParamName = QStringLiteral("fingerprint");
static const QString UrlRawSettingsParamNamePrefix = QStringLiteral("x-");

static bool IsHex(QChar C)
{
  return C.isDigit() || ((C.toLower() >= QLatin1Char('a')) && (C.toLower() <= QLatin1Char('f')));
}

QString DecodeUrlChars(QString S)
{
  qsizetype I = 0;
  while (I < S.size())
  {
    if (S[I] == QLatin1Char('+'))
    {
      S[I] = QLatin1Char(' ');
    }
    else if (S[I] == QLatin1Char('%'))
    {
      QByteArray Bytes;
      while ((I + 2 < S.size()) && (S[I] == QLatin1Char('%')) && IsHex(S[I + 1]) && IsHex(S[I + 2]))
      {
        Bytes += QByteArray::fromHex(S.mid(I + 1, 2).toLatin1());
        S.remove(I, 3);
      }
      if (!Bytes.isEmpty())
      {
        QString Chars = QString::fromUtf8(Bytes);
        S.insert(I, Chars);
        I += Chars.size() - 1;
      }
    }
    I++;
  }
  return S;
}

// CutToChar: Str is cut up to (excluding) Ch; Ch is removed
static QString CutToChar(QString & Str, QChar Ch, bool Trim)
{
  qsizetype P = Str.indexOf(Ch);
  QString Result;
  if (P >= 0)
  {
    Result = Str.left(P);
    Str.remove(0, P + 1);
  }
  else
  {
    Result = Str;
    Str.clear();
  }
  if (Trim)
  {
    Result = Result.trimmed();
    Str = Str.trimmed();
  }
  return Result;
}

static bool IsProtocolUrl(const QString & Url, const QString & Protocol, int & ProtocolLen)
{
  // DoIsProtocolUrl: "protocol:" (also WinSCP-specific "winscp-protocol:")
  QString Prefix = Protocol + QLatin1Char(':');
  if (Url.startsWith(Prefix, Qt::CaseInsensitive))
  {
    ProtocolLen = Prefix.size();
    return true;
  }
  QString WinSCPPrefix = QStringLiteral("winscp-") + Prefix;
  if (Url.startsWith(WinSCPPrefix, Qt::CaseInsensitive))
  {
    ProtocolLen = WinSCPPrefix.size();
    return true;
  }
  return false;
}

void ApplySessionRawSettings(SessionData & Data, SessionExtras & Extras, const QStringList & Settings)
{
  // TSessionData::ApplyRawSettings: TOptionsStorage over "Key=Value" lines;
  // SessionData::Load keeps the current values for keys that are not present.
  // Note that "HostKey" is the host key algorithm list (HostKeyList) and
  // "SshHostKey" the expected fingerprint(s), exactly as in WinSCP.ini.
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Sessions"), true);
  Storage.OpenSubKey(QStringLiteral("RawSettings"), true);
  for (const QString & Setting : Settings)
  {
    qsizetype P = Setting.indexOf(QLatin1Char('='));
    QString Key = (P >= 0) ? Setting.left(P) : Setting;
    QString Value = (P >= 0) ? Setting.mid(P + 1) : QString();
    // Settings outside of the WinSCP.ini site format
    if ((Key.compare(QStringLiteral("Passphrase"), Qt::CaseInsensitive) == 0) ||
        (Key.compare(QStringLiteral("PassphrasePlain"), Qt::CaseInsensitive) == 0))
    {
      Extras.Passphrase = Value;
    }
    else if (Key.compare(QStringLiteral("KnownHostsFile"), Qt::CaseInsensitive) == 0)
    {
      // NixSCP specific: known_hosts file to verify/store host keys with
      Data.KnownHostsFile = Value;
    }
    Storage.WriteString(Key, Value);
  }
  QString Name = Data.Name;
  QString KnownHosts = Data.KnownHostsFile;
  Data.Load(Storage);
  Data.Name = Name;
  Data.KnownHostsFile = KnownHosts;
}

static QString ReadPasswordFromFile(const QString & FileName)
{
  // ReadPasswordFromFile: first line of the file (LoadScriptFromFile)
  QString Result;
  if (!FileName.isEmpty())
  {
    QFile File(FileName);
    if (!File.open(QIODevice::ReadOnly))
    {
      throw ExtException(QStringLiteral("File '%1' not found.").arg(FileName));
    }
    QByteArray Data = File.readAll();
    if (Data.startsWith("\xEF\xBB\xBF"))
    {
      Data.remove(0, 3);
    }
    qsizetype P = Data.indexOf('\n');
    if (P >= 0)
    {
      Data.truncate(P);
    }
    if (Data.endsWith('\r'))
    {
      Data.chop(1);
    }
    Result = QString::fromUtf8(Data);
  }
  return Result;
}

void ReadPasswordsFromFiles(SessionData & Data, SessionExtras & Extras)
{
  // TSessionData::ReadPasswordsFromFiles
  Data.Password = ReadPasswordFromFile(Data.Password);
  Extras.NewPassword = ReadPasswordFromFile(Extras.NewPassword);
  Data.ProxyPassword = ReadPasswordFromFile(Data.ProxyPassword);
  Data.TunnelPassword = ReadPasswordFromFile(Data.TunnelPassword);
  Extras.Passphrase = ReadPasswordFromFile(Extras.Passphrase);
}

void ParseSessionUrl(const QString & AUrl, Options * Opts, const StoredSessionList * Stored,
  SessionData & Data, SessionExtras & Extras, int & ParsedInfo, QString * MaskedUrl)
{
  QString Url = AUrl;
  ParsedInfo = 0;
  bool ProtocolDefined = true;
  bool PortNumberDefined = false;
  TFSProtocol AFSProtocol = fsSFTP;
  int DefaultProtocolPortNumber = 0;
  TFtps AFtps = ftpsNone;
  int ProtocolLen = 0;
  if (IsProtocolUrl(Url, QStringLiteral("scp"), ProtocolLen))
  {
    AFSProtocol = fsSCPonly;
    DefaultProtocolPortNumber = SshPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("sftp"), ProtocolLen) ||
           // For most uses, handling ssh:// the same way as sftp://
           IsProtocolUrl(Url, QStringLiteral("ssh"), ProtocolLen))
  {
    AFSProtocol = fsSFTPonly;
    DefaultProtocolPortNumber = SshPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftp"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    DefaultProtocolPortNumber = FtpPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftps"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = FtpsImplicitPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftpes"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    AFtps = ftpsExplicitTls;
    DefaultProtocolPortNumber = FtpPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("dav"), ProtocolLen) ||
           IsProtocolUrl(Url, QStringLiteral("http"), ProtocolLen))
  {
    AFSProtocol = fsWebDAV;
    DefaultProtocolPortNumber = HTTPPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("davs"), ProtocolLen) ||
           IsProtocolUrl(Url, QStringLiteral("https"), ProtocolLen))
  {
    AFSProtocol = fsWebDAV;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = HTTPSPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("s3plain"), ProtocolLen))
  {
    AFSProtocol = fsS3;
    DefaultProtocolPortNumber = HTTPPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("s3"), ProtocolLen))
  {
    AFSProtocol = fsS3;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = HTTPSPortNumber;
  }
  else
  {
    ProtocolDefined = false;
  }

  QString Masked;
  if (ProtocolDefined)
  {
    ParsedInfo |= piProtocolDefined;
    Masked += Url.left(ProtocolLen);
    Url.remove(0, ProtocolLen);
    if (Url.startsWith(QStringLiteral("//")))
    {
      Masked += Url.left(2);
      Url.remove(0, 2);
    }
  }

  if (!Url.isEmpty())
  {
    QString DecodedUrl = DecodeUrlChars(Url);
    // lookup stored session even if protocol was defined
    std::shared_ptr<SessionData> Site;
    if ((Stored != nullptr) && !ProtocolDefined)
    {
      for (const auto & AData : Stored->Sessions())
      {
        if (AData->IsWorkspace)
        {
          continue;
        }
        const QString & Name = AData->Name;
        if ((Name.size() == DecodedUrl.size()) && (Name.compare(DecodedUrl, Qt::CaseInsensitive) == 0))
        {
          Site = AData;
        }
        else if ((Name.size() < DecodedUrl.size()) && (DecodedUrl[Name.size()] == QLatin1Char('/')) &&
                 DecodedUrl.startsWith(Name, Qt::CaseInsensitive))
        {
          Site = AData;
        }
        if (Site)
        {
          break;
        }
      }
    }

    QString ARemoteDirectory;
    if (Site)
    {
      Data = *Site;
      qsizetype P = 1;
      while ((P < Url.size()) && (DecodeUrlChars(Url.left(P)).compare(Site->Name, Qt::CaseInsensitive) != 0))
      {
        P++;
      }
      ARemoteDirectory = Url.mid(P);
      Masked += Url;
    }
    else
    {
      Data = (Stored != nullptr) ? const_cast<StoredSessionList *>(Stored)->DefaultSettings() : SessionData();
      Data.Name.clear();

      qsizetype PSlash = Url.indexOf(QLatin1Char('/'));
      if (PSlash < 0)
      {
        PSlash = Url.size();
      }
      QString ConnectInfo = Url.left(PSlash);
      qsizetype P = ConnectInfo.lastIndexOf(QLatin1Char('@'));
      QString UserInfo;
      QString HostInfo;
      if (P >= 0)
      {
        UserInfo = ConnectInfo.left(P);
        HostInfo = ConnectInfo.mid(P + 1);
      }
      else
      {
        HostInfo = ConnectInfo;
      }

      QString OrigHostInfo = HostInfo;
      if ((HostInfo.size() >= 2) && (HostInfo[0] == QLatin1Char('[')) && ((P = HostInfo.indexOf(QLatin1Char(']'))) > 0))
      {
        // IPv6 literal
        Data.HostName = HostInfo.mid(1, P - 1);
        HostInfo.remove(0, P + 1);
        if (HostInfo.startsWith(QLatin1Char(':')))
        {
          HostInfo.remove(0, 1);
        }
      }
      else
      {
        Data.HostName = DecodeUrlChars(CutToChar(HostInfo, QLatin1Char(':'), true));
      }

      if (!HostInfo.isEmpty())
      {
        bool Ok = false;
        int APortNumber = DecodeUrlChars(HostInfo).toInt(&Ok);
        if (Ok && (APortNumber > 0) && (APortNumber <= 65535))
        {
          Data.PortNumber = APortNumber;
          PortNumberDefined = true;
        }
      }
      else if (ProtocolDefined)
      {
        Data.PortNumber = DefaultProtocolPortNumber;
      }

      if (ProtocolDefined)
      {
        Data.Ftps = AFtps;
      }

      // user;fingerprint=...;x-name=value@host
      QString UserInfoWithoutConnectionParams = CutToChar(UserInfo, UrlParamSeparator, false);
      QString ConnectionParams = UserInfo;
      UserInfo = UserInfoWithoutConnectionParams;
      QStringList RawSettings;
      while (!ConnectionParams.isEmpty())
      {
        QString ConnectionParam = CutToChar(ConnectionParams, UrlParamSeparator, false);
        QString ConnectionParamName = CutToChar(ConnectionParam, UrlParamValueSeparator, false);
        if (ConnectionParamName.compare(UrlHostKeyParamName, Qt::CaseInsensitive) == 0)
        {
          Data.HostKey = DecodeUrlChars(ConnectionParam);
        }
        else if (ConnectionParamName.startsWith(UrlRawSettingsParamNamePrefix, Qt::CaseInsensitive))
        {
          QString AName = DecodeUrlChars(ConnectionParamName.mid(UrlRawSettingsParamNamePrefix.size()));
          RawSettings.append(AName + QLatin1Char('=') + DecodeUrlChars(ConnectionParam));
        }
      }
      if (!RawSettings.isEmpty())
      {
        ApplySessionRawSettings(Data, Extras, RawSettings);
      }

      bool HasPassword = UserInfo.contains(QLatin1Char(':'));
      QString RawUserName = CutToChar(UserInfo, QLatin1Char(':'), false);
      Data.UserName = DecodeUrlChars(RawUserName);
      Data.Password = DecodeUrlChars(UserInfo);
      // DenormalizeString: "user:@host" is an explicitly empty password
      Extras.EmptyPassword = HasPassword && Data.Password.isEmpty();

      QString RemoteDirectoryWithSessionParams = Url.mid(PSlash);
      ARemoteDirectory = CutToChar(RemoteDirectoryWithSessionParams, UrlParamSeparator, false);

      Masked += RawUserName;
      if (HasPassword)
      {
        Masked += QLatin1Char(':') + PasswordMask;
      }
      if (!RawUserName.isEmpty() || HasPassword)
      {
        Masked += QLatin1Char('@');
      }
      Masked += OrigHostInfo + ARemoteDirectory;
    }

    if (!ARemoteDirectory.isEmpty() && (ARemoteDirectory != QStringLiteral("/")))
    {
      Data.RemoteDirectory = DecodeUrlChars(ARemoteDirectory);
    }
  }
  else
  {
    Data = (Stored != nullptr) ? const_cast<StoredSessionList *>(Stored)->DefaultSettings() : SessionData();
    Data.Name.clear();
    ParsedInfo |= piDefaultsOnly;
  }

  if (ProtocolDefined)
  {
    Data.FSProtocol = AFSProtocol;
  }

  if (Opts != nullptr)
  {
    QString Value;
    if (Opts->FindSwitch(QStringLiteral("username"), Value))
    {
      Data.UserName = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("password"), Value))
    {
      Data.Password = Value;
      Extras.EmptyPassword = Value.isEmpty();
    }
    if (Opts->FindSwitch(QStringLiteral("sessionname"), Value))
    {
      Data.Name = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("newpassword"), Value))
    {
      Extras.NewPassword = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("privatekey"), Value))
    {
      Data.PublicKeyFile = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("clientcert"), Value))
    {
      Data.TlsCertificateFile = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("passphrase"), Value))
    {
      Extras.Passphrase = Value;
    }
    if (Opts->FindSwitch(QStringLiteral("timeout"), Value))
    {
      bool Ok = false;
      int Timeout = Value.toInt(&Ok);
      if (!Ok)
      {
        throw ExtException(QStringLiteral("'%1' is not a valid integer value.").arg(Value));
      }
      Data.Timeout = Timeout;
    }
    if (Opts->FindSwitch(QStringLiteral("hostkey"), Value) ||
        Opts->FindSwitch(QStringLiteral("certificate"), Value))
    {
      // SSH host key or TLS certificate fingerprint(s), ';'-separated
      Data.HostKey = Value;
    }
    QStringList FtpRawSettings;
    if (Opts->FindSwitch(QStringLiteral("passive")))
    {
      FtpRawSettings.append(QStringLiteral("FtpPasvMode=%1").arg(Opts->SwitchValue(QStringLiteral("passive"), true) ? 1 : 0));
    }
    if (Opts->FindSwitch(QStringLiteral("implicit")))
    {
      bool Enabled = Opts->SwitchValue(QStringLiteral("implicit"), true);
      Data.Ftps = Enabled ? ftpsImplicit : ftpsNone;
      if (!PortNumberDefined && Enabled)
      {
        Data.PortNumber = FtpsImplicitPortNumber;
      }
    }
    // BACKWARD COMPATIBILITY with 5.5.x
    if (Opts->FindSwitch(QStringLiteral("explicitssl")))
    {
      bool Enabled = Opts->SwitchValue(QStringLiteral("explicitssl"), true);
      Data.Ftps = Enabled ? ftpsExplicitSsl : ftpsNone;
      if (!PortNumberDefined && Enabled)
      {
        Data.PortNumber = FtpPortNumber;
      }
    }
    if (Opts->FindSwitch(QStringLiteral("explicit")) || Opts->FindSwitch(QStringLiteral("explicittls")))
    {
      QString SwitchName = Opts->FindSwitch(QStringLiteral("explicit")) ? QStringLiteral("explicit") : QStringLiteral("explicittls");
      bool Enabled = Opts->SwitchValue(SwitchName, true);
      Data.Ftps = Enabled ? ftpsExplicitTls : ftpsNone;
      if (!PortNumberDefined && Enabled)
      {
        Data.PortNumber = FtpPortNumber;
      }
    }
    if (!FtpRawSettings.isEmpty())
    {
      ApplySessionRawSettings(Data, Extras, FtpRawSettings);
    }
    QStringList RawSettings;
    if (Opts->FindSwitch(QStringLiteral("rawsettings"), RawSettings))
    {
      ApplySessionRawSettings(Data, Extras, RawSettings);
    }
    if (Opts->FindSwitch(QStringLiteral("passwordsfromfiles")))
    {
      ReadPasswordsFromFiles(Data, Extras);
    }
  }

  if (MaskedUrl != nullptr)
  {
    *MaskedUrl = Masked;
  }
}

QString NormalizeFingerprint(const QString & Fingerprint)
{
  // "ssh-ed25519 255 SHA256:xxxx", "ssh-ed25519 255 xxxx", "SHA256:xxxx", "xxxx",
  // "ssh-rsa 2048 aa:bb:..", "MD5:aa:bb:.."
  QString Result = Fingerprint.trimmed();
  qsizetype P = Result.lastIndexOf(QLatin1Char(' '));
  if (P >= 0)
  {
    Result = Result.mid(P + 1);
  }
  if (Result.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive))
  {
    Result = Result.mid(7);
  }
  else if (Result.startsWith(QStringLiteral("MD5:"), Qt::CaseInsensitive))
  {
    Result = Result.mid(4);
  }
  while (Result.endsWith(QLatin1Char('=')))
  {
    Result.chop(1);
  }
  // MD5 fingerprints may use '-' instead of ':' (as in WinSCP's NormalizeFingerprint)
  if ((Result.size() == 47) && (Result.count(QLatin1Char('-')) == 15))
  {
    Result.replace(QLatin1Char('-'), QLatin1Char(':'));
  }
  return Result;
}

bool VerifyFingerprint(const QString & Expected, const QString & FingerprintSHA256, const QString & FingerprintMD5)
{
  QString E = NormalizeFingerprint(Expected);
  if (E.isEmpty())
  {
    return false;
  }
  if (E == NormalizeFingerprint(FingerprintSHA256))
  {
    return true;
  }
  return !FingerprintMD5.isEmpty() && (E.compare(NormalizeFingerprint(FingerprintMD5), Qt::CaseInsensitive) == 0);
}
