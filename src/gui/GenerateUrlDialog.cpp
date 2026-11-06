// Port of WinSCP source/forms/GenerateUrl.{cpp,dfm} and the URL/code
// generation of core/SessionData.cpp (GenerateSessionUrl,
// GetRawSettingsForUrl, GenerateOpenCommandArgs)
#include "gui/GenerateUrlDialog.h"
#include "gui/HelpSystem.h"
#include "gui/LoginDialog.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QMap>
#include <QVBoxLayout>

#include <algorithm>

namespace
{

const QString WinSCPProtocolPrefix = QStringLiteral("winscp-");
const QChar UrlParamSeparator = QLatin1Char(';');
const QChar UrlParamValueSeparator = QLatin1Char('=');
const QString UrlHostKeyParamName = QStringLiteral("fingerprint");
const QString UrlSaveParamName = QStringLiteral("save");
const QString UrlRawSettingsParamNamePrefix = QStringLiteral("x-");
const QChar NormalizedFingerprintSeparator = QLatin1Char('-');

// GENERATE_URL_WRITABLE_PATH_TO_LOG / GENERATE_URL_PATH_TO_SCRIPT (Linux paths)
const QString WritablePathToLog = QStringLiteral("/writable/path/to/log/");
const QString PathToScript = QStringLiteral("/path/to/script/script.txt");

QString Base64ToUrlSafe(QString S)
{
  while (S.endsWith(QLatin1Char('=')))
  {
    S.chop(1);
  }
  S.replace(QLatin1Char('+'), QLatin1Char('-'));
  S.replace(QLatin1Char('/'), QLatin1Char('_'));
  return S;
}

QString MD5ToUrlSafe(QString S)
{
  return S.replace(QLatin1Char(':'), NormalizedFingerprintSeparator);
}

bool IsNumber(const QString & S)
{
  bool Ok = false;
  S.toLongLong(&Ok);
  return Ok;
}

// NormalizeFingerprint (DoNormalizeFingerprint): "ssh-ed25519 255 xxx" ->
// KeyName "ssh-ed25519", Fingerprint "xxx" (URL safe). Also accepts NixSCP's
// "ssh-ed25519 SHA256:xxx" (libssh notation, no key size).
void NormalizeFingerprint(QString & Fingerprint, QString & KeyName)
{
  QStringList Parts = Fingerprint.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if ((Parts.size() == 3) && IsNumber(Parts.at(1)))
  {
    KeyName = Parts.at(0);
    Fingerprint = Parts.at(2);
  }
  else if ((Parts.size() == 2) && !IsNumber(Parts.at(1)))
  {
    KeyName = Parts.at(0);
    Fingerprint = Parts.at(1);
  }
  else
  {
    return;
  }
  if (Fingerprint.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive))
  {
    Fingerprint = Fingerprint.mid(7);
  }
  else if (Fingerprint.startsWith(QStringLiteral("MD5:"), Qt::CaseInsensitive))
  {
    Fingerprint = Fingerprint.mid(4);
  }
  Fingerprint = MD5ToUrlSafe(Base64ToUrlSafe(Fingerprint));
}

// EscapeParam: doubles quotes (script parameter syntax)
QString EscapeParam(const QString & Param)
{
  QString Result = Param;
  return Result.replace(QLatin1Char('"'), QStringLiteral("\"\""));
}

QString AddQuotes(const QString & S)
{
  return QLatin1Char('"') + S + QLatin1Char('"');
}

// POSIX shell double-quoted string
QString ShellQuote(const QString & S)
{
  QString Result;
  for (QChar C : S)
  {
    if ((C == QLatin1Char('"')) || (C == QLatin1Char('\\')) || (C == QLatin1Char('$')) || (C == QLatin1Char('`')))
    {
      Result += QLatin1Char('\\');
    }
    Result += C;
  }
  return AddQuotes(Result);
}

// RtfSwitch(Name, Value): " -name="value""
QString Switch(const QString & Name, const QString & Value)
{
  return QStringLiteral(" -%1=%2").arg(Name, AddQuotes(EscapeParam(Value)));
}

QString Switch(const QString & Name, int Value)
{
  return QStringLiteral(" -%1=%2").arg(Name).arg(Value);
}

// StringsToParams
QString StringsToParams(const QStringList & Strings)
{
  QString Result;
  for (const QString & S : Strings)
  {
    int P = S.indexOf(QLatin1Char('='));
    QString Name = (P >= 0) ? S.left(P) : S;
    QString Value = (P >= 0) ? S.mid(P + 1) : QString();
    // Do not quote if it is all-numeric
    bool Ok = false;
    int Number = Value.toInt(&Ok);
    if (!Ok || (QString::number(Number) != Value))
    {
      Value = AddQuotes(EscapeParam(Value));
    }
    Result += QStringLiteral(" %1=%2").arg(Name, Value);
  }
  return Result;
}

// TSessionData::CopyNonCoreData from the factory defaults
void ResetNonCoreData(SessionData & Data, const SessionData & FactoryDefaults)
{
  Data.RemoteDirectory = FactoryDefaults.RemoteDirectory;
  Data.LocalDirectory = FactoryDefaults.LocalDirectory;
  Data.OtherLocalDirectory = FactoryDefaults.OtherLocalDirectory;
  Data.SynchronizeBrowsing = FactoryDefaults.SynchronizeBrowsing;
  Data.Color = FactoryDefaults.Color;
  Data.UpdateDirectories = FactoryDefaults.UpdateDirectories;
  Data.Note = FactoryDefaults.Note;
  Data.AgentAccess = FactoryDefaults.AgentAccess;
  Data.AgentPermissions = FactoryDefaults.AgentPermissions;
  Data.AgentName = FactoryDefaults.AgentName;
  Data.AgentAllow = FactoryDefaults.AgentAllow;
  Data.AgentDeny = FactoryDefaults.AgentDeny;
  Data.AgentRecord = FactoryDefaults.AgentRecord;
  Data.PortForwards = FactoryDefaults.PortForwards;
}

bool HttpForWebDAV()
{
  // TCustomWinConfiguration::HttpForWebDAV (Configuration\Interface)
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
  {
    return Storage.ReadBool(QStringLiteral("HttpForWebDAV"), false);
  }
  return false;
}

int ReadInterfaceInt(const QString & Name, int Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
  {
    return Storage.ReadInteger(Name, Default);
  }
  return Default;
}

void WriteInterfaceInt(const QString & Name, int Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
  {
    Storage.WriteInteger(Name, Value);
  }
}

} // namespace

//---------------------------------------------------------------------------
// Code generation
//---------------------------------------------------------------------------

static QString DoEncodeUrl(const QString & S, const QString & DoNotEncode)
{
  QString Result;
  for (QChar C : S)
  {
    if (((C.unicode() < 0x80) && C.isLetterOrNumber()) ||
        (C == QLatin1Char('_')) || (C == QLatin1Char('-')) || (C == QLatin1Char('.')) || (C == QLatin1Char('*')) ||
        DoNotEncode.contains(C))
    {
      Result += C;
    }
    else
    {
      for (char Byte : QString(C).toUtf8())
      {
        Result += QStringLiteral("%%1").arg(static_cast<unsigned char>(Byte), 2, 16, QLatin1Char('0')).toUpper();
      }
    }
  }
  return Result;
}

QString GenerateUrlDialog::EncodeUrlString(const QString & S)
{
  return DoEncodeUrl(S, QString());
}

QString GenerateUrlDialog::EncodeUrlPath(const QString & S)
{
  return DoEncodeUrl(S, QStringLiteral("/"));
}

QString GenerateUrlDialog::ProtocolUrl(const SessionData & Data, bool HttpForWebDAV)
{
  QString Url;
  switch (Data.FSProtocol)
  {
    case fsSCPonly:
      Url = QStringLiteral("scp");
      break;
    case fsFTP:
      if (Data.Ftps == ftpsImplicit)
      {
        Url = QStringLiteral("ftps");
      }
      else if ((Data.Ftps == ftpsExplicitTls) || (Data.Ftps == ftpsExplicitSsl))
      {
        Url = QStringLiteral("ftpes");
      }
      else
      {
        Url = QStringLiteral("ftp");
      }
      break;
    case fsWebDAV:
      if (HttpForWebDAV)
      {
        Url = (Data.Ftps == ftpsImplicit) ? QStringLiteral("https") : QStringLiteral("http");
      }
      else
      {
        Url = (Data.Ftps == ftpsImplicit) ? QStringLiteral("davs") : QStringLiteral("dav");
      }
      break;
    case fsS3:
      Url = (Data.Ftps == ftpsImplicit) ? QStringLiteral("s3") : QStringLiteral("s3plain");
      break;
    case fsSFTP:
    case fsSFTPonly:
    default:
      Url = QStringLiteral("sftp");
      break;
  }
  return Url + QStringLiteral("://");
}

QStringList GenerateUrlDialog::RawSettingsForUrl(const SessionData & Data)
{
  return Data.RawSettingsForUrl();
}

QString GenerateUrlDialog::GenerateSessionUrl(const SessionData & Data, unsigned int Flags)
{
  QString Url;
  if (Flags & sufSpecific)
  {
    Url += WinSCPProtocolPrefix;
  }
  Url += ProtocolUrl(Data, (Flags & sufHttpForWebDAV) != 0);

  if ((Flags & sufUserName) && !Data.UserName.isEmpty())
  {
    Url += EncodeUrlString(Data.UserNameExpanded());
    if ((Flags & sufPassword) && !Data.Password.isEmpty())
    {
      Url += QLatin1Char(':') + EncodeUrlString(Data.Password);
    }
    if ((Flags & sufHostKey) && !Data.HostKey.isEmpty())
    {
      QString KeyName;
      QString Fingerprint = Data.HostKey;
      NormalizeFingerprint(Fingerprint, KeyName);
      QString S = Fingerprint;
      if (!KeyName.isEmpty())
      {
        S = KeyName + NormalizedFingerprintSeparator + S;
      }
      S = MD5ToUrlSafe(Base64ToUrlSafe(S));
      Url += UrlParamSeparator + UrlHostKeyParamName + UrlParamValueSeparator + EncodeUrlString(S);
    }
    if (Flags & sufRawSettings)
    {
      for (const QString & Setting : RawSettingsForUrl(Data))
      {
        int P = Setting.indexOf(QLatin1Char('='));
        QString Name = Setting.left(P);
        QString Value = Setting.mid(P + 1);
        Url += UrlParamSeparator + UrlRawSettingsParamNamePrefix + EncodeUrlString(Name.toLower()) +
          UrlParamValueSeparator + EncodeUrlString(Value);
      }
    }
    Url += QLatin1Char('@');
  }

  QString HostName = Data.HostNameExpanded();
  if (SessionData::IsIPv6Literal(HostName))
  {
    // EscapeIPv6Literal
    if (!(HostName.startsWith(QLatin1Char('[')) && HostName.endsWith(QLatin1Char(']'))))
    {
      HostName = QLatin1Char('[') + HostName + QLatin1Char(']');
    }
    Url += HostName;
  }
  else
  {
    Url += EncodeUrlString(HostName);
  }
  if (Data.PortNumber != Data.DefaultPort())
  {
    Url += QLatin1Char(':') + QString::number(Data.PortNumber);
  }
  Url += QLatin1Char('/');
  return Url;
}

QString GenerateUrlDialog::GenerateOpenCommandArgs(const SessionData & Data)
{
  // TSessionData::GenerateOpenCommandArgs (Rtf = false)
  SessionData FactoryDefaults;
  SessionData Copy = Data;
  QString Result = GenerateSessionUrl(Copy, sufOpen);

  bool UsesSsh = Copy.IsSsh();
  // SFTP-only is not reflected by the protocol prefix, we have to use rawsettings for that
  if (Copy.FSProtocol != fsSFTPonly)
  {
    Copy.FSProtocol = FactoryDefaults.FSProtocol;
  }
  Copy.HostName = FactoryDefaults.HostName;
  Copy.PortNumber = FactoryDefaults.PortNumber;
  Copy.UserName = FactoryDefaults.UserName;
  Copy.Password = FactoryDefaults.Password;
  ResetNonCoreData(Copy, FactoryDefaults);
  Copy.Ftps = FactoryDefaults.Ftps;

  if (Copy.HostKey != FactoryDefaults.HostKey)
  {
    Result += Switch(UsesSsh ? QStringLiteral("hostkey") : QStringLiteral("certificate"), Copy.HostKey);
    Copy.HostKey = FactoryDefaults.HostKey;
  }
  if (Copy.PublicKeyFile != FactoryDefaults.PublicKeyFile)
  {
    Result += Switch(QStringLiteral("privatekey"), Copy.PublicKeyFile);
    Copy.PublicKeyFile = FactoryDefaults.PublicKeyFile;
  }
  if (Copy.TlsCertificateFile != FactoryDefaults.TlsCertificateFile)
  {
    Result += Switch(QStringLiteral("clientcert"), Copy.TlsCertificateFile);
    Copy.TlsCertificateFile = FactoryDefaults.TlsCertificateFile;
  }
  if (Copy.FtpPasvMode != FactoryDefaults.FtpPasvMode)
  {
    Result += Switch(QStringLiteral("passive"), Copy.FtpPasvMode ? 1 : 0);
    Copy.FtpPasvMode = FactoryDefaults.FtpPasvMode;
  }
  if (Copy.Timeout != FactoryDefaults.Timeout)
  {
    Result += Switch(QStringLiteral("timeout"), Copy.Timeout);
    Copy.Timeout = FactoryDefaults.Timeout;
  }

  QStringList RawSettings = Copy.SaveToOptions(&FactoryDefaults);
  if (!RawSettings.isEmpty())
  {
    Result += QStringLiteral(" -rawsettings");
    Result += StringsToParams(RawSettings);
  }
  return Result;
}

QString GenerateUrlDialog::CliExecutable()
{
  // Application->ExeName / winscp.com: nixscp-cli next to the application, or from PATH
  QString Local = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("nixscp-cli"));
  if (QFileInfo(Local).isExecutable())
  {
    return Local;
  }
  QString InPath = QStandardPaths::findExecutable(QStringLiteral("nixscp-cli"));
  return InPath.isEmpty() ? QStringLiteral("nixscp-cli") : InPath;
}

//---------------------------------------------------------------------------
// Dialog
//---------------------------------------------------------------------------

struct GenerateUrlDialog::Private
{
  SessionData Data;
  QStringList Paths;
  bool Changing = false;
  QTabWidget * OptionsPageControl = nullptr;
  QWidget * UrlSheet = nullptr;
  QWidget * ScriptSheet = nullptr;
  QMap<int, QCheckBox *> Checks;
  QComboBox * ScriptFormatCombo = nullptr;
  QLabel * ScriptDescriptionLabel = nullptr;
  QGroupBox * ResultGroup = nullptr;
  QPlainTextEdit * ResultMemo = nullptr;

  bool IsFileUrl() const { return !Paths.isEmpty(); }

  bool Checked(int Tag) const
  {
    QCheckBox * Check = Checks.value(Tag);
    return (Check != nullptr) && Check->isEnabled() && Check->isChecked();
  }
};

GenerateUrlDialog::GenerateUrlDialog(QWidget * Parent, const SessionData & Data, const QStringList & Paths) :
  QDialog(Parent),
  d(new Private)
{
  d->Data = Data;
  d->Paths = Paths;
  setObjectName(QStringLiteral("GenerateUrlDialog"));
  setSizeGripEnabled(true);
  setMinimumSize(500, 300);

  d->OptionsPageControl = new QTabWidget(this);
  d->UrlSheet = new QWidget(d->OptionsPageControl);
  QGridLayout * UrlLayout = new QGridLayout(d->UrlSheet);
  struct CheckDef { int Tag; const char * Caption; int Row; int Column; };
  const CheckDef CheckDefs[] =
  {
    { gucUserName, "&Username", 0, 0 },
    { gucPassword, "&Password", 1, 0 },
    { gucHostKey, "SSH &host key", 2, 0 },
    { gucRemoteDirectory, "Initial &directory", 0, 1 },
    { gucWinSCPSpecific, "&WinSCP-specific", 1, 1 },
    { gucSaveExtension, "&Save extension", 2, 1 },
    { gucRawSettings, "&Advanced settings", 0, 2 },
  };
  for (const CheckDef & Def : CheckDefs)
  {
    QCheckBox * Check = new QCheckBox(QString::fromLatin1(Def.Caption), d->UrlSheet);
    d->Checks.insert(Def.Tag, Check);
    UrlLayout->addWidget(Check, Def.Row, Def.Column);
    connect(Check, &QCheckBox::toggled, this, [this]() { UpdateControls(); });
  }
  UrlLayout->setRowStretch(3, 1);
  d->OptionsPageControl->addTab(d->UrlSheet, QStringLiteral("URL"));

  d->ScriptSheet = new QWidget(d->OptionsPageControl);
  QGridLayout * ScriptLayout = new QGridLayout(d->ScriptSheet);
  d->ScriptFormatCombo = new QComboBox(d->ScriptSheet);
  d->ScriptFormatCombo->addItems({ QStringLiteral("Script file"), QStringLiteral("Shell script"), QStringLiteral("Command-line") });
  QLabel * FormatLabel = new QLabel(QStringLiteral("&Format:"), d->ScriptSheet);
  FormatLabel->setBuddy(d->ScriptFormatCombo);
  d->ScriptDescriptionLabel = new QLabel(d->ScriptSheet);
  d->ScriptDescriptionLabel->setWordWrap(true);
  d->ScriptDescriptionLabel->setTextFormat(Qt::PlainText);
  ScriptLayout->addWidget(FormatLabel, 0, 0);
  ScriptLayout->addWidget(d->ScriptFormatCombo, 0, 1);
  ScriptLayout->setColumnStretch(2, 1);
  ScriptLayout->addWidget(d->ScriptDescriptionLabel, 1, 0, 1, 3);
  ScriptLayout->setRowStretch(2, 1);
  d->OptionsPageControl->addTab(d->ScriptSheet, QStringLiteral("Script"));
  connect(d->ScriptFormatCombo, &QComboBox::currentIndexChanged, this, [this]() { UpdateControls(); });
  connect(d->OptionsPageControl, &QTabWidget::currentChanged, this, [this]() { UpdateControls(); });

  d->ResultGroup = new QGroupBox(this);
  QVBoxLayout * ResultLayout = new QVBoxLayout(d->ResultGroup);
  d->ResultMemo = new QPlainTextEdit(d->ResultGroup);
  d->ResultMemo->setObjectName(QStringLiteral("ResultMemo"));
  d->ResultMemo->setReadOnly(true);
  ResultLayout->addWidget(d->ResultMemo);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  QPushButton * ClipboardButton = Buttons->addButton(QStringLiteral("&Copy to Clipboard"), QDialogButtonBox::ResetRole);
  ClipboardButton->setAutoDefault(false);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_generateurl"));
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // ClipboardButtonClick
  connect(ClipboardButton, &QPushButton::clicked, this, [this]()
  {
    QApplication::clipboard()->setText(ResultText());
  });

  QVBoxLayout * Layout = new QVBoxLayout(this);
  d->OptionsPageControl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  Layout->addWidget(d->OptionsPageControl);
  Layout->addWidget(d->ResultGroup, 1);
  Layout->addWidget(Buttons);

  // UrlSheet->TabVisible = !FTransfer; ScriptSheet->TabVisible = !IsFileUrl()
  d->OptionsPageControl->setTabVisible(1, !d->IsFileUrl());
  resize(539, 381);
  UpdateControls();
}

GenerateUrlDialog::~GenerateUrlDialog()
{
  delete d;
}

QString GenerateUrlDialog::GenerateUrl() const
{
  // TGenerateUrlDialog::GenerateUrl(Path)
  auto Generate = [this](QString Path)
  {
    QString Url = GenerateSessionUrl(d->Data,
      (d->Checked(gucWinSCPSpecific) ? sufSpecific : 0) |
      (d->Checked(gucUserName) ? sufUserName : 0) |
      (d->Checked(gucPassword) ? sufPassword : 0) |
      (d->Checked(gucHostKey) ? sufHostKey : 0) |
      (d->Checked(gucRawSettings) ? sufRawSettings : 0) |
      (HttpForWebDAV() ? sufHttpForWebDAV : 0));
    if (d->Checked(gucRemoteDirectory) || d->IsFileUrl())
    {
      if (Path.startsWith(QLatin1Char('/')))
      {
        Path.remove(0, 1);
      }
      Url += EncodeUrlPath(Path);
    }
    if (d->Checked(gucSaveExtension))
    {
      Url += UrlParamSeparator + UrlSaveParamName;
    }
    return Url;
  };

  QString Result;
  if (!d->IsFileUrl())
  {
    QString Path = d->Data.RemoteDirectory;
    if (!Path.isEmpty() && !Path.endsWith(QLatin1Char('/')))
    {
      Path += QLatin1Char('/');
    }
    Result = Generate(Path);
  }
  else
  {
    QStringList Urls;
    for (const QString & APath : d->Paths)
    {
      QString Path = APath;
      if (!Path.startsWith(QLatin1Char('/')) && !d->Data.RemoteDirectory.isEmpty())
      {
        QString Dir = d->Data.RemoteDirectory;
        Path = (Dir.endsWith(QLatin1Char('/')) ? Dir : (Dir + QLatin1Char('/'))) + Path;
      }
      Urls.append(Generate(Path));
    }
    Result = Urls.join(QLatin1Char('\n'));
  }
  return Result;
}

QString GenerateUrlDialog::GenerateScript(ScriptFormat Format, QString & Description) const
{
  // TGenerateUrlDialog::GenerateScript
  QString ExeName = CliExecutable();
  QString BaseExeName = QFileInfo(ExeName).completeBaseName();
  QString OpenCommand = GenerateOpenCommandArgs(d->Data);
  QString CommandPlaceholder1 = QStringLiteral("Your command %1").arg(1);   // GENERATE_URL_COMMAND
  QString CommandPlaceholder2 = QStringLiteral("Your command %1").arg(2);
  QString LogPath = WritablePathToLog + BaseExeName + QStringLiteral(".log");

  QStringList Commands;
  Commands << QStringLiteral("open ") + OpenCommand;
  Commands << QString();
  Commands << QStringLiteral("# ") + CommandPlaceholder1;
  Commands << QStringLiteral("# ") + CommandPlaceholder2;
  Commands << QString();
  Commands << QStringLiteral("exit");

  QString Result;
  if (Format == sfScriptFile)
  {
    for (const QString & Command : Commands)
    {
      Result += Command + QLatin1Char('\n');
    }
    QString ScriptCommandLine = QStringLiteral("%1 /log=%2 /ini=nul /script=%3")
      .arg(ShellQuote(ExeName), ShellQuote(LogPath), ShellQuote(PathToScript));
    Result +=
      QLatin1Char('\n') +
      QStringLiteral("# Execute the script using a command like:\n") +   // GENERATE_URL_SCRIPT_DESC
      QStringLiteral("# ") + ScriptCommandLine + QLatin1Char('\n');
  }
  else if (Format == sfShellScript)
  {
    // Linux replacement of sfBatchFile
    Result =
      QStringLiteral("#!/bin/sh\n\n") +
      ShellQuote(ExeName) + QStringLiteral(" \\\n") +
      QStringLiteral("  /log=") + ShellQuote(LogPath) + QStringLiteral(" /ini=nul \\\n") +
      QStringLiteral("  /command");
    for (QString Command : Commands)
    {
      if (!Command.isEmpty())
      {
        if (Command.startsWith(QLatin1Char('#')))
        {
          Command = Command.mid(1).trimmed();
        }
        Result += QStringLiteral(" \\\n    ") + ShellQuote(Command);
      }
    }
    Result +=
      QStringLiteral("\n\n") +
      QStringLiteral("WINSCP_RESULT=$?\n") +
      QStringLiteral("if [ $WINSCP_RESULT -eq 0 ]; then\n") +
      QStringLiteral("  echo Success\n") +
      QStringLiteral("else\n") +
      QStringLiteral("  echo Error\n") +
      QStringLiteral("fi\n\n") +
      QStringLiteral("exit $WINSCP_RESULT\n");
  }
  else if (Format == sfCommandLine)
  {
    Result = QStringLiteral("/log=") + ShellQuote(LogPath) + QStringLiteral(" /ini=nul /command");
    for (QString Command : Commands)
    {
      if (!Command.isEmpty())
      {
        if (Command.startsWith(QLatin1Char('#')))
        {
          Command = Command.mid(1).trimmed();
        }
        Result += QLatin1Char(' ') + ShellQuote(Command);
      }
    }
    // GENERATE_URL_COMMANDLINE_DESC
    Description = QStringLiteral("Command-line arguments for %1").arg(ShellQuote(ExeName)) + QLatin1Char('\n') + Description;
  }
  return Result;
}

void GenerateUrlDialog::UpdateControls()
{
  if (d->Changing)
  {
    return;
  }
  // GENERATE_URL_FILE_TITLE / GENERATE_URL_SESSION_TITLE
  setWindowTitle(d->IsFileUrl() ? QStringLiteral("Generate file URL") : QStringLiteral("Generate session URL/code"));

  bool HostKeyUnknown = d->Data.IsSsh() && d->Data.HostKey.isEmpty();
  bool UrlPage = (d->OptionsPageControl->currentWidget() == d->UrlSheet);
  ScriptFormat Format = static_cast<ScriptFormat>(std::max(0, d->ScriptFormatCombo->currentIndex()));

  if (UrlPage)
  {
    d->ResultGroup->setTitle(QStringLiteral("URL"));   // GENERATE_URL_URL
  }
  else if (Format == sfCommandLine)
  {
    d->ResultGroup->setTitle(QStringLiteral("Command-line arguments"));   // GENERATE_URL_COMMANDLINE_LABEL
  }
  else
  {
    d->ResultGroup->setTitle(d->ScriptFormatCombo->currentText());
  }

  d->Checks[gucUserName]->setEnabled(!d->Data.UserNameExpanded().isEmpty());
  bool UserNameIncluded = d->Checked(gucUserName);
  d->Checks[gucPassword]->setEnabled(UserNameIncluded && d->Data.HasPassword());
  d->Checks[gucHostKey]->setEnabled(UserNameIncluded && !d->Data.HostKey.isEmpty());
  d->Checks[gucRemoteDirectory]->setEnabled(!d->Data.RemoteDirectory.isEmpty() && !d->IsFileUrl());
  d->Checks[gucSaveExtension]->setEnabled(!d->IsFileUrl());
  d->Checks[gucRawSettings]->setEnabled(UserNameIncluded && !RawSettingsForUrl(d->Data).isEmpty());

  QString Result;
  bool WordWrap;
  bool FixedWidth;
  if (UrlPage)
  {
    Result = GenerateUrl();
    WordWrap = true;
    FixedWidth = false;
  }
  else
  {
    QString Description;
    if (HostKeyUnknown)
    {
      Description = QStringLiteral("A server host key is not known. Please connect at least once, before generating the code.") + QLatin1Char('\n');
    }
    Result = GenerateScript(Format, Description);
    d->ScriptDescriptionLabel->setText(Description.trimmed());
    WordWrap = (Format == sfCommandLine);
    FixedWidth = (Format != sfCommandLine);
  }
  d->ResultMemo->setLineWrapMode(WordWrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  d->ResultMemo->setFont(FixedWidth ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : font());
  d->ResultMemo->setPlainText(Result);
}

QString GenerateUrlDialog::ResultText() const
{
  return d->ResultMemo->toPlainText();
}

QString GenerateUrlDialog::Description() const
{
  return d->ScriptDescriptionLabel->text();
}

QString GenerateUrlDialog::ResultCaption() const
{
  return d->ResultGroup->title();
}

QTabWidget * GenerateUrlDialog::PageControl() const
{
  return d->OptionsPageControl;
}

QCheckBox * GenerateUrlDialog::Check(int Tag) const
{
  return d->Checks.value(Tag);
}

QComboBox * GenerateUrlDialog::ScriptFormatCombo() const
{
  return d->ScriptFormatCombo;
}

void GenerateUrlDialog::LoadOptions()
{
  // TGenerateUrlDialog::Execute
  int Components = ReadInterfaceInt(QStringLiteral("GenerateUrlComponents"), -1);
  if (Components < 0)
  {
    Components = gucUserName | gucRemoteDirectory;
  }
  int Target = ReadInterfaceInt(QStringLiteral("GenerateUrlCodeTarget"), 0);
  int Format = ReadInterfaceInt(QStringLiteral("GenerateUrlScriptFormat"), sfScriptFile);
  if ((Format < sfScriptFile) || (Format > sfCommandLine))
  {
    Format = sfScriptFile;   // WinSCP's PowerShell script
  }
  {
    d->Changing = true;
    d->OptionsPageControl->setCurrentWidget(((Target == 1) && !d->IsFileUrl()) ? d->ScriptSheet : d->UrlSheet);
    for (auto It = d->Checks.cbegin(); It != d->Checks.cend(); ++It)
    {
      It.value()->setChecked((Components & It.key()) != 0);
    }
    d->ScriptFormatCombo->setCurrentIndex(Format);
    d->Changing = false;
  }
  UpdateControls();
  if (d->OptionsPageControl->currentWidget() != d->UrlSheet)
  {
    resize(777, 666);
  }
}

void GenerateUrlDialog::SaveOptions()
{
  bool UrlPage = (d->OptionsPageControl->currentWidget() == d->UrlSheet);
  // Do not save the selection for files as the "URL" was selected implicitly
  if (!d->IsFileUrl())
  {
    WriteInterfaceInt(QStringLiteral("GenerateUrlCodeTarget"), UrlPage ? 0 : 1);
  }
  if (UrlPage)
  {
    int Components = 0;
    for (auto It = d->Checks.cbegin(); It != d->Checks.cend(); ++It)
    {
      if (It.value()->isChecked())
      {
        Components |= It.key();
      }
    }
    WriteInterfaceInt(QStringLiteral("GenerateUrlComponents"), Components);
  }
  else
  {
    WriteInterfaceInt(QStringLiteral("GenerateUrlScriptFormat"), d->ScriptFormatCombo->currentIndex());
  }
}

void GenerateUrlDialog::Execute(QWidget * Parent, const SessionData & Data, const QStringList & Paths)
{
  GenerateUrlDialog Dialog(Parent, Data, Paths);
  Dialog.LoadOptions();
  Dialog.exec();
  Dialog.SaveOptions();
  Configuration::Instance().Save();
}

// Login dialog: Manage > Generate Session URL/Code... (LoginDialog::GenerateUrlHook),
// installed once the application object exists (after all static initialization)
static void InstallGenerateUrlHook()
{
  if (!LoginDialog::GenerateUrlHook)
  {
    LoginDialog::GenerateUrlHook = [](const SessionData & Data, QWidget * Parent)
    {
      GenerateUrlDialog::Execute(Parent, Data);
    };
  }
}
Q_COREAPP_STARTUP_FUNCTION(InstallGenerateUrlHook)
