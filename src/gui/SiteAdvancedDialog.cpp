// Port of WinSCP source/forms/SiteAdvanced.cpp + SiteAdvanced.dfm
#include "gui/SiteAdvancedDialog.h"
#include "gui/HelpSystem.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SecureShell.h"
#include "gui/GuiUserInterface.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QShortcut>
#include <QToolButton>
#include <QWidgetAction>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <cmath>

static const QString DefaultRecycleBinPath = QStringLiteral("/tmp");

//---------------------------------------------------------------------------
// windows/VCLCommon.cpp: ComboAutoSwitchInitialize/Load/Save
static void ComboAutoSwitchInitialize(QComboBox * Combo)
{
  Combo->clear();
  Combo->addItem(QStringLiteral("Auto"));
  Combo->addItem(QStringLiteral("Off"));
  Combo->addItem(QStringLiteral("On"));
}

static void ComboAutoSwitchLoad(QComboBox * Combo, TAutoSwitch Value)
{
  Combo->setCurrentIndex(2 - Value);
}

static TAutoSwitch ComboAutoSwitchSave(QComboBox * Combo)
{
  return static_cast<TAutoSwitch>(2 - Combo->currentIndex());
}

static QComboBox * AutoSwitchCombo()
{
  auto * Combo = new QComboBox();
  ComboAutoSwitchInitialize(Combo);
  return Combo;
}

// Display names (resource/TextsWin1.rc CIPHER_NAME_*, KEX_NAME_*), in enum order
static QString CipherDisplayName(const QString & Name)
{
  static const QStringList Captions = {
    QStringLiteral("-- warn below here --"), QStringLiteral("3DES"), QStringLiteral("Blowfish"), QStringLiteral("AES"),
    QStringLiteral("DES"), QStringLiteral("Arcfour"), QStringLiteral("ChaCha20"), QStringLiteral("AES-GCM") };
  int Index = SessionData::CipherNames().indexOf(Name);
  return (Index >= 0) ? Captions[Index] : Name;
}

static QString KexDisplayName(const QString & Name)
{
  static const QStringList Captions = {
    QStringLiteral("-- warn below here --"),
    QStringLiteral("Diffie-Hellman group 1 (1024-bit)"),
    QStringLiteral("Diffie-Hellman group 14 (2048-bit)"),
    QStringLiteral("Diffie-Hellman group 15 (3072-bit)"),
    QStringLiteral("Diffie-Hellman group 16 (4096-bit)"),
    QStringLiteral("Diffie-Hellman group 17 (6144-bit)"),
    QStringLiteral("Diffie-Hellman group 18 (8192-bit)"),
    QStringLiteral("Diffie-Hellman group exchange"),
    QStringLiteral("RSA-based key exchange"),
    QStringLiteral("ECDH key exchange"),
    QStringLiteral("NTRU Prime / Curve25519 hybrid kex"),
    QStringLiteral("ML-KEM / Curve25519 hybrid kex"),
    QStringLiteral("ML-KEM / NIST ECDH hybrid kex") };
  int Index = SessionData::KexNames().indexOf(Name);
  return (Index >= 0) ? Captions[Index] : Name;
}

static int TlsVersionToIndex(TTlsVersion Version)
{
  switch (Version)
  {
    case tls10: return 0;
    case tls11: return 1;
    case tls12: return 2;
    default: return 3;
  }
}

static TTlsVersion IndexToTlsVersion(int Index)
{
  static const TTlsVersion Versions[] = { tls10, tls11, tls12, tls13 };
  return Versions[qBound(0, Index, 3)];
}

//---------------------------------------------------------------------------
struct SiteAdvancedDialog::Private
{
  QTreeWidget * NavigationTree;
  QStackedWidget * PageControl;
  struct Page
  {
    QString Caption;
    QWidget * Sheet;
    bool Indented;
    bool Enabled;
  };
  QList<Page> Pages;
  QWidget * EnvironmentSheet, * DirectoriesSheet, * RecycleBinSheet, * SftpSheet, * ScpSheet, * FtpSheet,
    * S3Sheet, * WebDavSheet, * ConnSheet, * ProxySheet, * TunnelSheet, * SslSheet, * AdvancedSheet,
    * KexSheet, * AuthSheet, * BugsSheet, * NoteSheet;
  bool NoUpdate = false;
  int Color = 0;
  QPushButton * ColorButton;

  // Environment
  QComboBox * EOLTypeCombo;
  QLabel * EOLTypeLabel;
  QComboBox * UtfCombo;
  QLabel * UtfLabel;
  QLabel * TimeDifferenceLabel;
  QSpinBox * TimeDifferenceEdit;
  QSpinBox * TimeDifferenceMinutesEdit;
  QCheckBox * TimeDifferenceAutoCheck;
  QCheckBox * TrimVMSVersionsCheck;
  // Directories
  QLineEdit * LocalDirectoryEdit;
  QLineEdit * RemoteDirectoryEdit;
  QCheckBox * UpdateDirectoriesCheck;
  QCheckBox * SynchronizeBrowsingCheck;
  QCheckBox * CacheDirectoriesCheck;
  QCheckBox * CacheDirectoryChangesCheck;
  QCheckBox * PreserveDirectoryChangesCheck;
  QCheckBox * ResolveSymlinksCheck;
  QCheckBox * FollowDirectorySymlinksCheck;
  // Recycle bin
  QCheckBox * DeleteToRecycleBinCheck;
  QCheckBox * OverwrittenToRecycleBinCheck;
  QLabel * RecycleBinPathLabel;
  QLineEdit * RecycleBinPathEdit;
  // SFTP
  QComboBox * SFTPBugSymlinkCombo;
  QComboBox * SFTPBugSignedTSCombo;
  QComboBox * SftpServerEdit;
  QCheckBox * AllowScpFallbackCheck;
  QComboBox * SFTPRealPathCombo;
  QCheckBox * UsePosixRenameCheck;
  // SCP/Shell
  QComboBox * ShellEdit;
  QComboBox * ReturnVarEdit;
  QGroupBox * ScpLsOptionsGroup;
  QComboBox * ListingCommandEdit;
  QCheckBox * IgnoreLsWarningsCheck;
  QCheckBox * SCPLsFullTimeAutoCheck;
  QGroupBox * OtherShellOptionsGroup;
  QCheckBox * LookupUserGroupsCheck;
  QCheckBox * ClearAliasesCheck;
  QCheckBox * UnsetNationalVarsCheck;
  QCheckBox * Scp1CompatibilityCheck;
  // FTP
  QPlainTextEdit * PostLoginCommandsMemo;
  QComboBox * FtpListAllCombo;
  QLabel * FtpListAllLabel;
  QComboBox * FtpUseMlsdCombo;
  QComboBox * FtpForcePasvIpCombo;
  QLabel * FtpForcePasvIpLabel;
  QLineEdit * FtpAccountEdit;
  QComboBox * FtpHostCombo;
  QCheckBox * VMSAllRevisionsCheck;
  // S3
  QComboBox * S3DefaultRegionCombo;
  QComboBox * S3UrlStyleCombo;
  QCheckBox * S3RequesterPaysCheck;
  QPlainTextEdit * S3SessionTokenMemo;
  QLineEdit * S3RoleArnEdit;
  // WebDAV
  QCheckBox * WebDavLiberalEscapingCheck;
  // Connection
  QCheckBox * FtpPasvModeCheck;
  QCheckBox * BufferSizeCheck;
  QSpinBox * TimeoutEdit;
  QGroupBox * PingGroup;
  QRadioButton * PingOffButton;
  QRadioButton * PingNullPacketButton;
  QRadioButton * PingDummyCommandButton;
  QSpinBox * PingIntervalSecEdit;
  QLabel * PingIntervalLabel;
  QGroupBox * FtpPingGroup;
  QRadioButton * FtpPingOffButton;
  QRadioButton * FtpPingDummyCommandButton;
  QRadioButton * FtpPingDirectoryListingButton;
  QSpinBox * FtpPingIntervalSecEdit;
  QLabel * FtpPingIntervalLabel;
  QGroupBox * IPvGroup;
  QRadioButton * IPAutoButton;
  QRadioButton * IPv4Button;
  QRadioButton * IPv6Button;
  // Proxy
  QComboBox * ProxyMethodCombo;
  QPushButton * ProxyAutodetectButton;
  QPushButton * PrivateKeyViewButton;
  QPushButton * PrivateKeyToolsButton;
  QLabel * ProxyHostLabel;
  QLineEdit * ProxyHostEdit;
  QLabel * ProxyPortLabel;
  QSpinBox * ProxyPortEdit;
  QLabel * ProxyUsernameLabel;
  QLineEdit * ProxyUsernameEdit;
  QLabel * ProxyPasswordLabel;
  QLineEdit * ProxyPasswordEdit;
  QGroupBox * ProxySettingsGroup;
  QLabel * ProxyTelnetCommandLabel;
  QLineEdit * ProxyTelnetCommandEdit;
  QLabel * ProxyLocalCommandLabel;
  QLineEdit * ProxyLocalCommandEdit;
  QPushButton * ProxyLocalCommandBrowseButton;
  QComboBox * ProxyDNSCombo;
  QCheckBox * ProxyLocalhostCheck;
  // Tunnel
  QCheckBox * TunnelCheck;
  QGroupBox * TunnelSessionGroup;
  QLineEdit * TunnelHostNameEdit;
  QSpinBox * TunnelPortNumberEdit;
  QLineEdit * TunnelUserNameEdit;
  QLineEdit * TunnelPasswordEdit;
  QGroupBox * TunnelAuthenticationParamsGroup;
  QLineEdit * TunnelPrivateKeyEdit;
  // TLS/SSL
  QComboBox * MinTlsVersionCombo;
  QComboBox * MaxTlsVersionCombo;
  QCheckBox * SslSessionReuseCheck;
  QGroupBox * TlsAuthenticationGroup;
  QLineEdit * TlsCertificateFileEdit;
  // SSH
  QCheckBox * CompressionCheck;
  QListWidget * CipherListBox;
  QPushButton * CipherUpButton;
  QPushButton * CipherDownButton;
  // Key exchange
  QListWidget * KexListBox;
  QPushButton * KexUpButton;
  QPushButton * KexDownButton;
  QGroupBox * KexReexchangeGroup;
  QLineEdit * RekeyDataEdit;
  // Authentication
  QCheckBox * SshNoUserAuthCheck;
  QGroupBox * AuthenticationGroup;
  QCheckBox * TryAgentCheck;
  QCheckBox * AuthKICheck;
  QCheckBox * AuthKIPasswordCheck;
  QGroupBox * AuthenticationParamsGroup;
  QCheckBox * AgentFwdCheck;
  QLineEdit * PrivateKeyEdit;
  QLabel * DetachedCertificateLabel;
  QLineEdit * DetachedCertificateEdit;
  QCheckBox * AuthGSSAPICheck;
  QCheckBox * GSSAPIFwdTGTCheck;
  // Bugs
  QComboBox * BugCombos[sbCount] = {};
  // Note
  QPlainTextEdit * NoteMemo;
};

//---------------------------------------------------------------------------
static QWidget * NewSheet(QVBoxLayout *& Layout)
{
  auto * Sheet = new QWidget();
  Layout = new QVBoxLayout(Sheet);
  Layout->setContentsMargins(0, 0, 0, 0);
  return Sheet;
}

static QLabel * Buddy(const QString & Caption, QWidget * Control)
{
  auto * Label = new QLabel(Caption);
  Label->setBuddy(Control);
  return Label;
}

static QHBoxLayout * Indent(QWidget * W, int Amount = 20)
{
  auto * Row = new QHBoxLayout();
  Row->addSpacing(Amount);
  Row->addWidget(W);
  return Row;
}

static QWidget * FileEdit(QLineEdit * Edit, QWidget * Parent, const QString & Title, const QString & Filter, bool Directory = false)
{
  auto * Box = new QWidget();
  auto * Row = new QHBoxLayout(Box);
  Row->setContentsMargins(0, 0, 0, 0);
  Row->addWidget(Edit, 1);
  auto * Button = new QPushButton(QStringLiteral("..."));
  Button->setMaximumWidth(32);
  Row->addWidget(Button);
  QObject::connect(Button, &QPushButton::clicked, Parent, [Edit, Parent, Title, Filter, Directory]()
  {
    QString Start = Edit->text().isEmpty() ? QDir::homePath() : Edit->text();
    QString Result = Directory ?
      QFileDialog::getExistingDirectory(Parent, Title, Start) :
      QFileDialog::getOpenFileName(Parent, Title, Start, Filter);
    if (!Result.isEmpty())
    {
      Edit->setText(QDir::toNativeSeparators(Result));
    }
  });
  return Box;
}

static const QString PrivateKeyFilter =
  QStringLiteral("All Private Key Files (*.ppk *.pem *.key id_*);;PuTTY Private Key Files (*.ppk);;All Files (*)");

SiteAdvancedDialog::SiteAdvancedDialog(SessionData & Data, QWidget * Parent) :
  QDialog(Parent),
  d(new Private),
  FData(Data)
{
  setWindowTitle(QStringLiteral("Advanced Site Settings"));
  resize(700, 560);

  QVBoxLayout * L;

  // ---- Environment ----
  d->EnvironmentSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Server environment"));
    auto * Grid = new QGridLayout(G);
    d->EOLTypeCombo = new QComboBox();
    d->EOLTypeCombo->addItems({QStringLiteral("LF"), QStringLiteral("CR/LF")});
    d->EOLTypeLabel = Buddy(QStringLiteral("End-&of-line characters (if not indicated by server):"), d->EOLTypeCombo);
    Grid->addWidget(d->EOLTypeLabel, 0, 0);
    Grid->addWidget(d->EOLTypeCombo, 0, 1);
    d->UtfCombo = AutoSwitchCombo();
    d->UtfLabel = Buddy(QStringLiteral("&UTF-8 encoding for filenames:"), d->UtfCombo);
    Grid->addWidget(d->UtfLabel, 1, 0);
    Grid->addWidget(d->UtfCombo, 1, 1);
    d->TimeDifferenceEdit = new QSpinBox();
    d->TimeDifferenceEdit->setRange(-25, 25);
    d->TimeDifferenceMinutesEdit = new QSpinBox();
    d->TimeDifferenceMinutesEdit->setRange(-59, 59);
    d->TimeDifferenceMinutesEdit->setSingleStep(15);
    d->TimeDifferenceLabel = Buddy(QStringLiteral("Time &zone offset:"), d->TimeDifferenceEdit);
    auto * TzRow = new QHBoxLayout();
    TzRow->addWidget(d->TimeDifferenceEdit);
    TzRow->addWidget(new QLabel(QStringLiteral("hours")));
    TzRow->addWidget(d->TimeDifferenceMinutesEdit);
    TzRow->addWidget(new QLabel(QStringLiteral("minutes")));
    TzRow->addStretch();
    Grid->addWidget(d->TimeDifferenceLabel, 2, 0);
    Grid->addLayout(TzRow, 2, 1);
    d->TimeDifferenceAutoCheck = new QCheckBox(QStringLiteral("Detect &automatically"));
    Grid->addWidget(d->TimeDifferenceAutoCheck, 3, 1);
    d->TrimVMSVersionsCheck = new QCheckBox(QStringLiteral("&Trim VMS version numbers"));
    Grid->addWidget(d->TrimVMSVersionsCheck, 4, 0, 1, 2);
    L->addWidget(G);
    L->addStretch();
  }

  // ---- Environment > Directories ----
  d->DirectoriesSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Directories"));
    auto * V = new QVBoxLayout(G);
    d->SynchronizeBrowsingCheck = new QCheckBox(QStringLiteral("&Synchronize browsing"));
    V->addWidget(d->SynchronizeBrowsingCheck);
    d->RemoteDirectoryEdit = new QLineEdit();
    V->addWidget(Buddy(QStringLiteral("&Remote directory:"), d->RemoteDirectoryEdit));
    V->addWidget(d->RemoteDirectoryEdit);
    d->LocalDirectoryEdit = new QLineEdit();
    V->addWidget(Buddy(QStringLiteral("&Local directory:"), d->LocalDirectoryEdit));
    V->addWidget(FileEdit(d->LocalDirectoryEdit, this, QString(), QString(), true));
    d->UpdateDirectoriesCheck = new QCheckBox(QStringLiteral("Re&member last used directory"));
    V->addWidget(d->UpdateDirectoriesCheck);
    L->addWidget(G);

    auto * G2 = new QGroupBox(QStringLiteral("Directory reading options"));
    auto * V2 = new QVBoxLayout(G2);
    d->CacheDirectoriesCheck = new QCheckBox(QStringLiteral("Cache &visited remote directories"));
    d->CacheDirectoryChangesCheck = new QCheckBox(QStringLiteral("Cache &directory changes"));
    d->PreserveDirectoryChangesCheck = new QCheckBox(QStringLiteral("&Permanent cache"));
    d->ResolveSymlinksCheck = new QCheckBox(QStringLiteral("Resolve symbolic li&nks"));
    d->FollowDirectorySymlinksCheck = new QCheckBox(QStringLiteral("&Follow symbolic links to directories"));
    V2->addWidget(d->CacheDirectoriesCheck);
    V2->addWidget(d->CacheDirectoryChangesCheck);
    V2->addLayout(Indent(d->PreserveDirectoryChangesCheck));
    V2->addWidget(d->ResolveSymlinksCheck);
    V2->addWidget(d->FollowDirectorySymlinksCheck);
    L->addWidget(G2);
    L->addStretch();
  }

  // ---- Environment > Recycle bin ----
  d->RecycleBinSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Recycle bin"));
    auto * V = new QVBoxLayout(G);
    d->DeleteToRecycleBinCheck = new QCheckBox(QStringLiteral("&Preserve deleted remote files to recycle bin"));
    d->OverwrittenToRecycleBinCheck = new QCheckBox(QStringLiteral("Preserve &overwritten remote files to recycle bin (SFTP only)"));
    d->RecycleBinPathEdit = new QLineEdit();
    d->RecycleBinPathLabel = Buddy(QStringLiteral("&Remote recycle bin:"), d->RecycleBinPathEdit);
    V->addWidget(d->DeleteToRecycleBinCheck);
    V->addWidget(d->OverwrittenToRecycleBinCheck);
    V->addWidget(d->RecycleBinPathLabel);
    V->addWidget(d->RecycleBinPathEdit);
    L->addWidget(G);
    L->addStretch();
  }

  // ---- Environment > SFTP ----
  d->SftpSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Protocol options"));
    auto * Grid = new QGridLayout(G);
    d->SftpServerEdit = new QComboBox();
    d->SftpServerEdit->setEditable(true);
    d->SftpServerEdit->addItems({QStringLiteral("Default"), QStringLiteral("/bin/sftp-server"), QStringLiteral("sudo su -c /bin/sftp-server")});
    Grid->addWidget(Buddy(QStringLiteral("SFTP ser&ver:"), d->SftpServerEdit), 0, 0);
    Grid->addWidget(d->SftpServerEdit, 0, 1);
    // libssh speaks version 3 only (WinSCP: "Preferred SFTP protocol version")
    Grid->addWidget(new QLabel(QStringLiteral("SFTP protocol version:")), 1, 0);
    Grid->addWidget(new QLabel(QStringLiteral("3 (the only version NixSCP uses)")), 1, 1);
    d->SFTPRealPathCombo = AutoSwitchCombo();
    Grid->addWidget(Buddy(QStringLiteral("Ca&nonicalize paths on the server"), d->SFTPRealPathCombo), 2, 0);
    Grid->addWidget(d->SFTPRealPathCombo, 2, 1);
    d->AllowScpFallbackCheck = new QCheckBox(QStringLiteral("Allow SCP &fallback"));
    Grid->addWidget(d->AllowScpFallbackCheck, 3, 0, 1, 2);
    d->UsePosixRenameCheck = new QCheckBox(QStringLiteral("&Use POSIX rename"));
    Grid->addWidget(d->UsePosixRenameCheck, 4, 0, 1, 2);
    L->addWidget(G);

    auto * G2 = new QGroupBox(QStringLiteral("Detection of known bugs in SFTP servers"));
    auto * Grid2 = new QGridLayout(G2);
    d->SFTPBugSymlinkCombo = AutoSwitchCombo();
    d->SFTPBugSignedTSCombo = AutoSwitchCombo();
    Grid2->addWidget(Buddy(QStringLiteral("&Reverses order of link command arguments:"), d->SFTPBugSymlinkCombo), 0, 0);
    Grid2->addWidget(d->SFTPBugSymlinkCombo, 0, 1);
    Grid2->addWidget(Buddy(QStringLiteral("&Misinterprets file timestamps prior to 1970:"), d->SFTPBugSignedTSCombo), 1, 0);
    Grid2->addWidget(d->SFTPBugSignedTSCombo, 1, 1);
    L->addWidget(G2);
    L->addStretch();
  }

  // ---- Environment > SCP/Shell ----
  d->ScpSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Shell"));
    auto * Grid = new QGridLayout(G);
    d->ShellEdit = new QComboBox();
    d->ShellEdit->setEditable(true);
    d->ShellEdit->addItems({QStringLiteral("Default"), QStringLiteral("/bin/bash"), QStringLiteral("/bin/ksh"), QStringLiteral("/bin/sh"), QStringLiteral("sudo su -")});
    d->ReturnVarEdit = new QComboBox();
    d->ReturnVarEdit->setEditable(true);
    d->ReturnVarEdit->addItems({QStringLiteral("Autodetect"), QStringLiteral("?"), QStringLiteral("status")});
    Grid->addWidget(Buddy(QStringLiteral("S&hell:"), d->ShellEdit), 0, 0);
    Grid->addWidget(d->ShellEdit, 0, 1);
    Grid->addWidget(Buddy(QStringLiteral("&Return code variable:"), d->ReturnVarEdit), 1, 0);
    Grid->addWidget(d->ReturnVarEdit, 1, 1);
    Grid->setColumnStretch(1, 1);
    L->addWidget(G);

    d->ScpLsOptionsGroup = new QGroupBox(QStringLiteral("Directory listing"));
    auto * Grid2 = new QGridLayout(d->ScpLsOptionsGroup);
    d->ListingCommandEdit = new QComboBox();
    d->ListingCommandEdit->setEditable(true);
    d->ListingCommandEdit->addItems({QStringLiteral("ls -la"), QStringLiteral("ls -gla")});
    Grid2->addWidget(Buddy(QStringLiteral("Listing &command:"), d->ListingCommandEdit), 0, 0);
    Grid2->addWidget(d->ListingCommandEdit, 0, 1);
    Grid2->setColumnStretch(1, 1);
    d->IgnoreLsWarningsCheck = new QCheckBox(QStringLiteral("Ignore LS &warnings"));
    d->SCPLsFullTimeAutoCheck = new QCheckBox(QStringLiteral("Try to get &full timestamp"));
    Grid2->addWidget(d->IgnoreLsWarningsCheck, 1, 0, 1, 2);
    Grid2->addWidget(d->SCPLsFullTimeAutoCheck, 2, 0, 1, 2);
    L->addWidget(d->ScpLsOptionsGroup);

    d->OtherShellOptionsGroup = new QGroupBox(QStringLiteral("Other options"));
    auto * Grid3 = new QGridLayout(d->OtherShellOptionsGroup);
    d->LookupUserGroupsCheck = new QCheckBox(QStringLiteral("Lookup &user groups"));
    d->LookupUserGroupsCheck->setTristate(true);
    d->ClearAliasesCheck = new QCheckBox(QStringLiteral("Clear a&liases"));
    d->UnsetNationalVarsCheck = new QCheckBox(QStringLiteral("Clear &national variables"));
    d->Scp1CompatibilityCheck = new QCheckBox(QStringLiteral("Use scp&2 with scp1 compatibility"));
    Grid3->addWidget(d->LookupUserGroupsCheck, 0, 0);
    Grid3->addWidget(d->UnsetNationalVarsCheck, 0, 1);
    Grid3->addWidget(d->ClearAliasesCheck, 1, 0);
    Grid3->addWidget(d->Scp1CompatibilityCheck, 1, 1);
    L->addWidget(d->OtherShellOptionsGroup);
    L->addStretch();
  }

  // ---- Environment > FTP ----
  d->FtpSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Protocol options"));
    auto * Grid = new QGridLayout(G);
    d->PostLoginCommandsMemo = new QPlainTextEdit();
    d->PostLoginCommandsMemo->setMaximumHeight(70);
    Grid->addWidget(Buddy(QStringLiteral("Post login &commands:"), d->PostLoginCommandsMemo), 0, 0, 1, 2);
    Grid->addWidget(d->PostLoginCommandsMemo, 1, 0, 1, 2);
    d->FtpListAllCombo = AutoSwitchCombo();
    d->FtpListAllLabel = Buddy(QStringLiteral("&Support for listing of hidden files:"), d->FtpListAllCombo);
    Grid->addWidget(d->FtpListAllLabel, 2, 0);
    Grid->addWidget(d->FtpListAllCombo, 2, 1);
    d->FtpUseMlsdCombo = AutoSwitchCombo();
    Grid->addWidget(Buddy(QStringLiteral("Use &MLSD command for directory listing"), d->FtpUseMlsdCombo), 3, 0);
    Grid->addWidget(d->FtpUseMlsdCombo, 3, 1);
    d->FtpForcePasvIpCombo = AutoSwitchCombo();
    d->FtpForcePasvIpLabel = Buddy(QStringLiteral("&Force IP address for passive mode connections:"), d->FtpForcePasvIpCombo);
    Grid->addWidget(d->FtpForcePasvIpLabel, 4, 0);
    Grid->addWidget(d->FtpForcePasvIpCombo, 4, 1);
    d->FtpHostCombo = AutoSwitchCombo();
    Grid->addWidget(Buddy(QStringLiteral("Use &HOST command to select host on the server"), d->FtpHostCombo), 5, 0);
    Grid->addWidget(d->FtpHostCombo, 5, 1);
    d->FtpAccountEdit = new QLineEdit();
    Grid->addWidget(Buddy(QStringLiteral("&Account:"), d->FtpAccountEdit), 6, 0);
    Grid->addWidget(d->FtpAccountEdit, 6, 1);
    d->VMSAllRevisionsCheck = new QCheckBox(QStringLiteral("Display all file &revisions on VMS servers"));
    Grid->addWidget(d->VMSAllRevisionsCheck, 7, 0, 1, 2);
    L->addWidget(G);
    L->addStretch();
  }

  // ---- Environment > S3 ----
  d->S3Sheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Protocol options"));
    auto * Grid = new QGridLayout(G);
    d->S3DefaultRegionCombo = new QComboBox();
    d->S3DefaultRegionCombo->setEditable(true);
    d->S3DefaultRegionCombo->addItems(QStringLiteral(
      "af-south-1 ap-east-1 ap-east-2 ap-northeast-1 ap-northeast-2 ap-northeast-3 ap-south-1 ap-south-2 "
      "ap-southeast-1 ap-southeast-2 ap-southeast-3 ap-southeast-4 ap-southeast-5 ap-southeast-6 ap-southeast-7 "
      "ca-central-1 ca-west-1 cn-north-1 cn-northwest-1 eu-central-1 eu-central-2 eu-north-1 eu-south-1 eu-south-2 "
      "eu-west-1 eu-west-2 eu-west-3 il-central-1 me-central-1 me-south-1 mx-central-1 sa-east-1 us-east-1 us-east-2 "
      "us-gov-east-1 us-gov-west-1 us-west-1 us-west-2").split(QLatin1Char(' ')));
    Grid->addWidget(Buddy(QStringLiteral("&Default region:"), d->S3DefaultRegionCombo), 0, 0);
    Grid->addWidget(d->S3DefaultRegionCombo, 0, 1);
    d->S3UrlStyleCombo = new QComboBox();
    d->S3UrlStyleCombo->addItems({QStringLiteral("Virtual Host"), QStringLiteral("Path")});
    Grid->addWidget(Buddy(QStringLiteral("&URL style:"), d->S3UrlStyleCombo), 1, 0);
    Grid->addWidget(d->S3UrlStyleCombo, 1, 1);
    d->S3RequesterPaysCheck = new QCheckBox(QStringLiteral("Requester &pays"));
    Grid->addWidget(d->S3RequesterPaysCheck, 2, 0, 1, 2);
    L->addWidget(G);
    auto * G2 = new QGroupBox(QStringLiteral("Authentication"));
    auto * V2 = new QVBoxLayout(G2);
    d->S3SessionTokenMemo = new QPlainTextEdit();
    d->S3SessionTokenMemo->setMaximumHeight(70);
    V2->addWidget(Buddy(QStringLiteral("&Session token:"), d->S3SessionTokenMemo));
    V2->addWidget(d->S3SessionTokenMemo);
    d->S3RoleArnEdit = new QLineEdit();
    V2->addWidget(Buddy(QStringLiteral("&Role ARN:"), d->S3RoleArnEdit));
    V2->addWidget(d->S3RoleArnEdit);
    L->addWidget(G2);
    L->addStretch();
  }

  // ---- Environment > WebDAV ----
  d->WebDavSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Protocol options"));
    auto * V = new QVBoxLayout(G);
    d->WebDavLiberalEscapingCheck = new QCheckBox(QStringLiteral("&Tolerate non-encoded special characters in filenames"));
    V->addWidget(d->WebDavLiberalEscapingCheck);
    L->addWidget(G);
    L->addStretch();
  }

  // ---- Connection ----
  d->ConnSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Connection"));
    auto * V = new QVBoxLayout(G);
    d->FtpPasvModeCheck = new QCheckBox(QStringLiteral("&Passive mode"));
    d->BufferSizeCheck = new QCheckBox(QStringLiteral("Optimize connection &buffer size"));
    V->addWidget(d->FtpPasvModeCheck);
    V->addWidget(d->BufferSizeCheck);
    L->addWidget(G);

    auto * TG = new QGroupBox(QStringLiteral("Timeouts"));
    auto * TRow = new QHBoxLayout(TG);
    d->TimeoutEdit = new QSpinBox();
    d->TimeoutEdit->setRange(5, 6000);
    d->TimeoutEdit->setSingleStep(5);
    TRow->addWidget(Buddy(QStringLiteral("Server &response timeout:"), d->TimeoutEdit));
    TRow->addWidget(d->TimeoutEdit);
    TRow->addWidget(new QLabel(QStringLiteral("seconds")));
    TRow->addStretch();
    L->addWidget(TG);

    d->PingGroup = new QGroupBox(QStringLiteral("Keepalives"));
    auto * PV = new QVBoxLayout(d->PingGroup);
    d->PingOffButton = new QRadioButton(QStringLiteral("&Off"));
    d->PingNullPacketButton = new QRadioButton(QStringLiteral("Sending of &null SSH packets"));
    d->PingDummyCommandButton = new QRadioButton(QStringLiteral("Executing &dummy protocol commands"));
    PV->addWidget(d->PingOffButton);
    PV->addWidget(d->PingNullPacketButton);
    PV->addWidget(d->PingDummyCommandButton);
    d->PingIntervalSecEdit = new QSpinBox();
    d->PingIntervalSecEdit->setRange(1, 3600);
    d->PingIntervalLabel = Buddy(QStringLiteral("Seconds &between keepalives:"), d->PingIntervalSecEdit);
    auto * PRow = new QHBoxLayout();
    PRow->addWidget(d->PingIntervalLabel);
    PRow->addWidget(d->PingIntervalSecEdit);
    PRow->addStretch();
    PV->addLayout(PRow);
    L->addWidget(d->PingGroup);

    d->FtpPingGroup = new QGroupBox(QStringLiteral("Keepalives"));
    auto * FV = new QVBoxLayout(d->FtpPingGroup);
    d->FtpPingOffButton = new QRadioButton(QStringLiteral("&Off"));
    d->FtpPingDummyCommandButton = new QRadioButton(QStringLiteral("Executing &dummy protocol commands"));
    d->FtpPingDirectoryListingButton = new QRadioButton(QStringLiteral("&And additionally reading the current directory"));
    FV->addWidget(d->FtpPingOffButton);
    FV->addWidget(d->FtpPingDummyCommandButton);
    FV->addWidget(d->FtpPingDirectoryListingButton);
    d->FtpPingIntervalSecEdit = new QSpinBox();
    d->FtpPingIntervalSecEdit->setRange(1, 3600);
    d->FtpPingIntervalLabel = Buddy(QStringLiteral("Seconds &between keepalives:"), d->FtpPingIntervalSecEdit);
    auto * FRow = new QHBoxLayout();
    FRow->addWidget(d->FtpPingIntervalLabel);
    FRow->addWidget(d->FtpPingIntervalSecEdit);
    FRow->addStretch();
    FV->addLayout(FRow);
    L->addWidget(d->FtpPingGroup);

    d->IPvGroup = new QGroupBox(QStringLiteral("Internet protocol version"));
    auto * IRow = new QHBoxLayout(d->IPvGroup);
    d->IPAutoButton = new QRadioButton(QStringLiteral("A&uto"));
    d->IPv4Button = new QRadioButton(QStringLiteral("IPv&4"));
    d->IPv6Button = new QRadioButton(QStringLiteral("IPv&6"));
    IRow->addWidget(d->IPAutoButton);
    IRow->addWidget(d->IPv4Button);
    IRow->addWidget(d->IPv6Button);
    IRow->addStretch();
    L->addWidget(d->IPvGroup);
    L->addStretch();
  }

  // ---- Connection > Proxy ----
  d->ProxySheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Proxy"));
    auto * Grid = new QGridLayout(G);
    d->ProxyMethodCombo = new QComboBox();
    if (FData.IsSsh())
    {
      d->ProxyMethodCombo->addItems({QStringLiteral("None"), QStringLiteral("SOCKS4"), QStringLiteral("SOCKS5"),
        QStringLiteral("HTTP"), QStringLiteral("Telnet"), QStringLiteral("Local")});
#ifdef Q_OS_WIN
      // libssh for Windows has no proxy command: a site could not connect.
      // Disabled, not removed, so a site that has it still shows what it has
      if (auto * Items = qobject_cast<QStandardItemModel *>(d->ProxyMethodCombo->model()))
      {
        Items->item(pmCmd)->setEnabled(false);
      }
#endif
    }
    else
    {
      // FtpProxyMethodCombo/NeonProxyMethodCombo
      d->ProxyMethodCombo->addItems({QStringLiteral("None"), QStringLiteral("SOCKS4"), QStringLiteral("SOCKS5"), QStringLiteral("HTTP")});
      if (FData.FSProtocol == fsFTP)
      {
        // SessionData::FtpProxyLogonType 1.. after the proxy methods
        d->ProxyMethodCombo->addItems({QStringLiteral("SITE %host"), QStringLiteral("USER %proxyuser, USER %user@%host"),
          QStringLiteral("OPEN %host"), QStringLiteral("USER %proxyuser, USER %user"), QStringLiteral("USER %user@%host"),
          QStringLiteral("USER %proxyuser@%host"), QStringLiteral("USER %user@%host %proxyuser"),
          QStringLiteral("USER %user@%proxyuser@%host")});
      }
    }
    Grid->addWidget(Buddy(QStringLiteral("Proxy &type:"), d->ProxyMethodCombo), 0, 0, 1, 2);
    Grid->addWidget(d->ProxyMethodCombo, 1, 0);
    // ProxyAutodetectButtonClick
    d->ProxyAutodetectButton = new QPushButton(QStringLiteral("&Autodetect"));
    Grid->addWidget(d->ProxyAutodetectButton, 1, 1);
    connect(d->ProxyAutodetectButton, &QPushButton::clicked, this, [this]()
    {
      TProxyMethod Method = pmHTTP;
      QString Host;
      int Port = 0;
      if (SecureShell::AutodetectProxy(Method, Host, Port))
      {
        d->ProxyHostEdit->setText(Host);
        d->ProxyPortEdit->setValue(Port);
        d->ProxyMethodCombo->setCurrentIndex(static_cast<int>(Method));
        UpdateControls();
      }
    });
    d->ProxyHostEdit = new QLineEdit();
    d->ProxyHostLabel = Buddy(QStringLiteral("Pro&xy hostname:"), d->ProxyHostEdit);
    d->ProxyPortEdit = new QSpinBox();
    d->ProxyPortEdit->setRange(1, 65535);
    d->ProxyPortLabel = Buddy(QStringLiteral("Po&rt number:"), d->ProxyPortEdit);
    Grid->addWidget(d->ProxyHostLabel, 2, 0);
    Grid->addWidget(d->ProxyPortLabel, 2, 1);
    Grid->addWidget(d->ProxyHostEdit, 3, 0);
    Grid->addWidget(d->ProxyPortEdit, 3, 1);
    d->ProxyUsernameEdit = new QLineEdit();
    d->ProxyUsernameLabel = Buddy(QStringLiteral("&Username:"), d->ProxyUsernameEdit);
    d->ProxyPasswordEdit = new QLineEdit();
    d->ProxyPasswordEdit->setEchoMode(QLineEdit::Password);
    d->ProxyPasswordLabel = Buddy(QStringLiteral("&Password:"), d->ProxyPasswordEdit);
    Grid->addWidget(d->ProxyUsernameLabel, 4, 0);
    Grid->addWidget(d->ProxyPasswordLabel, 4, 1);
    Grid->addWidget(d->ProxyUsernameEdit, 5, 0);
    Grid->addWidget(d->ProxyPasswordEdit, 5, 1);
    Grid->setColumnStretch(0, 1);
    L->addWidget(G);

    d->ProxySettingsGroup = new QGroupBox(QStringLiteral("Proxy settings"));
    auto * Grid2 = new QGridLayout(d->ProxySettingsGroup);
    d->ProxyTelnetCommandEdit = new QLineEdit();
    d->ProxyTelnetCommandLabel = Buddy(QStringLiteral("Telnet co&mmand:"), d->ProxyTelnetCommandEdit);
    d->ProxyLocalCommandEdit = new QLineEdit();
    d->ProxyLocalCommandLabel = Buddy(QStringLiteral("Local proxy co&mmand:"), d->ProxyLocalCommandEdit);
    d->ProxyLocalCommandBrowseButton = new QPushButton(QStringLiteral("&Browse..."));
    Grid2->addWidget(d->ProxyTelnetCommandLabel, 0, 0, 1, 2);
    Grid2->addWidget(d->ProxyTelnetCommandEdit, 1, 0, 1, 2);
    Grid2->addWidget(d->ProxyLocalCommandLabel, 2, 0, 1, 2);
    Grid2->addWidget(d->ProxyLocalCommandEdit, 3, 0);
    Grid2->addWidget(d->ProxyLocalCommandBrowseButton, 3, 1);
    auto * Hint = new QLabel(QStringLiteral("Patterns: %host, %port, %user, %pass, %proxyhost, %proxyport, \\n, \\r, \\t, \\\\"));
    Hint->setWordWrap(true);
    Hint->setEnabled(false);
    Grid2->addWidget(Hint, 4, 0, 1, 2);
    d->ProxyLocalhostCheck = new QCheckBox(QStringLiteral("Co&nsider proxying local host connections"));
    Grid2->addWidget(d->ProxyLocalhostCheck, 5, 0, 1, 2);
    d->ProxyDNSCombo = new QComboBox();
    d->ProxyDNSCombo->addItems({QStringLiteral("Auto"), QStringLiteral("No"), QStringLiteral("Yes")});
    auto * DnsRow = new QHBoxLayout();
    DnsRow->addWidget(Buddy(QStringLiteral("Do &DNS name lookup at proxy end:"), d->ProxyDNSCombo));
    DnsRow->addWidget(d->ProxyDNSCombo);
    DnsRow->addStretch();
    Grid2->addLayout(DnsRow, 6, 0, 1, 2);
    L->addWidget(d->ProxySettingsGroup);
    L->addStretch();
    connect(d->ProxyLocalCommandBrowseButton, &QPushButton::clicked, this, [this]()
    {
      QString File = QFileDialog::getOpenFileName(this, QString(), QDir::homePath());
      if (!File.isEmpty())
      {
        d->ProxyLocalCommandEdit->setText(File);
      }
    });
  }

  // ---- Connection > Tunnel ----
  d->TunnelSheet = NewSheet(L);
  {
    d->TunnelCheck = new QCheckBox(QStringLiteral("Connect &through SSH tunnel"));
    L->addWidget(d->TunnelCheck);
    d->TunnelSessionGroup = new QGroupBox(QStringLiteral("Host to setup tunnel on"));
    auto * Grid = new QGridLayout(d->TunnelSessionGroup);
    d->TunnelHostNameEdit = new QLineEdit();
    d->TunnelPortNumberEdit = new QSpinBox();
    d->TunnelPortNumberEdit->setRange(1, 65535);
    d->TunnelUserNameEdit = new QLineEdit();
    d->TunnelPasswordEdit = new QLineEdit();
    d->TunnelPasswordEdit->setEchoMode(QLineEdit::Password);
    Grid->addWidget(Buddy(QStringLiteral("&Hostname:"), d->TunnelHostNameEdit), 0, 0);
    Grid->addWidget(Buddy(QStringLiteral("Po&rt number:"), d->TunnelPortNumberEdit), 0, 1);
    Grid->addWidget(d->TunnelHostNameEdit, 1, 0);
    Grid->addWidget(d->TunnelPortNumberEdit, 1, 1);
    Grid->addWidget(Buddy(QStringLiteral("&Username:"), d->TunnelUserNameEdit), 2, 0);
    Grid->addWidget(Buddy(QStringLiteral("&Password:"), d->TunnelPasswordEdit), 2, 1);
    Grid->addWidget(d->TunnelUserNameEdit, 3, 0);
    Grid->addWidget(d->TunnelPasswordEdit, 3, 1);
    Grid->setColumnStretch(0, 1);
    L->addWidget(d->TunnelSessionGroup);

    d->TunnelAuthenticationParamsGroup = new QGroupBox(QStringLiteral("Tunnel authentication parameters"));
    auto * AV = new QVBoxLayout(d->TunnelAuthenticationParamsGroup);
    d->TunnelPrivateKeyEdit = new QLineEdit();
    AV->addWidget(Buddy(QStringLiteral("Private &key file:"), d->TunnelPrivateKeyEdit));
    AV->addWidget(FileEdit(d->TunnelPrivateKeyEdit, this, QStringLiteral("Select private key file"), PrivateKeyFilter));
    L->addWidget(d->TunnelAuthenticationParamsGroup);
    L->addStretch();
  }

  // ---- Connection > TLS/SSL ----
  d->SslSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("TLS options"));
    auto * Grid = new QGridLayout(G);
    QStringList Versions = {QStringLiteral("TLS 1.0"), QStringLiteral("TLS 1.1"), QStringLiteral("TLS 1.2"), QStringLiteral("TLS 1.3")};
    d->MinTlsVersionCombo = new QComboBox();
    d->MinTlsVersionCombo->addItems(Versions);
    d->MaxTlsVersionCombo = new QComboBox();
    d->MaxTlsVersionCombo->addItems(Versions);
    Grid->addWidget(Buddy(QStringLiteral("Mi&nimum TLS version:"), d->MinTlsVersionCombo), 0, 0);
    Grid->addWidget(d->MinTlsVersionCombo, 0, 1);
    Grid->addWidget(Buddy(QStringLiteral("Ma&ximum TLS version:"), d->MaxTlsVersionCombo), 1, 0);
    Grid->addWidget(d->MaxTlsVersionCombo, 1, 1);
    d->SslSessionReuseCheck = new QCheckBox(QStringLiteral("&Reuse TLS session ID for data connections"));
    Grid->addWidget(d->SslSessionReuseCheck, 2, 0, 1, 2);
    L->addWidget(G);
    d->TlsAuthenticationGroup = new QGroupBox(QStringLiteral("Authentication parameters"));
    auto * AV = new QVBoxLayout(d->TlsAuthenticationGroup);
    d->TlsCertificateFileEdit = new QLineEdit();
    AV->addWidget(Buddy(QStringLiteral("&Client certificate file:"), d->TlsCertificateFileEdit));
    AV->addWidget(FileEdit(d->TlsCertificateFileEdit, this, QString(),
      QStringLiteral("Certificate Files (*.pfx *.p12 *.key *.pem);;All Files (*)")));
    L->addWidget(d->TlsAuthenticationGroup);
    L->addStretch();
  }

  // ---- SSH ----
  d->AdvancedSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Protocol options"));
    auto * V = new QVBoxLayout(G);
    d->CompressionCheck = new QCheckBox(QStringLiteral("&Enable compression"));
    V->addWidget(d->CompressionCheck);
    L->addWidget(G);
    auto * G2 = new QGroupBox(QStringLiteral("Encryption options"));
    auto * Grid = new QGridLayout(G2);
    d->CipherListBox = new QListWidget();
    Grid->addWidget(Buddy(QStringLiteral("Encryption cipher selection &policy:"), d->CipherListBox), 0, 0, 1, 2);
    Grid->addWidget(d->CipherListBox, 1, 0, 3, 1);
    d->CipherUpButton = new QPushButton(QStringLiteral("&Up"));
    d->CipherDownButton = new QPushButton(QStringLiteral("&Down"));
    Grid->addWidget(d->CipherUpButton, 1, 1);
    Grid->addWidget(d->CipherDownButton, 2, 1);
    Grid->setRowStretch(3, 1);
    L->addWidget(G2);
    L->addStretch();
  }

  // ---- SSH > Key exchange ----
  d->KexSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Key exchange algorithm options"));
    auto * Grid = new QGridLayout(G);
    d->KexListBox = new QListWidget();
    Grid->addWidget(Buddy(QStringLiteral("Algorithm selection &policy:"), d->KexListBox), 0, 0, 1, 2);
    Grid->addWidget(d->KexListBox, 1, 0, 3, 1);
    d->KexUpButton = new QPushButton(QStringLiteral("&Up"));
    d->KexDownButton = new QPushButton(QStringLiteral("&Down"));
    Grid->addWidget(d->KexUpButton, 1, 1);
    Grid->addWidget(d->KexDownButton, 2, 1);
    Grid->setRowStretch(3, 1);
    L->addWidget(G);
    d->KexReexchangeGroup = new QGroupBox(QStringLiteral("Options controlling key re-exchange"));
    auto * Grid2 = new QGridLayout(d->KexReexchangeGroup);
    // Only by data: a time limit slows every request down in libssh (SecureShell.cpp)
    d->RekeyDataEdit = new QLineEdit();
    Grid2->addWidget(Buddy(QStringLiteral("Ma&x data before rekey (0 for no limit):"), d->RekeyDataEdit), 0, 0);
    Grid2->addWidget(d->RekeyDataEdit, 0, 1);
    L->addWidget(d->KexReexchangeGroup);
    L->addStretch();
  }

  // ---- SSH > Authentication ----
  d->AuthSheet = NewSheet(L);
  {
    d->SshNoUserAuthCheck = new QCheckBox(QStringLiteral("&Bypass authentication entirely"));
    L->addWidget(d->SshNoUserAuthCheck);
    d->AuthenticationGroup = new QGroupBox(QStringLiteral("Authentication options"));
    auto * V = new QVBoxLayout(d->AuthenticationGroup);
    d->TryAgentCheck = new QCheckBox(QStringLiteral("&Attempt authentication using agent"));
    d->AuthKICheck = new QCheckBox(QStringLiteral("Attempt 'keyboard-&interactive' authentication"));
    d->AuthKIPasswordCheck = new QCheckBox(QStringLiteral("Respond with a pass&word to the first prompt"));
    V->addWidget(d->TryAgentCheck);
    V->addWidget(d->AuthKICheck);
    V->addLayout(Indent(d->AuthKIPasswordCheck));
    L->addWidget(d->AuthenticationGroup);

    d->AuthenticationParamsGroup = new QGroupBox(QStringLiteral("Authentication parameters"));
    auto * PV = new QVBoxLayout(d->AuthenticationParamsGroup);
    d->AgentFwdCheck = new QCheckBox(QStringLiteral("Allow agent &forwarding"));
    PV->addWidget(d->AgentFwdCheck);
    d->PrivateKeyEdit = new QLineEdit();
    PV->addWidget(Buddy(QStringLiteral("Private &key file:"), d->PrivateKeyEdit));
    PV->addWidget(FileEdit(d->PrivateKeyEdit, this, QStringLiteral("Select private key file"), PrivateKeyFilter));
    {
      // PrivateKeyViewButton, PrivateKeyToolsButton (+ PrivateKeyMenu)
      auto * KeyButtons = new QHBoxLayout();
      d->PrivateKeyViewButton = new QPushButton(QStringLiteral("&Display Public Key"));
      d->PrivateKeyToolsButton = new QPushButton(QStringLiteral("&Tools"));
      auto * KeyMenu = new QMenu(d->PrivateKeyToolsButton);
      QAction * UploadItem = KeyMenu->addAction(QStringLiteral("&Install Public Key into Server..."));
      connect(KeyMenu, &QMenu::aboutToShow, this, [this, UploadItem]()
      {
        // PrivateKeyToolsButtonClick: SFTP sites only
        SessionData Data = FData;
        SaveSession(Data);
        UploadItem->setEnabled(Data.CanLogin() && ((Data.FSProtocol == fsSFTP) || (Data.FSProtocol == fsSFTPonly)));
      });
      connect(UploadItem, &QAction::triggered, this, [this]() { PrivateKeyUploadItemClick(); });
      d->PrivateKeyToolsButton->setMenu(KeyMenu);
      connect(d->PrivateKeyViewButton, &QPushButton::clicked, this, [this]() { PrivateKeyViewButtonClick(); });
      KeyButtons->addWidget(d->PrivateKeyViewButton);
      KeyButtons->addWidget(d->PrivateKeyToolsButton);
      KeyButtons->addStretch();
      PV->addLayout(KeyButtons);
    }
    d->DetachedCertificateEdit = new QLineEdit();
    d->DetachedCertificateLabel = Buddy(QStringLiteral("Certificate to &use with the private key:"), d->DetachedCertificateEdit);
    PV->addWidget(d->DetachedCertificateLabel);
    PV->addWidget(FileEdit(d->DetachedCertificateEdit, this, QString(),
      QStringLiteral("Public Key Files (*.pub);;All Files (*)")));
    L->addWidget(d->AuthenticationParamsGroup);

    auto * G = new QGroupBox(QStringLiteral("GSSAPI"));
    auto * GV = new QVBoxLayout(G);
    d->AuthGSSAPICheck = new QCheckBox(QStringLiteral("Attempt &GSSAPI authentication"));
    d->GSSAPIFwdTGTCheck = new QCheckBox(QStringLiteral("Allow GSSAPI c&redential delegation"));
    GV->addWidget(d->AuthGSSAPICheck);
    GV->addLayout(Indent(d->GSSAPIFwdTGTCheck));
    L->addWidget(G);
    L->addStretch();
  }

  // ---- SSH > Bugs ----
  d->BugsSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Detection of known bugs in SSH servers"));
    auto * Grid = new QGridLayout(G);
    // Only the workarounds that are possible with libssh; the stored values
    // of the others are kept
    struct { TSshBug Bug; const char * Caption; } Bugs[] = {
      { sbRekey2, "Handles SSH &key re-exchange badly:" },
      { sbIgnore2, "Chokes on SSH i&gnore messages:" },
    };
    int Row = 0;
    for (const auto & B : Bugs)
    {
      d->BugCombos[B.Bug] = AutoSwitchCombo();
      Grid->addWidget(Buddy(QString::fromLatin1(B.Caption), d->BugCombos[B.Bug]), Row, 0);
      Grid->addWidget(d->BugCombos[B.Bug], Row, 1);
      Row++;
    }
    L->addWidget(G);
    L->addStretch();
  }

  // ---- Note ----
  d->NoteSheet = NewSheet(L);
  {
    auto * G = new QGroupBox(QStringLiteral("Note"));
    auto * V = new QVBoxLayout(G);
    d->NoteMemo = new QPlainTextEdit();
    V->addWidget(d->NoteMemo);
    L->addWidget(G);
  }

  // Page order of SiteAdvanced.dfm (navigation tree follows it)
  d->Pages = {
    {QStringLiteral("Environment"), d->EnvironmentSheet, false, true},
    {QStringLiteral("Directories"), d->DirectoriesSheet, true, true},
    {QStringLiteral("Recycle bin"), d->RecycleBinSheet, true, true},
    {QStringLiteral("SFTP"), d->SftpSheet, true, true},
    {QStringLiteral("SCP/Shell"), d->ScpSheet, true, true},
    {QStringLiteral("FTP"), d->FtpSheet, true, true},
    {QStringLiteral("S3"), d->S3Sheet, true, true},
    {QStringLiteral("WebDAV"), d->WebDavSheet, true, true},
    {QStringLiteral("Connection"), d->ConnSheet, false, true},
    {QStringLiteral("Proxy"), d->ProxySheet, true, true},
    {QStringLiteral("Tunnel"), d->TunnelSheet, true, true},
    {QStringLiteral("TLS/SSL"), d->SslSheet, true, true},
    {QStringLiteral("SSH"), d->AdvancedSheet, false, true},
    {QStringLiteral("Key exchange"), d->KexSheet, true, true},
    {QStringLiteral("Authentication"), d->AuthSheet, true, true},
    {QStringLiteral("Bugs"), d->BugsSheet, true, true},
    {QStringLiteral("Note"), d->NoteSheet, false, true},
  };

  // HelpKeyword of the TTabSheets of SiteAdvanced.dfm
  d->EnvironmentSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_environment"));
  d->DirectoriesSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_directories"));
  d->RecycleBinSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_recycle_bin"));
  d->SftpSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_sftp"));
  d->ScpSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_scp"));
  d->FtpSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_ftp"));
  d->S3Sheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_s3"));
  d->WebDavSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_webdav"));
  d->ConnSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_connection"));
  d->ProxySheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_proxy"));
  d->TunnelSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_tunnel"));
  d->SslSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_tls"));
  d->AdvancedSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_ssh"));
  d->KexSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_kex"));
  d->AuthSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_authentication"));
  d->BugsSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_bugs"));
  d->NoteSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_login_note"));

  d->NavigationTree = new QTreeWidget();
  d->NavigationTree->setHeaderHidden(true);
  d->NavigationTree->setRootIsDecorated(false);
  d->NavigationTree->setIndentation(14);
  d->NavigationTree->setFixedWidth(150);
  d->PageControl = new QStackedWidget();
  for (const auto & Page : d->Pages)
  {
    d->PageControl->addWidget(Page.Sheet);
  }
  connect(d->NavigationTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem * Item)
  {
    if (Item != nullptr)
    {
      d->PageControl->setCurrentIndex(Item->data(0, Qt::UserRole).toInt());
    }
  });

  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help);
  // Help and F1 open the help of the current page (FormHelp -> the active sheet's HelpKeyword)
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_login_advanced"), Buttons->button(QDialogButtonBox::Help));
  auto UpdateHelpKeyword = [this]()
  {
    QWidget * Page = d->PageControl->currentWidget();
    QString Keyword = (Page != nullptr) ? Page->property("PageHelpKeyword").toString() : QString();
    HelpSystem::SetFormHelpKeyword(this, Keyword.isEmpty() ? QStringLiteral("ui_login_advanced") : Keyword);
  };
  connect(d->PageControl, &QStackedWidget::currentChanged, this, UpdateHelpKeyword);
  UpdateHelpKeyword();
  d->ColorButton = new QPushButton(QStringLiteral("&Color"));
  Buttons->addButton(d->ColorButton, QDialogButtonBox::ActionRole);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  auto * ColorMenu = new QMenu(this);
  connect(ColorMenu, &QMenu::aboutToShow, this, [this, ColorMenu]()
  {
    PopulateSessionColorMenu(ColorMenu, d->Color, [this](int Color)
    {
      d->Color = Color;
      UpdateControls();
    });
  });
  d->ColorButton->setMenu(ColorMenu);

  auto * Body = new QHBoxLayout();
  Body->addWidget(d->NavigationTree);
  Body->addWidget(d->PageControl, 1);
  auto * Layout = new QVBoxLayout(this);
  Layout->addLayout(Body, 1);
  Layout->addWidget(Buttons);

  // Change handlers -> UpdateControls
  auto Update = [this]() { UpdateControls(); };
  for (QCheckBox * C : findChildren<QCheckBox *>())
  {
    connect(C, &QCheckBox::toggled, this, Update);
  }
  for (QRadioButton * R : findChildren<QRadioButton *>())
  {
    connect(R, &QRadioButton::toggled, this, Update);
  }
  for (QComboBox * C : findChildren<QComboBox *>())
  {
    connect(C, &QComboBox::currentIndexChanged, this, Update);
  }
  for (QLineEdit * E : {d->ProxyTelnetCommandEdit, d->ProxyLocalCommandEdit, d->PrivateKeyEdit})
  {
    connect(E, &QLineEdit::textChanged, this, Update);
  }
  connect(d->CipherListBox, &QListWidget::currentRowChanged, this, Update);
  connect(d->KexListBox, &QListWidget::currentRowChanged, this, Update);
  auto MoveItem = [this](QListWidget * List, int Delta)
  {
    int Row = List->currentRow();
    int Target = Row + Delta;
    if ((Row >= 0) && (Target >= 0) && (Target < List->count()))
    {
      QListWidgetItem * Item = List->takeItem(Row);
      List->insertItem(Target, Item);
      List->setCurrentRow(Target);
    }
    UpdateControls();
  };
  connect(d->CipherUpButton, &QPushButton::clicked, this, [this, MoveItem]() { MoveItem(d->CipherListBox, -1); });
  connect(d->CipherDownButton, &QPushButton::clicked, this, [this, MoveItem]() { MoveItem(d->CipherListBox, 1); });
  connect(d->KexUpButton, &QPushButton::clicked, this, [this, MoveItem]() { MoveItem(d->KexListBox, -1); });
  connect(d->KexDownButton, &QPushButton::clicked, this, [this, MoveItem]() { MoveItem(d->KexListBox, 1); });

  // Ctrl+Tab / Ctrl+Shift+Tab cycle the pages (TPageControl)
  auto * NextPage = new QShortcut(QKeySequence::NextChild, this);
  connect(NextPage, &QShortcut::activated, this, [this]() { CyclePage(true); });
  auto * PrevPage = new QShortcut(QKeySequence::PreviousChild, this);
  connect(PrevPage, &QShortcut::activated, this, [this]() { CyclePage(false); });
  auto * PrevPage2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this);
  connect(PrevPage2, &QShortcut::activated, this, [this]() { CyclePage(false); });

  LoadSession();
  UpdateNavigationTree();
  // WinSCP opens on the first page
  d->NavigationTree->setCurrentItem(d->NavigationTree->topLevelItem(0));
}

SiteAdvancedDialog::~SiteAdvancedDialog()
{
  delete d;
}

void SiteAdvancedDialog::LoadSession()
{
  d->NoUpdate = true;
  const SessionData & S = FData;

  // Directories page
  d->SynchronizeBrowsingCheck->setChecked(S.SynchronizeBrowsing);
  d->LocalDirectoryEdit->setText(S.LocalDirectory);
  d->RemoteDirectoryEdit->setText(S.RemoteDirectory);
  d->UpdateDirectoriesCheck->setChecked(S.UpdateDirectories);
  d->CacheDirectoriesCheck->setChecked(S.CacheDirectories);
  d->CacheDirectoryChangesCheck->setChecked(S.CacheDirectoryChanges);
  d->PreserveDirectoryChangesCheck->setChecked(S.PreserveDirectoryChanges);
  d->ResolveSymlinksCheck->setChecked(S.ResolveSymlinks);
  d->FollowDirectorySymlinksCheck->setChecked(S.FollowDirectorySymlinks);

  // Environment page
  d->EOLTypeCombo->setCurrentIndex((S.EOLType == eolLF) ? 0 : 1);
  switch (S.NotUtf)
  {
    case asOn: d->UtfCombo->setCurrentIndex(1); break;
    case asOff: d->UtfCombo->setCurrentIndex(2); break;
    default: d->UtfCombo->setCurrentIndex(0); break;
  }
  int TimeDifferenceMin = static_cast<int>(std::lround(S.TimeDifference * 24 * 60));
  d->TimeDifferenceEdit->setValue(TimeDifferenceMin / 60);
  d->TimeDifferenceMinutesEdit->setValue(TimeDifferenceMin % 60);
  d->TimeDifferenceAutoCheck->setChecked(S.TimeDifferenceAuto);
  d->TrimVMSVersionsCheck->setChecked(S.TrimVMSVersions);
  d->VMSAllRevisionsCheck->setChecked(S.VMSAllRevisions);

  // Recycle bin page
  d->DeleteToRecycleBinCheck->setChecked(S.DeleteToRecycleBin);
  d->OverwrittenToRecycleBinCheck->setChecked(S.OverwrittenToRecycleBin);
  d->RecycleBinPathEdit->setText(!S.RecycleBinPath.isEmpty() ? S.RecycleBinPath : DefaultRecycleBinPath);

  // SFTP page
  d->SftpServerEdit->setCurrentText(S.SftpServer.isEmpty() ? d->SftpServerEdit->itemText(0) : S.SftpServer);
  d->AllowScpFallbackCheck->setChecked(S.FSProtocol == fsSFTP);
  d->UsePosixRenameCheck->setChecked(S.UsePosixRename);
  ComboAutoSwitchLoad(d->SFTPRealPathCombo, S.SFTPRealPath);
  ComboAutoSwitchLoad(d->SFTPBugSymlinkCombo, S.SFTPBug[sbSymlink]);
  ComboAutoSwitchLoad(d->SFTPBugSignedTSCombo, S.SFTPBug[sbSignedTS]);

  // FTP page
  d->FtpAccountEdit->setText(S.FtpAccount);
  d->PostLoginCommandsMemo->setPlainText(S.PostLoginCommands);
  ComboAutoSwitchLoad(d->FtpListAllCombo, S.FtpListAll);
  ComboAutoSwitchLoad(d->FtpUseMlsdCombo, S.FtpUseMlsd);
  ComboAutoSwitchLoad(d->FtpForcePasvIpCombo, S.FtpForcePasvIp);
  ComboAutoSwitchLoad(d->FtpHostCombo, S.FtpHost);

  // S3 page
  d->S3DefaultRegionCombo->setCurrentText(S.S3DefaultRegion);
  d->S3UrlStyleCombo->setCurrentIndex((S.S3UrlStyle == s3usPath) ? 1 : 0);
  d->S3RequesterPaysCheck->setChecked(S.S3RequesterPays);
  d->S3SessionTokenMemo->setPlainText(S.S3SessionToken);
  d->S3RoleArnEdit->setText(S.S3RoleArn);

  // Authentication page
  d->SshNoUserAuthCheck->setChecked(S.SshNoUserAuth);
  d->TryAgentCheck->setChecked(S.TryAgent);
  d->AuthKICheck->setChecked(S.AuthKI);
  d->AuthKIPasswordCheck->setChecked(S.AuthKIPassword);
  d->AuthGSSAPICheck->setChecked(S.AuthGSSAPI);
  d->GSSAPIFwdTGTCheck->setChecked(S.GSSAPIFwdTGT);
  d->AgentFwdCheck->setChecked(S.AgentFwd);
  d->PrivateKeyEdit->setText(S.PublicKeyFile);
  d->DetachedCertificateEdit->setText(S.DetachedCertificate);

  // SSH page
  d->CompressionCheck->setChecked(S.Compression);
  d->CipherListBox->clear();
  for (const QString & Name : S.Ciphers())
  {
    auto * Item = new QListWidgetItem(CipherDisplayName(Name), d->CipherListBox);
    Item->setData(Qt::UserRole, Name);
  }

  // KEX page
  d->KexListBox->clear();
  for (const QString & Name : S.Kexes())
  {
    auto * Item = new QListWidgetItem(KexDisplayName(Name), d->KexListBox);
    Item->setData(Qt::UserRole, Name);
  }
  d->RekeyDataEdit->setText(S.RekeyData);

  // Connection page
  d->FtpPasvModeCheck->setChecked(S.FtpPasvMode);
  d->BufferSizeCheck->setChecked((S.SendBuf > 0) && S.SshSimple);
  switch (S.PingType)
  {
    case ptNullPacket: d->PingNullPacketButton->setChecked(true); break;
    case ptDummyCommand: d->PingDummyCommandButton->setChecked(true); break;
    default: d->PingOffButton->setChecked(true); break;
  }
  d->PingIntervalSecEdit->setValue(S.PingInterval);
  switch (S.FtpPingType)
  {
    case fptDummyCommand0:
    case fptDummyCommand: d->FtpPingDummyCommandButton->setChecked(true); break;
    case fptDirectoryListing: d->FtpPingDirectoryListingButton->setChecked(true); break;
    default: d->FtpPingOffButton->setChecked(true); break;
  }
  d->FtpPingIntervalSecEdit->setValue(S.FtpPingInterval);
  d->TimeoutEdit->setValue(S.Timeout);
  switch (S.AddressFamily)
  {
    case afIPv4: d->IPv4Button->setChecked(true); break;
    case afIPv6: d->IPv6Button->setChecked(true); break;
    default: d->IPAutoButton->setChecked(true); break;
  }

  // Shell page
  d->ShellEdit->setCurrentText(S.Shell.isEmpty() ? d->ShellEdit->itemText(0) : S.Shell);
  d->ReturnVarEdit->setCurrentText(S.ReturnVar.isEmpty() ? d->ReturnVarEdit->itemText(0) : S.ReturnVar);
  d->ListingCommandEdit->setCurrentText(S.ListingCommand);
  // CheckBoxAutoSwitchLoad
  d->LookupUserGroupsCheck->setCheckState(
    (S.LookupUserGroups == asOn) ? Qt::Checked : ((S.LookupUserGroups == asOff) ? Qt::Unchecked : Qt::PartiallyChecked));
  d->ClearAliasesCheck->setChecked(S.ClearAliases);
  d->IgnoreLsWarningsCheck->setChecked(S.IgnoreLsWarnings);
  d->Scp1CompatibilityCheck->setChecked(S.Scp1Compatibility);
  d->UnsetNationalVarsCheck->setChecked(S.UnsetNationalVars);
  d->SCPLsFullTimeAutoCheck->setChecked(S.SCPLsFullTime != asOff);

  // Proxy page
  // GetFtpProxyLogonType: a logon type rules out a proxy method
  int ProxyIndex = ((S.FSProtocol == fsFTP) && (S.FtpProxyLogonType > 0)) ? (pmHTTP + S.FtpProxyLogonType) : static_cast<int>(S.ProxyMethod);
  d->ProxyMethodCombo->setCurrentIndex(qMin(ProxyIndex, d->ProxyMethodCombo->count() - 1));
  d->ProxyHostEdit->setText(S.ProxyHost);
  d->ProxyPortEdit->setValue(S.ProxyPort);
  d->ProxyUsernameEdit->setText(S.ProxyUsername);
  d->ProxyPasswordEdit->setText(S.ProxyPassword);
  d->ProxyTelnetCommandEdit->setText(S.ProxyTelnetCommand);
  d->ProxyLocalCommandEdit->setText(S.ProxyLocalCommand);
  d->ProxyLocalhostCheck->setChecked(S.ProxyLocalhost);
  d->ProxyDNSCombo->setCurrentIndex(2 - S.ProxyDNS);

  // Bugs page
  for (int Index = 0; Index < sbCount; Index++)
  {
    if (d->BugCombos[Index] != nullptr)
    {
      ComboAutoSwitchLoad(d->BugCombos[Index], S.Bug[Index]);
    }
  }

  // Tunnel page
  d->TunnelCheck->setChecked(S.Tunnel);
  d->TunnelUserNameEdit->setText(S.TunnelUserName);
  d->TunnelPortNumberEdit->setValue(S.TunnelPortNumber);
  d->TunnelHostNameEdit->setText(S.TunnelHostName);
  d->TunnelPasswordEdit->setText(S.TunnelPassword);
  d->TunnelPrivateKeyEdit->setText(S.TunnelPublicKeyFile);

  // TLS/SSL page
  d->MinTlsVersionCombo->setCurrentIndex(TlsVersionToIndex(S.MinTlsVersion));
  d->MaxTlsVersionCombo->setCurrentIndex(TlsVersionToIndex(S.MaxTlsVersion));
  d->SslSessionReuseCheck->setChecked(S.SslSessionReuse);
  d->TlsCertificateFileEdit->setText(S.TlsCertificateFile);

  // Note page
  d->NoteMemo->setPlainText(S.Note);

  // WebDAV page
  d->WebDavLiberalEscapingCheck->setChecked(S.WebDavLiberalEscaping);

  d->Color = S.Color;
  d->NoUpdate = false;
  UpdateControls();
}

void SiteAdvancedDialog::SaveSession(SessionData & S)
{
  // SSH page
  S.Compression = d->CompressionCheck->isChecked();
  QStringList Ciphers;
  for (int Index = 0; Index < d->CipherListBox->count(); Index++)
  {
    Ciphers.append(d->CipherListBox->item(Index)->data(Qt::UserRole).toString());
  }
  S.CipherList = Ciphers.join(QLatin1Char(','));

  // Kex page
  QStringList Kexes;
  for (int Index = 0; Index < d->KexListBox->count(); Index++)
  {
    Kexes.append(d->KexListBox->item(Index)->data(Qt::UserRole).toString());
  }
  S.KexList = Kexes.join(QLatin1Char(','));
  S.RekeyData = d->RekeyDataEdit->text().trimmed();

  // Authentication page
  S.SshNoUserAuth = d->SshNoUserAuthCheck->isChecked();
  S.TryAgent = d->TryAgentCheck->isChecked();
  S.AuthKI = d->AuthKICheck->isChecked();
  S.AuthKIPassword = d->AuthKIPasswordCheck->isChecked();
  S.AuthGSSAPI = d->AuthGSSAPICheck->isChecked();
  S.GSSAPIFwdTGT = d->GSSAPIFwdTGTCheck->isChecked();
  S.AgentFwd = d->AgentFwdCheck->isChecked();
  S.PublicKeyFile = d->PrivateKeyEdit->text().trimmed();
  S.DetachedCertificate = d->DetachedCertificateEdit->text().trimmed();

  // Connection page
  S.FtpPasvMode = d->FtpPasvModeCheck->isChecked();
  S.SendBuf = d->BufferSizeCheck->isChecked() ? DefaultSendBuf : 0;
  S.SshSimple = d->BufferSizeCheck->isChecked();
  S.PingType = d->PingNullPacketButton->isChecked() ? ptNullPacket :
    (d->PingDummyCommandButton->isChecked() ? ptDummyCommand : ptOff);
  S.PingInterval = d->PingIntervalSecEdit->value();
  S.FtpPingType = d->FtpPingDummyCommandButton->isChecked() ? fptDummyCommand :
    (d->FtpPingDirectoryListingButton->isChecked() ? fptDirectoryListing : fptOff);
  S.FtpPingInterval = d->FtpPingIntervalSecEdit->value();
  S.Timeout = d->TimeoutEdit->value();
  S.AddressFamily = d->IPv4Button->isChecked() ? afIPv4 : (d->IPv6Button->isChecked() ? afIPv6 : afAuto);

  // Directories page
  S.SynchronizeBrowsing = d->SynchronizeBrowsingCheck->isChecked();
  S.LocalDirectory = d->LocalDirectoryEdit->text().trimmed();
  S.RemoteDirectory = d->RemoteDirectoryEdit->text().trimmed();
  S.UpdateDirectories = d->UpdateDirectoriesCheck->isChecked();
  S.CacheDirectories = d->CacheDirectoriesCheck->isChecked();
  S.CacheDirectoryChanges = d->CacheDirectoryChangesCheck->isChecked();
  S.PreserveDirectoryChanges = d->PreserveDirectoryChangesCheck->isChecked();
  S.ResolveSymlinks = d->ResolveSymlinksCheck->isChecked();
  S.FollowDirectorySymlinks = d->FollowDirectorySymlinksCheck->isChecked();

  // Environment page
  S.EOLType = (d->EOLTypeCombo->currentIndex() == 0) ? eolLF : eolCRLF;
  switch (d->UtfCombo->currentIndex())
  {
    case 1: S.NotUtf = asOn; break;
    case 2: S.NotUtf = asOff; break;
    default: S.NotUtf = asAuto; break;
  }
  S.TimeDifference =
    (double(d->TimeDifferenceEdit->value()) / 24) + (double(d->TimeDifferenceMinutesEdit->value()) / (24 * 60));
  S.TimeDifferenceAuto = d->TimeDifferenceAutoCheck->isChecked();
  S.TrimVMSVersions = d->TrimVMSVersionsCheck->isChecked();
  S.VMSAllRevisions = d->VMSAllRevisionsCheck->isChecked();

  // Recycle bin page
  S.DeleteToRecycleBin = d->DeleteToRecycleBinCheck->isChecked();
  S.OverwrittenToRecycleBin = d->OverwrittenToRecycleBinCheck->isChecked();
  S.RecycleBinPath = d->RecycleBinPathEdit->text().trimmed();
  if (S.RecycleBinPath == DefaultRecycleBinPath)
  {
    S.RecycleBinPath.clear();
  }

  // Shell page
  S.Shell = (d->ShellEdit->currentText() == d->ShellEdit->itemText(0)) ? QString() : d->ShellEdit->currentText().trimmed();
  S.ReturnVar = (d->ReturnVarEdit->currentText() == d->ReturnVarEdit->itemText(0)) ? QString() : d->ReturnVarEdit->currentText().trimmed();
  S.ListingCommand = d->ListingCommandEdit->currentText();
  switch (d->LookupUserGroupsCheck->checkState())
  {
    case Qt::Checked: S.LookupUserGroups = asOn; break;
    case Qt::Unchecked: S.LookupUserGroups = asOff; break;
    default: S.LookupUserGroups = asAuto; break;
  }
  S.ClearAliases = d->ClearAliasesCheck->isChecked();
  S.IgnoreLsWarnings = d->IgnoreLsWarningsCheck->isChecked();
  S.Scp1Compatibility = d->Scp1CompatibilityCheck->isChecked();
  S.UnsetNationalVars = d->UnsetNationalVarsCheck->isChecked();
  S.SCPLsFullTime = d->SCPLsFullTimeAutoCheck->isChecked() ? asAuto : asOff;

  // SFTP page
  QString SftpServer = d->SftpServerEdit->currentText().trimmed();
  S.SftpServer = (SftpServer.isEmpty() || (SftpServer == d->SftpServerEdit->itemText(0))) ? QString() : SftpServer;
  if ((S.FSProtocol == fsSFTP) || (S.FSProtocol == fsSFTPonly))
  {
    // AllowScpFallbackCheck
    S.FSProtocol = (d->AllowScpFallbackCheck->isEnabled() && d->AllowScpFallbackCheck->isChecked()) ? fsSFTP : fsSFTPonly;
  }
  S.UsePosixRename = d->UsePosixRenameCheck->isChecked();
  S.SFTPRealPath = ComboAutoSwitchSave(d->SFTPRealPathCombo);
  S.SFTPBug[sbSymlink] = ComboAutoSwitchSave(d->SFTPBugSymlinkCombo);
  S.SFTPBug[sbSignedTS] = ComboAutoSwitchSave(d->SFTPBugSignedTSCombo);

  // FTP page
  S.FtpAccount = d->FtpAccountEdit->text();
  S.PostLoginCommands = d->PostLoginCommandsMemo->toPlainText();
  S.FtpListAll = ComboAutoSwitchSave(d->FtpListAllCombo);
  S.FtpUseMlsd = ComboAutoSwitchSave(d->FtpUseMlsdCombo);
  S.FtpForcePasvIp = ComboAutoSwitchSave(d->FtpForcePasvIpCombo);
  S.FtpHost = ComboAutoSwitchSave(d->FtpHostCombo);

  // S3 page
  S.S3DefaultRegion = d->S3DefaultRegionCombo->currentText().trimmed();
  S.S3UrlStyle = (d->S3UrlStyleCombo->currentIndex() == 1) ? s3usPath : s3usVirtualHost;
  S.S3RequesterPays = d->S3RequesterPaysCheck->isChecked();
  S.S3SessionToken = d->S3SessionTokenMemo->toPlainText().trimmed();
  S.S3RoleArn = d->S3RoleArnEdit->text().trimmed();

  // Proxy page
  int ProxyIndex = d->ProxyMethodCombo->currentIndex();
  bool FtpLogonType = (FData.FSProtocol == fsFTP) && (ProxyIndex > pmHTTP);
  S.ProxyMethod = FtpLogonType ? pmNone : static_cast<TProxyMethod>(ProxyIndex);
  if (FData.FSProtocol == fsFTP)
  {
    S.FtpProxyLogonType = FtpLogonType ? (ProxyIndex - pmHTTP) : 0;
  }
  S.ProxyHost = d->ProxyHostEdit->text().trimmed();
  S.ProxyPort = d->ProxyPortEdit->value();
  S.ProxyUsername = d->ProxyUsernameEdit->text();
  S.ProxyPassword = d->ProxyPasswordEdit->text();
  S.ProxyTelnetCommand = d->ProxyTelnetCommandEdit->text();
  S.ProxyLocalCommand = d->ProxyLocalCommandEdit->text();
  S.ProxyLocalhost = d->ProxyLocalhostCheck->isChecked();
  S.ProxyDNS = static_cast<TAutoSwitch>(2 - d->ProxyDNSCombo->currentIndex());

  // Bugs page
  for (int Index = 0; Index < sbCount; Index++)
  {
    if (d->BugCombos[Index] != nullptr)
    {
      S.Bug[Index] = ComboAutoSwitchSave(d->BugCombos[Index]);
    }
  }

  // Tunnel page
  S.Tunnel = d->TunnelCheck->isChecked();
  S.TunnelUserName = d->TunnelUserNameEdit->text().trimmed();
  S.TunnelPortNumber = d->TunnelPortNumberEdit->value();
  S.TunnelHostName = d->TunnelHostNameEdit->text().trimmed();
  S.TunnelPassword = d->TunnelPasswordEdit->text();
  S.TunnelPublicKeyFile = d->TunnelPrivateKeyEdit->text().trimmed();

  // TLS/SSL page
  S.MinTlsVersion = IndexToTlsVersion(d->MinTlsVersionCombo->currentIndex());
  S.MaxTlsVersion = IndexToTlsVersion(d->MaxTlsVersionCombo->currentIndex());
  S.SslSessionReuse = d->SslSessionReuseCheck->isChecked();
  S.TlsCertificateFile = d->TlsCertificateFileEdit->text().trimmed();

  // Note page
  S.Note = d->NoteMemo->toPlainText();

  // WebDAV page
  S.WebDavLiberalEscaping = d->WebDavLiberalEscapingCheck->isChecked();

  S.Color = d->Color;
}

void SiteAdvancedDialog::UpdateControls()
{
  if (d->NoUpdate)
  {
    return;
  }
  d->NoUpdate = true;

  TFSProtocol Protocol = FData.FSProtocol;
  bool SshProtocol = FData.IsSsh();
  bool SftpProtocol = (Protocol == fsSFTPonly) || (Protocol == fsSFTP);
  bool ScpProtocol = (Protocol == fsSCPonly);
  bool FtpProtocol = (Protocol == fsFTP);
  bool WebDavProtocol = (Protocol == fsWebDAV);
  bool S3Protocol = (Protocol == fsS3);
  bool Ssl = (FtpProtocol || WebDavProtocol || S3Protocol) && (FData.Ftps != ftpsNone);

  auto Enable = [](QWidget * W, bool Enabled) { W->setEnabled(Enabled); };

  // connection sheet
  Enable(d->FtpPasvModeCheck, FtpProtocol);
  if (FtpProtocol && (d->ProxyMethodCombo->currentIndex() != pmNone) && (d->ProxyMethodCombo->currentIndex() <= pmHTTP) &&
      !d->FtpPasvModeCheck->isChecked())
  {
    d->FtpPasvModeCheck->setChecked(true);
    if (isVisible())
    {
      QMessageBox::information(this, windowTitle(), QStringLiteral("Passive mode must be enabled when FTP connection through proxy is selected."));
    }
  }
  // FTP only: for SSH the socket keeps the buffer tuning of the system
  d->BufferSizeCheck->setVisible(FtpProtocol);
  d->PingGroup->setVisible(!FtpProtocol);
  Enable(d->PingGroup, SshProtocol);
  Enable(d->PingIntervalSecEdit, d->PingGroup->isEnabled() && !d->PingOffButton->isChecked());
  Enable(d->PingIntervalLabel, d->PingIntervalSecEdit->isEnabled());
  d->FtpPingGroup->setVisible(FtpProtocol);
  Enable(d->FtpPingIntervalSecEdit, !d->FtpPingOffButton->isChecked());
  Enable(d->FtpPingIntervalLabel, d->FtpPingIntervalSecEdit->isEnabled());
  Enable(d->IPvGroup, SshProtocol || FtpProtocol);
  Enable(d->IPAutoButton, d->IPvGroup->isEnabled() && SshProtocol);

  // ssh/authentication sheet
  d->Pages[14].Enabled = SshProtocol;
  Enable(d->AuthenticationGroup, !d->SshNoUserAuthCheck->isChecked());
  Enable(d->AuthKIPasswordCheck, d->AuthenticationGroup->isEnabled() && d->AuthKICheck->isChecked());
  Enable(d->AuthenticationParamsGroup, d->AuthenticationGroup->isEnabled());
  Enable(d->AgentFwdCheck, d->AuthenticationParamsGroup->isEnabled() && d->TryAgentCheck->isChecked());
  Enable(d->DetachedCertificateEdit->parentWidget(), d->AuthenticationParamsGroup->isEnabled() && !d->PrivateKeyEdit->text().isEmpty());
  Enable(d->DetachedCertificateLabel, d->DetachedCertificateEdit->parentWidget()->isEnabled());
  Enable(d->GSSAPIFwdTGTCheck, d->AuthGSSAPICheck->isEnabled() && d->AuthGSSAPICheck->isChecked());

  // ssh sheet
  d->Pages[12].Enabled = SshProtocol;
  int CipherRow = d->CipherListBox->currentRow();
  Enable(d->CipherUpButton, CipherRow > 0);
  Enable(d->CipherDownButton, (CipherRow >= 0) && (CipherRow < d->CipherListBox->count() - 1));

  // ssh/kex sheet
  d->Pages[13].Enabled = SshProtocol;
  int KexRow = d->KexListBox->currentRow();
  Enable(d->KexUpButton, KexRow > 0);
  Enable(d->KexDownButton, (KexRow >= 0) && (KexRow < d->KexListBox->count() - 1));
  Enable(d->KexReexchangeGroup, d->BugCombos[sbRekey2]->currentIndex() != 2);

  // ssh/bugs sheet
  d->Pages[15].Enabled = SshProtocol;

  // connection/proxy sheet
  bool FtpLogonType = FtpProtocol && (d->ProxyMethodCombo->currentIndex() > pmHTTP);
  TProxyMethod ProxyMethod = FtpLogonType ? pmNone : static_cast<TProxyMethod>(d->ProxyMethodCombo->currentIndex());
  QString ProxyCommand = (ProxyMethod == pmCmd) ? d->ProxyLocalCommandEdit->text() : d->ProxyTelnetCommandEdit->text();
  bool ProxyHostPort = FtpLogonType ||
    (ProxyMethod == pmSocks4) || (ProxyMethod == pmSocks5) || (ProxyMethod == pmHTTP) || (ProxyMethod == pmTelnet);
  Enable(d->ProxyHostEdit, ProxyHostPort ||
    ((ProxyMethod == pmCmd) && ProxyCommand.contains(QStringLiteral("%proxyhost"), Qt::CaseInsensitive)));
  Enable(d->ProxyHostLabel, d->ProxyHostEdit->isEnabled());
  Enable(d->ProxyPortEdit, ProxyHostPort ||
    ((ProxyMethod == pmCmd) && ProxyCommand.contains(QStringLiteral("%proxyport"), Qt::CaseInsensitive)));
  Enable(d->ProxyPortLabel, d->ProxyPortEdit->isEnabled());
  bool CommandProxy = (ProxyMethod == pmTelnet) || (ProxyMethod == pmCmd);
  Enable(d->ProxyUsernameEdit, FtpLogonType ||
    ((ProxyMethod == pmSocks4) && !FtpProtocol) || (ProxyMethod == pmSocks5) || (ProxyMethod == pmHTTP) ||
    (CommandProxy && ProxyCommand.contains(QStringLiteral("%user"), Qt::CaseInsensitive)));
  Enable(d->ProxyUsernameLabel, d->ProxyUsernameEdit->isEnabled());
  Enable(d->ProxyPasswordEdit, FtpLogonType || (ProxyMethod == pmSocks5) || (ProxyMethod == pmHTTP) ||
    (CommandProxy && ProxyCommand.contains(QStringLiteral("%pass"), Qt::CaseInsensitive)));
  Enable(d->ProxyPasswordLabel, d->ProxyPasswordEdit->isEnabled());
  bool ProxySettings = (ProxyMethod != pmNone) && SshProtocol;
  d->ProxySettingsGroup->setVisible(SshProtocol);
  Enable(d->ProxySettingsGroup, ProxySettings);
  Enable(d->ProxyTelnetCommandEdit, ProxySettings && (ProxyMethod == pmTelnet));
  Enable(d->ProxyTelnetCommandLabel, d->ProxyTelnetCommandEdit->isEnabled());
  bool LocalVisible = (ProxyMethod == pmCmd);
  d->ProxyLocalCommandEdit->setVisible(LocalVisible);
  d->ProxyLocalCommandLabel->setVisible(LocalVisible);
  d->ProxyLocalCommandBrowseButton->setVisible(LocalVisible);
  d->ProxyTelnetCommandEdit->setVisible(!LocalVisible);
  d->ProxyTelnetCommandLabel->setVisible(!LocalVisible);

  // environment/directories sheet
  Enable(d->CacheDirectoryChangesCheck, !ScpProtocol || d->CacheDirectoriesCheck->isChecked());
  Enable(d->PreserveDirectoryChangesCheck, d->CacheDirectoryChangesCheck->isEnabled() && d->CacheDirectoryChangesCheck->isChecked());
  Enable(d->ResolveSymlinksCheck, SftpProtocol || ScpProtocol);
  Enable(d->FollowDirectorySymlinksCheck, SftpProtocol || ScpProtocol || FtpProtocol);

  // environment sheet
  Enable(d->EOLTypeCombo, SftpProtocol || ScpProtocol);
  Enable(d->EOLTypeLabel, d->EOLTypeCombo->isEnabled());
  Enable(d->UtfCombo, SftpProtocol || FtpProtocol || ScpProtocol);
  Enable(d->UtfLabel, d->UtfCombo->isEnabled());
  bool FtpProtocolWithMlsdOff = FtpProtocol && (ComboAutoSwitchSave(d->FtpUseMlsdCombo) == asOff);
  Enable(d->TimeDifferenceAutoCheck, FtpProtocolWithMlsdOff);
  Enable(d->TimeDifferenceEdit, (FtpProtocolWithMlsdOff && !d->TimeDifferenceAutoCheck->isChecked()) || ScpProtocol);
  Enable(d->TimeDifferenceLabel, d->TimeDifferenceEdit->isEnabled() || d->TimeDifferenceAutoCheck->isEnabled());
  Enable(d->TimeDifferenceMinutesEdit, d->TimeDifferenceEdit->isEnabled());
  Enable(d->TrimVMSVersionsCheck, !S3Protocol);

  // environment/recycle bin sheet
  Enable(d->OverwrittenToRecycleBinCheck, SftpProtocol);
  Enable(d->RecycleBinPathEdit,
    (d->DeleteToRecycleBinCheck->isEnabled() && d->DeleteToRecycleBinCheck->isChecked()) ||
    (d->OverwrittenToRecycleBinCheck->isEnabled() && d->OverwrittenToRecycleBinCheck->isChecked()));
  Enable(d->RecycleBinPathLabel, d->RecycleBinPathEdit->isEnabled());

  // environment/sftp sheet
  d->Pages[3].Enabled = SftpProtocol;
  QString SftpServer = d->SftpServerEdit->currentText();
  Enable(d->AllowScpFallbackCheck, SftpServer.isEmpty() || (SftpServer == d->SftpServerEdit->itemText(0)));

  // environment/scp/shell
  d->Pages[4].Enabled = SshProtocol;
  d->Pages[4].Caption = ScpProtocol ? QStringLiteral("SCP/Shell") : QStringLiteral("Shell");
  d->ScpLsOptionsGroup->setVisible(ScpProtocol);
  d->OtherShellOptionsGroup->setVisible(ScpProtocol);

  // environment/ftp
  d->Pages[5].Enabled = FtpProtocol;
  Enable(d->FtpListAllCombo, ComboAutoSwitchSave(d->FtpUseMlsdCombo) == asOff);
  Enable(d->FtpListAllLabel, d->FtpListAllCombo->isEnabled());
  Enable(d->FtpForcePasvIpCombo, d->FtpPasvModeCheck->isChecked() && (d->IPAutoButton->isChecked() || d->IPv4Button->isChecked()));
  Enable(d->FtpForcePasvIpLabel, d->FtpForcePasvIpCombo->isEnabled());

  // environment/s3
  d->Pages[6].Enabled = S3Protocol;

  // tunnel sheet
  d->Pages[10].Enabled = SshProtocol;
  Enable(d->TunnelSessionGroup, d->TunnelCheck->isChecked());
  Enable(d->TunnelAuthenticationParamsGroup, d->TunnelSessionGroup->isEnabled());

  // connection/ssl/tls
  d->Pages[11].Enabled = Ssl;
  Enable(d->SslSessionReuseCheck, Ssl && FtpProtocol);
  d->TlsAuthenticationGroup->setVisible(Ssl && (FtpProtocol || WebDavProtocol));

  // environment/webdav
  d->Pages[7].Enabled = WebDavProtocol;

  // color button glyph
  QPixmap Glyph(16, 16);
  Glyph.fill(Qt::transparent);
  if (d->Color != 0)
  {
    QPainter Painter(&Glyph);
    Painter.fillRect(2, 2, 12, 12, QColor(d->Color & 0xFF, (d->Color >> 8) & 0xFF, (d->Color >> 16) & 0xFF));
    Painter.setPen(Qt::black);
    Painter.drawRect(2, 2, 11, 11);
  }
  d->ColorButton->setIcon(QIcon(Glyph));

  // Pages depend on the protocol only (cannot change in this dialog),
  // so the navigation tree is built once, after LoadSession
  d->NoUpdate = false;
}

void SiteAdvancedDialog::UpdateNavigationTree()
{
  // TSiteAdvancedDialog::UpdateNavigationTree: disabled sheets are not listed
  int CurrentPage = d->PageControl->currentIndex();
  const QSignalBlocker Blocker(d->NavigationTree);
  d->NavigationTree->clear();
  QTreeWidgetItem * Parent = nullptr;
  QTreeWidgetItem * Current = nullptr;
  for (int Index = 0; Index < d->Pages.size(); Index++)
  {
    const auto & Page = d->Pages[Index];
    if (!Page.Enabled)
    {
      continue;
    }
    QTreeWidgetItem * Item;
    if (Page.Indented && (Parent != nullptr))
    {
      Item = new QTreeWidgetItem(Parent);
    }
    else
    {
      Item = new QTreeWidgetItem(d->NavigationTree);
      Parent = Item;
    }
    Item->setText(0, Page.Caption);
    Item->setData(0, Qt::UserRole, Index);
    if (Index == CurrentPage)
    {
      Current = Item;
    }
  }
  d->NavigationTree->expandAll();
  if (Current != nullptr)
  {
    d->NavigationTree->setCurrentItem(Current);
  }
  else if (d->NavigationTree->topLevelItemCount() > 0)
  {
    d->NavigationTree->setCurrentItem(d->NavigationTree->topLevelItem(0));
    d->PageControl->setCurrentIndex(d->NavigationTree->topLevelItem(0)->data(0, Qt::UserRole).toInt());
  }
}

void SiteAdvancedDialog::SelectPage(const QString & Caption)
{
  for (QTreeWidgetItemIterator It(d->NavigationTree); *It; ++It)
  {
    if ((*It)->text(0).compare(Caption, Qt::CaseInsensitive) == 0)
    {
      d->NavigationTree->setCurrentItem(*It);
      return;
    }
  }
}

void SiteAdvancedDialog::accept()
{
  SaveSession(FData);
  QDialog::accept();
}

void SiteAdvancedDialog::CyclePage(bool Forward)
{
  QList<QTreeWidgetItem *> Items;
  for (QTreeWidgetItemIterator It(d->NavigationTree); *It; ++It)
  {
    Items.append(*It);
  }
  if (Items.isEmpty())
  {
    return;
  }
  qsizetype Index = Items.indexOf(d->NavigationTree->currentItem());
  Index = Forward ? ((Index + 1) % Items.size()) : ((Index <= 0) ? (Items.size() - 1) : (Index - 1));
  d->NavigationTree->setCurrentItem(Items[Index]);
}

//---------------------------------------------------------------------------
// Session color menu (windows/UserInterface.cpp CreateColorMenu)

static const int ColorCols = 8;
static const int UserColorCount = 8;

// GetStandardSessionColorInfo (TColor = 0x00BBGGRR)
static const struct { const char * Name; int Color; } StandardSessionColors[] = {
  {"Rose", 0xCC99FF}, {"Tan", 0x99CCFF}, {"Light Yellow", 0x99FFFF}, {"Light Green", 0xCCFFCC},
  {"Light Turquoise", 0xFFFFCC}, {"Pale Blue", 0xFFCC99}, {"Lavender", 0xFF99CC}, {"Light Orange", 0xB5D5FB},
  {"Darker White", 0xD8D8D8}, {"Darker Tan", 0x97BDC4}, {"Lighter Blue", 0xE2B38D}, {"Light Blue", 0xE4CCB8},
  {"Lighter Red", 0xB7B9E5}, {"Light Olive Green", 0xBCE3D7}, {"Light Purple", 0xD9C1CC}, {"Light Aqua", 0xE8DDB7},
};

static QColor TColorToQColor(int Color)
{
  return QColor(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF);
}

static int QColorToTColor(const QColor & Color)
{
  int Result = Color.red() | (Color.green() << 8) | (Color.blue() << 16);
  // GetNonZeroColor
  return (Result == 0) ? 0x010101 : Result;
}

static bool IsStandardSessionColor(int Color)
{
  for (const auto & Standard : StandardSessionColors)
  {
    if (Standard.Color == Color)
    {
      return true;
    }
  }
  return false;
}

// Configuration\Interface\SessionColors: "BBGGRR,BBGGRR,..." (StoreColor)
static QList<int> LoadCustomColors()
{
  QList<int> Result;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) && Storage.OpenSubKey(QStringLiteral("Interface"), false))
  {
    for (const QString & S : Storage.ReadString(QStringLiteral("SessionColors"), QString()).split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
      bool Ok = false;
      int Color = S.trimmed().toInt(&Ok, 16);
      if (Ok)
      {
        Result.append(Color);
      }
    }
  }
  return Result;
}

static void SaveCustomColors(const QList<int> & Colors)
{
  QStringList List;
  for (int Color : Colors)
  {
    List.append(QStringLiteral("%1").arg(Color, 6, 16, QLatin1Char('0')).toUpper());
  }
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), true) && Storage.OpenSubKey(QStringLiteral("Interface"), true))
  {
    Storage.WriteString(QStringLiteral("SessionColors"), List.join(QLatin1Char(',')));
  }
}

// TColorChangeData::ColorChange: custom colors are remembered (most recent first)
static void ColorChange(int Color, const std::function<void(int)> & OnColorChange)
{
  if ((Color != 0) && !IsStandardSessionColor(Color))
  {
    QList<int> Colors = LoadCustomColors();
    Colors.removeAll(Color);
    Colors.prepend(Color);
    SaveCustomColors(Colors);
  }
  OnColorChange(Color);
}

static QWidgetAction * CreateColorPalette(QMenu * Menu, int Color, const QList<int> & Colors, const QStringList & Names,
  const std::function<void(int)> & OnColorChange)
{
  auto * Action = new QWidgetAction(Menu);
  auto * Palette = new QWidget(Menu);
  auto * Grid = new QGridLayout(Palette);
  Grid->setContentsMargins(6, 2, 6, 2);
  Grid->setSpacing(2);
  for (int Index = 0; Index < Colors.size(); Index++)
  {
    auto * Button = new QToolButton(Palette);
    Button->setAutoRaise(true);
    Button->setCheckable(true);
    Button->setChecked(Colors[Index] == Color);
    QPixmap Pixmap(14, 14);
    Pixmap.fill(TColorToQColor(Colors[Index]));
    {
      QPainter Painter(&Pixmap);
      Painter.setPen(Qt::gray);
      Painter.drawRect(0, 0, 13, 13);
    }
    Button->setIcon(QIcon(Pixmap));
    if (Index < Names.size())
    {
      Button->setToolTip(Names[Index]);
    }
    int AColor = Colors[Index];
    QObject::connect(Button, &QToolButton::clicked, Menu, [Menu, AColor, OnColorChange]()
    {
      Menu->close();
      ColorChange(AColor, OnColorChange);
    });
    Grid->addWidget(Button, Index / ColorCols, Index % ColorCols);
  }
  Action->setDefaultWidget(Palette);
  Menu->addAction(Action);
  return Action;
}

void SiteAdvancedDialog::PopulateSessionColorMenu(QMenu * Menu, int Color, const std::function<void(int)> & OnColorChange)
{
  Menu->clear();
  QAction * DefaultItem = Menu->addAction(QStringLiteral("&Default"));
  // COLOR_DEFAULT_HINT
  DefaultItem->setToolTip(QStringLiteral("Reset session (panel) color to system default"));
  DefaultItem->setCheckable(true);
  DefaultItem->setChecked(Color == 0);
  QObject::connect(DefaultItem, &QAction::triggered, Menu, [OnColorChange]() { ColorChange(0, OnColorChange); });
  Menu->addSeparator();
  QList<int> CustomColors = LoadCustomColors().mid(0, UserColorCount);
  if (!CustomColors.isEmpty())
  {
    CreateColorPalette(Menu, Color, CustomColors, QStringList(), OnColorChange);
    Menu->addSeparator();
  }
  QList<int> Standard;
  QStringList Names;
  for (const auto & S : StandardSessionColors)
  {
    Standard.append(S.Color);
    Names.append(QString::fromLatin1(S.Name));
  }
  CreateColorPalette(Menu, Color, Standard, Names, OnColorChange);
  Menu->addSeparator();
  // COLOR_PICK_CAPTION
  QAction * PickItem = Menu->addAction(QStringLiteral("&More Colors..."));
  PickItem->setToolTip(QStringLiteral("Choose any session (panel) color"));
  QWidget * Parent = Menu->parentWidget();
  QObject::connect(PickItem, &QAction::triggered, Menu, [Color, OnColorChange, Parent]()
  {
    // ColorPickClick: custom colors of the dialog = remembered + standard colors
    QList<int> Colors = LoadCustomColors();
    for (const auto & S : StandardSessionColors)
    {
      Colors.append(S.Color);
    }
    for (int Index = 0; (Index < Colors.size()) && (Index < QColorDialog::customCount()); Index++)
    {
      QColorDialog::setCustomColor(Index, TColorToQColor(Colors[Index]));
    }
    QColor Initial = (Color != 0) ? TColorToQColor(Color) : QColor(0xA6, 0xCA, 0xF0); // clSkyBlue
    QColor C = QColorDialog::getColor(Initial, Parent);
    if (C.isValid())
    {
      ColorChange(QColorToTColor(C), OnColorChange);
    }
  });
}

//---------------------------------------------------------------------------
// TSiteRawDialog

SiteRawDialog::SiteRawDialog(SessionData & Data, QWidget * Parent) :
  QDialog(Parent),
  FData(Data)
{
  // SITE_RAW_CAPTION
  setWindowTitle(QStringLiteral("Raw Site Settings"));
  auto * Layout = new QVBoxLayout(this);
  SettingsMemo = new QPlainTextEdit();
  SettingsMemo->setLineWrapMode(QPlainTextEdit::NoWrap);
  SettingsMemo->setMinimumSize(420, 333);
  Layout->addWidget(SettingsMemo, 1);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  // SITE_RAW_ADD
  QPushButton * AddButton = Buttons->addButton(QStringLiteral("&Add..."), QDialogButtonBox::ActionRole);
  connect(AddButton, &QPushButton::clicked, this, [this]() { AddButtonClick(); });
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // HELP_SITE_RAW
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_login_raw"));
  Layout->addWidget(Buttons);

  // TSiteRawDialog::Execute: the basic settings are edited on the Login dialog
  SessionData FactoryDefaults;
  SessionData RawData = Data;
  // SFTP-only is not reflected by the protocol prefix, we have to use raw settings for that
  if (RawData.FSProtocol != fsSFTPonly)
  {
    RawData.FSProtocol = FactoryDefaults.FSProtocol;
  }
  RawData.HostName = FactoryDefaults.HostName;
  RawData.PortNumber = FactoryDefaults.PortNumber;
  RawData.UserName = FactoryDefaults.UserName;
  RawData.Password = FactoryDefaults.Password;
  RawData.Ftps = FactoryDefaults.Ftps;
  SettingsMemo->setPlainText(RawData.SaveToOptions(&FactoryDefaults).join(QLatin1Char('\n')));
  resize(444, 420);
}

QStringList SiteRawDialog::Settings() const
{
  QStringList Result;
  for (const QString & Line : SettingsMemo->toPlainText().split(QLatin1Char('\n')))
  {
    if (!Line.trimmed().isEmpty())
    {
      Result.append(Line);
    }
  }
  return Result;
}

void SiteRawDialog::accept()
{
  SessionData Backup = FData;
  FData.Default();
  FData.Name = Backup.Name;
  FData.KnownHostsFile = Backup.KnownHostsFile;
  FData.FSProtocol = Backup.FSProtocol;
  FData.HostName = Backup.HostName;
  FData.PortNumber = Backup.PortNumber;
  FData.UserName = Backup.UserName;
  FData.Password = Backup.Password;
  FData.Ftps = Backup.Ftps;
  FData.ApplyRawSettings(Settings());
  QDialog::accept();
}

void SiteRawDialog::AddButtonClick()
{
  // names of all settings, except for the basic ones and those already listed
  SessionData FactoryDefaults;
  QStringList AllOptions = FactoryDefaults.SaveToOptions(nullptr);
  QStringList Excluded = {
    QStringLiteral("HostName"), QStringLiteral("PortNumber"), QStringLiteral("UserName"), QStringLiteral("PasswordPlain"),
    QStringLiteral("Password"), QStringLiteral("FSProtocol"), QStringLiteral("Ftps") };
  for (const QString & Setting : Settings())
  {
    Excluded.append(Setting.section(QLatin1Char('='), 0, 0).trimmed());
  }
  QStringList Names;
  QMap<QString, QString> Values;
  for (const QString & Option : AllOptions)
  {
    QString Name = Option.section(QLatin1Char('='), 0, 0);
    if (!Excluded.contains(Name, Qt::CaseInsensitive) && !Names.contains(Name))
    {
      Names.append(Name);
      Values.insert(Name, Option.mid(Name.size() + 1));
    }
  }
  Names.sort(Qt::CaseInsensitive);
  QDialog Dialog(this);
  // SITE_RAW_ADD_CAPTION, SITE_RAW_ADD_LABEL
  Dialog.setWindowTitle(QStringLiteral("Add Raw Site Setting"));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Label = new QLabel(QStringLiteral("&Site Setting:"));
  auto * AddComboBox = new QComboBox();
  AddComboBox->addItems(Names);
  AddComboBox->setMaxVisibleItems(20);
  Label->setBuddy(AddComboBox);
  Layout->addWidget(Label);
  Layout->addWidget(AddComboBox);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  // TCustomDialog(HelpKeyword) of TSiteRawDialog::AddButtonClick
  HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_login_raw"));
  Layout->addWidget(Buttons);
  if ((Dialog.exec() == QDialog::Accepted) && (AddComboBox->currentIndex() >= 0))
  {
    QString Name = AddComboBox->currentText();
    QString Value = Values.value(Name);
    QString Text = SettingsMemo->toPlainText();
    if (!Text.isEmpty() && !Text.endsWith(QLatin1Char('\n')))
    {
      Text += QLatin1Char('\n');
    }
    int Start = static_cast<int>(Text.size() + Name.size() + 1);
    Text += Name + QLatin1Char('=') + Value;
    SettingsMemo->setPlainText(Text);
    SettingsMemo->setFocus();
    QTextCursor Cursor = SettingsMemo->textCursor();
    Cursor.setPosition(Start);
    Cursor.setPosition(Start + static_cast<int>(Value.size()), QTextCursor::KeepAnchor);
    SettingsMemo->setTextCursor(Cursor);
  }
}

//---------------------------------------------------------------------------
// TSiteAdvancedDialog::PrivateKeyViewButtonClick
void SiteAdvancedDialog::PrivateKeyViewButtonClick()
{
  QString FileName = d->PrivateKeyEdit->text().trimmed();
  QString Line;
  try
  {
    Line = SecureShell::PublicKeyLine(FileName);
  }
  catch (const ExtException & E)
  {
    ShowErrorMessage(this, E.Message(), E.MoreMessages());
    return;
  }
  bool HasCertificate = !SecureShell::MatchingCertificateFile(FileName).isEmpty() ||
    !d->DetachedCertificateEdit->text().trimmed().isEmpty();
  QApplication::clipboard()->setText(Line);
  QMessageBox Box(QMessageBox::Information, windowTitle(),
    HasCertificate ?
      // LOGIN_KEY_WITH_CERTIFICATE
      QStringLiteral("This key contains an OpenSSH certificate.\nIt is not supposed to be added to OpenSSH authorized_keys file.") :
      // LOGIN_AUTHORIZED_KEYS
      QStringLiteral("Public key for pasting into OpenSSH authorized_keys file:"),
    QMessageBox::Ok, this);
  Box.setInformativeText(Line);
  Box.setTextInteractionFlags(Qt::TextSelectableByMouse);
  // COPY_KEY_BUTTON
  QPushButton * CopyButton = Box.addButton(QStringLiteral("&Copy Key"), QMessageBox::ActionRole);
  Box.exec();
  if (Box.clickedButton() == CopyButton)
  {
    QApplication::clipboard()->setText(Line);
  }
}

// TSiteAdvancedDialog::PrivateKeyUploadItemClick + TTerminalManager::UploadPublicKey
void SiteAdvancedDialog::PrivateKeyUploadItemClick()
{
  SessionData Data = FData;
  SaveSession(Data);
  Data.FSProtocol = fsSFTPonly; // no SCP fallback, as SCP does not implement GetHomeDirectory
  Data.RemoteDirectory.clear();

  // LOGIN_PUBLIC_KEY_TITLE / LOGIN_PUBLIC_KEY_FILTER
  QString FileName = QFileDialog::getOpenFileName(this, QStringLiteral("Select key to install into server"),
    d->PrivateKeyEdit->text().trimmed(),
    QStringLiteral("Public key files (*.pub);;Private key files (*.ppk *.pem *.key id_*);;All files (*)"));
  if (FileName.isEmpty())
  {
    return;
  }
  QString Comment;
  try
  {
    SecureShell::PublicKeyLine(FileName, &Comment);
  }
  catch (const ExtException & E)
  {
    ShowErrorMessage(this, E.Message(), E.MoreMessages());
    return;
  }

  GuiUserInterface UI(this);
  auto Cancelled = std::make_shared<bool>(false);
  UI.BeginConnecting(Data, [Cancelled]() { *Cancelled = true; });
  // LOGIN_PUBLIC_KEY_UPLOAD
  UI.Information(QStringLiteral("Installing public key \"%1\"...").arg(Comment.isEmpty() ? QFileInfo(FileName).fileName() : Comment));
  QString Installed;
  QString Error, MoreMessages;
  bool Aborted = false;
  QEventLoop Loop;
  QThread * Thread = QThread::create([&]()
  {
    try
    {
      Installed = SecureShell::InstallPublicKeyToServer(Data, &UI, FileName);
    }
    catch (const AbortException &)
    {
      Aborted = true;
    }
    catch (const ExtException & E)
    {
      Error = E.Message();
      MoreMessages = E.MoreMessages();
    }
    catch (const std::exception & E)
    {
      Error = QString::fromUtf8(E.what());
    }
  });
  connect(Thread, &QThread::finished, &Loop, &QEventLoop::quit);
  Thread->start();
  Loop.exec();
  Thread->wait();
  delete Thread;
  UI.EndConnecting();

  if (!Error.isEmpty() && !*Cancelled)
  {
    ShowErrorMessage(this, Error, MoreMessages);
  }
  else if (!Installed.isEmpty())
  {
    // PUBLIC_KEY_UPLOADED (with the **main instruction** markup)
    QString Message = Installed;
    Message.remove(QStringLiteral("**"));
    QMessageBox::information(this, windowTitle(), Message);
    // the private key is now usable for the site
    if (!FileName.endsWith(QStringLiteral(".pub"), Qt::CaseInsensitive))
    {
      d->PrivateKeyEdit->setText(FileName);
    }
    d->PrivateKeyEdit->setFocus();
  }
  Q_UNUSED(Aborted);
}
