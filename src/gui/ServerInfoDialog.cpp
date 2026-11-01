// Port of WinSCP source/forms/FileSystemInfo.{cpp,dfm}
#include "gui/ServerInfoDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/HelpSystem.h"

#include "core/Terminal.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{

// FSINFO_* captions
const QString RemoteSystemStr = QStringLiteral("Remote system");
const QString FsProtocolStr = QStringLiteral("File transfer protocol");
const QString CryptographicProtocolStr = QStringLiteral("Cryptographic protocol");
const QString SshImplementationStr = QStringLiteral("SSH implementation");
const QString CipherStr = QStringLiteral("Encryption algorithm");
const QString CompressionStr = QStringLiteral("Compression");

struct ParsedInfo
{
  QString RemoteSystem;
  QString SecurityProtocolName;
  QString SshImplementation;
  QString Cipher;
  QString Compression;
  QString HostKeyAlgorithm;
  QString HostKeyFingerprintSHA256;
  QString HostKeyFingerprintMD5;
  QString CertificateFingerprintSHA256;
  QString CertificateFingerprintSHA1;
  QStringList Additional;
};

// Splits the protocol's "Name: value" server information into the
// TSessionInfo / TFileSystemInfo fields
ParsedInfo ParseServerInfo(const QString & ProtocolName, const QString & ServerInfo)
{
  ParsedInfo Result;
  bool Ssh = ProtocolName.startsWith(QStringLiteral("SFTP"), Qt::CaseInsensitive) ||
    ProtocolName.startsWith(QStringLiteral("SCP"), Qt::CaseInsensitive);
  const QStringList Lines = ServerInfo.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const QString & ALine : Lines)
  {
    QString Line = ALine.trimmed();
    int P = Line.indexOf(QStringLiteral(": "));
    QString Name = (P > 0) ? Line.left(P) : QString();
    QString Value = (P > 0) ? Line.mid(P + 2).trimmed() : Line;
    if (Name == QStringLiteral("Server"))
    {
      (Ssh ? Result.SshImplementation : Result.RemoteSystem) = Value;
    }
    else if (Name == QStringLiteral("SSH protocol version"))
    {
      Result.SecurityProtocolName = QStringLiteral("SSH-%1").arg(Value);
    }
    else if (Name == QStringLiteral("Encryption algorithm"))
    {
      Result.Cipher = Value;
    }
    else if (Name == QStringLiteral("Encryption"))
    {
      Result.SecurityProtocolName = Value;
    }
    else if (Name == QStringLiteral("Cipher"))
    {
      Result.Cipher = Value;
    }
    else if (Name == QStringLiteral("Compression"))
    {
      Result.Compression = Value;
    }
    else if (Name == QStringLiteral("Host key algorithm"))
    {
      Result.HostKeyAlgorithm = Value;
    }
    else if (Name == QStringLiteral("Host key fingerprint (SHA-256)"))
    {
      Result.HostKeyFingerprintSHA256 = Value;
    }
    else if (Name == QStringLiteral("Host key fingerprint (MD5)"))
    {
      Result.HostKeyFingerprintMD5 = Value;
    }
    else if (Name == QStringLiteral("Certificate fingerprint (SHA-256)"))
    {
      Result.CertificateFingerprintSHA256 = Value;
    }
    else if (Name == QStringLiteral("Certificate fingerprint (SHA-1)"))
    {
      Result.CertificateFingerprintSHA1 = Value;
    }
    else
    {
      Result.Additional.append(Line);
    }
  }
  return Result;
}

QString BooleanToStr(bool Value)
{
  return Value ? QStringLiteral("Yes") : QStringLiteral("No");
}

// TFileSystemInfoDialog::SpaceStr
QString SpaceStr(qint64 Bytes)
{
  if (Bytes == 0)
  {
    return QStringLiteral("Unknown");   // FSINFO_BYTES_UNKNOWN
  }
  QLocale Locale;
  QString Result = Locale.formattedDataSize(Bytes, 2, QLocale::DataSizeTraditionalFormat);
  QString Unordered = Locale.toString(Bytes) + QStringLiteral(" B");
  if (Result != Unordered)
  {
    Result = QStringLiteral("%1 (%2)").arg(Result, Unordered);
  }
  return Result;
}

QTreeWidget * CreateListView(QWidget * Parent)
{
  QTreeWidget * View = new QTreeWidget(Parent);
  View->setColumnCount(2);
  View->setHeaderLabels({ QStringLiteral("Item"), QStringLiteral("Value") });
  // item names are WinSCP captions (Translator), values are data
  View->setProperty("NixSCPTranslateItems", QVariantList{ 0 });
  View->setRootIsDecorated(false);
  View->setSelectionMode(QAbstractItemView::ExtendedSelection);
  View->setAllColumnsShowFocus(true);
  View->header()->setStretchLastSection(true);
  View->setContextMenuPolicy(Qt::ActionsContextMenu);
  // ListViewMenu: Copy (CopyClick)
  QAction * Copy = new QAction(QStringLiteral("&Copy"), View);
  Copy->setShortcut(QKeySequence::Copy);
  Copy->setShortcutContext(Qt::WidgetShortcut);
  QObject::connect(Copy, &QAction::triggered, View, [View]()
  {
    QString Text;
    for (int Index = 0; Index < View->topLevelItemCount(); Index++)
    {
      QTreeWidgetItem * Item = View->topLevelItem(Index);
      if (Item->isSelected())
      {
        Text += QStringLiteral("%1 = %2\n").arg(Item->text(0), Item->text(1));
      }
    }
    QApplication::clipboard()->setText(Text);
  });
  View->addAction(Copy);
  return View;
}

QLineEdit * CreateReadOnlyEdit(QWidget * Parent)
{
  QLineEdit * Edit = new QLineEdit(Parent);
  Edit->setReadOnly(true);
  return Edit;
}

} // namespace

struct ServerInfoDialog::Private
{
  QString ProtocolName;
  ParsedInfo Info;
  std::function<bool(FSCapability)> IsCapable;
  GetSpaceAvailableFunc OnGetSpaceAvailable;
  SpaceAvailable Space;
  bool SpaceAvailableLoaded = false;

  QTabWidget * PageControl = nullptr;
  QWidget * ProtocolSheet = nullptr;
  QWidget * CapabilitiesSheet = nullptr;
  QWidget * SpaceAvailableSheet = nullptr;
  QTreeWidget * ServerView = nullptr;
  QTreeWidget * ProtocolView = nullptr;
  QTreeWidget * SpaceAvailableView = nullptr;
  QGroupBox * HostKeyGroup = nullptr;
  QLineEdit * HostKeyAlgorithmEdit = nullptr;
  QLineEdit * HostKeyFingerprintSHA256Edit = nullptr;
  QLineEdit * HostKeyFingerprintMD5Edit = nullptr;
  QGroupBox * CertificateGroup = nullptr;
  QLineEdit * CertificateFingerprintSha256Edit = nullptr;
  QLineEdit * CertificateFingerprintSha1Edit = nullptr;
  QGroupBox * InfoGroup = nullptr;
  QPlainTextEdit * InfoMemo = nullptr;
  QLineEdit * SpaceAvailablePathEdit = nullptr;
  QPushButton * SpaceAvailableButton = nullptr;

  QString CapabilityStr(FSCapability Capability) const
  {
    return BooleanToStr(IsCapable && IsCapable(Capability));
  }

  QString CapabilityStr(FSCapability Capability1, FSCapability Capability2) const
  {
    return QStringLiteral("%1/%2").arg(CapabilityStr(Capability1), CapabilityStr(Capability2));
  }

  struct Row
  {
    QTreeWidget * View;
    QString Item;
    QString Value;
  };

  // TFileSystemInfoDialog::Feed (list parts)
  QList<Row> Rows() const
  {
    QList<Row> Result;
    auto Add = [&Result](QTreeWidget * View, const QString & Item, const QString & Value)
    {
      if (!Value.isEmpty())
      {
        Result.append({ View, Item, Value });
      }
    };
    Add(ServerView, RemoteSystemStr, Info.RemoteSystem);
    Add(ServerView, FsProtocolStr, ProtocolName);
    Add(ServerView, CryptographicProtocolStr, Info.SecurityProtocolName);
    Add(ServerView, SshImplementationStr, Info.SshImplementation);
    Add(ServerView, CipherStr, Info.Cipher);
    Add(ServerView, CompressionStr, Info.Compression.isEmpty() ? QStringLiteral("No") : Info.Compression);

    Add(ProtocolView, QStringLiteral("Can change permissions"), CapabilityStr(fcModeChanging));
    Add(ProtocolView, QStringLiteral("Can change ACL"), CapabilityStr(fcAclChangingFiles));
    Add(ProtocolView, QStringLiteral("Can change owner/group"), CapabilityStr(fcGroupChanging));
    QString AnyCommand;
    if (IsCapable && !IsCapable(fcShellAnyCommand) && IsCapable(fcAnyCommand))
    {
      AnyCommand = QStringLiteral("Protocol commands only");   // FSINFO_PROTOCOL_ANY_COMMAND
    }
    else
    {
      AnyCommand = CapabilityStr(fcAnyCommand);
    }
    Add(ProtocolView, QStringLiteral("Can execute arbitrary command"), AnyCommand);
    Add(ProtocolView, QStringLiteral("Can create symbolic/hard link"), CapabilityStr(fcSymbolicLink, fcHardLink));
    Add(ProtocolView, QStringLiteral("Can lookup user groups"), CapabilityStr(fcUserGroupListing));
    Add(ProtocolView, QStringLiteral("Can duplicate remote files"), CapabilityStr(fcRemoteCopy));
    Add(ProtocolView, QStringLiteral("Can check available space"), CapabilityStr(fcCheckingSpaceAvailable));
    Add(ProtocolView, QStringLiteral("Can calculate file checksum"), CapabilityStr(fcCalculatingChecksum));
    Add(ProtocolView, QStringLiteral("Native text (ASCII) mode transfers"), CapabilityStr(fcNativeTextMode));

    Add(SpaceAvailableView, QStringLiteral("Total bytes on device"), SpaceStr(Space.BytesOnDevice));
    Add(SpaceAvailableView, QStringLiteral("Free bytes on device"), SpaceStr(Space.UnusedBytesOnDevice));
    Add(SpaceAvailableView, QStringLiteral("Total bytes for user"), SpaceStr(Space.BytesAvailableToUser));
    Add(SpaceAvailableView, QStringLiteral("Free bytes for user"), SpaceStr(Space.UnusedBytesAvailableToUser));
    Add(SpaceAvailableView, QStringLiteral("Bytes per allocation unit"), SpaceStr(Space.BytesPerAllocationUnit));
    return Result;
  }

  // TFileSystemInfoDialog::FeedControls
  void FeedControls()
  {
    for (QTreeWidget * View : { ServerView, ProtocolView, SpaceAvailableView })
    {
      View->clear();
    }
    for (const Row & R : Rows())
    {
      new QTreeWidgetItem(R.View, { R.Item, R.Value });
    }
    for (QTreeWidget * View : { ServerView, ProtocolView, SpaceAvailableView })
    {
      View->resizeColumnToContents(0);
    }

    bool HasHostKey = !Info.HostKeyFingerprintSHA256.isEmpty() || !Info.HostKeyFingerprintMD5.isEmpty();
    HostKeyGroup->setVisible(HasHostKey);
    HostKeyAlgorithmEdit->setText(Info.HostKeyAlgorithm);
    HostKeyFingerprintSHA256Edit->setText(Info.HostKeyFingerprintSHA256);
    HostKeyFingerprintMD5Edit->setText(Info.HostKeyFingerprintMD5);
    bool HasCertificate = !Info.CertificateFingerprintSHA256.isEmpty() || !Info.CertificateFingerprintSHA1.isEmpty();
    CertificateGroup->setVisible(HasCertificate);
    CertificateFingerprintSha256Edit->setText(Info.CertificateFingerprintSHA256);
    CertificateFingerprintSha1Edit->setText(Info.CertificateFingerprintSHA1);
    QString Additional = Info.Additional.join(QLatin1Char('\n'));
    InfoGroup->setVisible(!Additional.isEmpty());
    InfoMemo->setPlainText(Additional);
  }

  bool SpaceAvailableSupported() const
  {
    return static_cast<bool>(OnGetSpaceAvailable);
  }

  void UpdateControls()
  {
    PageControl->setTabEnabled(PageControl->indexOf(SpaceAvailableSheet), SpaceAvailableSupported());
    SpaceAvailableButton->setEnabled(SpaceAvailableSupported() && !SpaceAvailablePathEdit->text().isEmpty());
  }

  // TFileSystemInfoDialog::CheckSpaceAvailable
  void CheckSpaceAvailable(QWidget * Parent)
  {
    SpaceAvailableLoaded = true;
    SpaceAvailable Result;
    QString Error;
    if (OnGetSpaceAvailable(SpaceAvailablePathEdit->text(), Result, Error))
    {
      Space = Result;
    }
    else if (!Error.isEmpty())
    {
      ShowErrorMessage(Parent, Error);
    }
    FeedControls();
  }

  void NeedSpaceAvailable(QWidget * Parent)
  {
    if (!SpaceAvailableLoaded && SpaceAvailableSupported() && !SpaceAvailablePathEdit->text().isEmpty())
    {
      CheckSpaceAvailable(Parent);
    }
  }
};

ServerInfoDialog::ServerInfoDialog(QWidget * Parent, const QString & ProtocolName, const QString & ServerInfo,
  std::function<bool(FSCapability)> IsCapable, const QString & SpaceAvailablePath, GetSpaceAvailableFunc OnGetSpaceAvailable) :
  QDialog(Parent),
  d(new Private)
{
  d->ProtocolName = ProtocolName;
  d->Info = ParseServerInfo(ProtocolName, ServerInfo);
  d->IsCapable = std::move(IsCapable);
  d->OnGetSpaceAvailable = std::move(OnGetSpaceAvailable);
  setObjectName(QStringLiteral("FileSystemInfoDialog"));
  setWindowTitle(QStringLiteral("Server and protocol information"));
  setSizeGripEnabled(true);

  d->PageControl = new QTabWidget(this);

  // Protocol
  d->ProtocolSheet = new QWidget(d->PageControl);
  QVBoxLayout * ProtocolLayout = new QVBoxLayout(d->ProtocolSheet);
  d->ServerView = CreateListView(d->ProtocolSheet);
  d->ServerView->setObjectName(QStringLiteral("ServerView"));
  ProtocolLayout->addWidget(d->ServerView, 1);

  d->HostKeyGroup = new QGroupBox(QStringLiteral("Server host key fingerprints"), d->ProtocolSheet);
  QGridLayout * HostKeyLayout = new QGridLayout(d->HostKeyGroup);
  d->HostKeyAlgorithmEdit = CreateReadOnlyEdit(d->HostKeyGroup);
  d->HostKeyFingerprintSHA256Edit = CreateReadOnlyEdit(d->HostKeyGroup);
  d->HostKeyFingerprintMD5Edit = CreateReadOnlyEdit(d->HostKeyGroup);
  const QList<QPair<QString, QLineEdit *>> HostKeyRows =
    { { QStringLiteral("Algorithm:"), d->HostKeyAlgorithmEdit },
      { QStringLiteral("SHA-256:"), d->HostKeyFingerprintSHA256Edit },
      { QStringLiteral("MD5:"), d->HostKeyFingerprintMD5Edit } };
  for (int Index = 0; Index < HostKeyRows.size(); Index++)
  {
    QLabel * Label = new QLabel(HostKeyRows.at(Index).first, d->HostKeyGroup);
    Label->setBuddy(HostKeyRows.at(Index).second);
    HostKeyLayout->addWidget(Label, Index, 0);
    HostKeyLayout->addWidget(HostKeyRows.at(Index).second, Index, 1);
  }
  ProtocolLayout->addWidget(d->HostKeyGroup);

  d->CertificateGroup = new QGroupBox(QStringLiteral("Certificate fingerprint"), d->ProtocolSheet);
  QGridLayout * CertificateLayout = new QGridLayout(d->CertificateGroup);
  d->CertificateFingerprintSha256Edit = CreateReadOnlyEdit(d->CertificateGroup);
  d->CertificateFingerprintSha1Edit = CreateReadOnlyEdit(d->CertificateGroup);
  QLabel * Sha256Label = new QLabel(QStringLiteral("SHA-256:"), d->CertificateGroup);
  Sha256Label->setBuddy(d->CertificateFingerprintSha256Edit);
  QLabel * Sha1Label = new QLabel(QStringLiteral("SHA-1:"), d->CertificateGroup);
  Sha1Label->setBuddy(d->CertificateFingerprintSha1Edit);
  CertificateLayout->addWidget(Sha256Label, 0, 0);
  CertificateLayout->addWidget(d->CertificateFingerprintSha256Edit, 0, 1);
  CertificateLayout->addWidget(Sha1Label, 1, 0);
  CertificateLayout->addWidget(d->CertificateFingerprintSha1Edit, 1, 1);
  ProtocolLayout->addWidget(d->CertificateGroup);
  d->PageControl->addTab(d->ProtocolSheet, QStringLiteral("Protocol"));

  // Capabilities
  d->CapabilitiesSheet = new QWidget(d->PageControl);
  QVBoxLayout * CapabilitiesLayout = new QVBoxLayout(d->CapabilitiesSheet);
  d->ProtocolView = CreateListView(d->CapabilitiesSheet);
  d->ProtocolView->setObjectName(QStringLiteral("ProtocolView"));
  CapabilitiesLayout->addWidget(d->ProtocolView, 1);
  d->InfoGroup = new QGroupBox(QStringLiteral("Additional information"), d->CapabilitiesSheet);
  QVBoxLayout * InfoLayout = new QVBoxLayout(d->InfoGroup);
  d->InfoMemo = new QPlainTextEdit(d->InfoGroup);
  d->InfoMemo->setReadOnly(true);
  d->InfoMemo->setLineWrapMode(QPlainTextEdit::NoWrap);
  InfoLayout->addWidget(d->InfoMemo);
  CapabilitiesLayout->addWidget(d->InfoGroup, 1);
  d->PageControl->addTab(d->CapabilitiesSheet, QStringLiteral("Capabilities"));

  // Space available
  d->SpaceAvailableSheet = new QWidget(d->PageControl);
  QGridLayout * SpaceLayout = new QGridLayout(d->SpaceAvailableSheet);
  d->SpaceAvailablePathEdit = new QLineEdit(SpaceAvailablePath, d->SpaceAvailableSheet);
  QLabel * PathLabel = new QLabel(QStringLiteral("&Path:"), d->SpaceAvailableSheet);
  PathLabel->setBuddy(d->SpaceAvailablePathEdit);
  d->SpaceAvailableButton = new QPushButton(QStringLiteral("Check &space"), d->SpaceAvailableSheet);
  d->SpaceAvailableButton->setAutoDefault(false);
  d->SpaceAvailableView = CreateListView(d->SpaceAvailableSheet);
  d->SpaceAvailableView->setObjectName(QStringLiteral("SpaceAvailableView"));
  SpaceLayout->addWidget(PathLabel, 0, 0);
  SpaceLayout->addWidget(d->SpaceAvailablePathEdit, 0, 1);
  SpaceLayout->addWidget(d->SpaceAvailableButton, 0, 2);
  SpaceLayout->addWidget(d->SpaceAvailableView, 1, 0, 1, 3);
  SpaceLayout->setRowStretch(1, 1);
  d->PageControl->addTab(d->SpaceAvailableSheet, QStringLiteral("Space available"));

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  QPushButton * ClipboardButton = Buttons->addButton(QStringLiteral("&Copy to Clipboard"), QDialogButtonBox::ResetRole);
  ClipboardButton->setAutoDefault(false);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_fsinfo"));
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(ClipboardButton, &QPushButton::clicked, this, [this]()
  {
    QApplication::clipboard()->setText(ClipboardText());
  });
  connect(d->SpaceAvailableButton, &QPushButton::clicked, this, [this]() { d->CheckSpaceAvailable(this); });
  connect(d->SpaceAvailablePathEdit, &QLineEdit::textChanged, this, [this]() { d->UpdateControls(); });
  // PageControlChange
  connect(d->PageControl, &QTabWidget::currentChanged, this, [this](int Index)
  {
    if (d->PageControl->widget(Index) == d->SpaceAvailableSheet)
    {
      d->NeedSpaceAvailable(this);
    }
  });

  QVBoxLayout * Layout = new QVBoxLayout(this);
  Layout->addWidget(d->PageControl, 1);
  Layout->addWidget(Buttons);

  d->FeedControls();
  d->UpdateControls();
  d->PageControl->setCurrentWidget(d->ProtocolSheet);
  resize(510, 500);
}

ServerInfoDialog::~ServerInfoDialog()
{
  delete d;
}

QString ServerInfoDialog::ClipboardText()
{
  // ClipboardButtonClick / ClipboardAddItem
  d->NeedSpaceAvailable(this);
  QString Result;
  const QString Divider = QString(60, QLatin1Char('-')) + QLatin1Char('\n');
  QObject * LastGroup = nullptr;
  auto StartGroup = [&](QObject * Group, const QString & Caption)
  {
    if (LastGroup != Group)
    {
      if (LastGroup != nullptr)
      {
        Result += Divider;
      }
      if (!Caption.isEmpty())
      {
        Result += Caption + QLatin1Char('\n');
      }
      LastGroup = Group;
    }
  };
  for (const Private::Row & R : d->Rows())
  {
    if ((R.View == d->SpaceAvailableView) && !d->SpaceAvailableSupported())
    {
      continue;
    }
    StartGroup(R.View, QString());
    Result += QStringLiteral("%1 = %2\n").arg(R.Item, R.Value);
  }
  auto AddEdit = [&](QGroupBox * Group, const QString & Label, const QString & Value)
  {
    if (!Value.isEmpty())
    {
      StartGroup(Group, Group->title());
      Result += QStringLiteral("%1 = %2\n").arg(Label, Value);
    }
  };
  AddEdit(d->HostKeyGroup, QStringLiteral("Algorithm"), d->Info.HostKeyAlgorithm);
  AddEdit(d->HostKeyGroup, QStringLiteral("SHA-256"), d->Info.HostKeyFingerprintSHA256);
  AddEdit(d->HostKeyGroup, QStringLiteral("MD5"), d->Info.HostKeyFingerprintMD5);
  AddEdit(d->CertificateGroup, QStringLiteral("SHA-256"), d->Info.CertificateFingerprintSHA256);
  AddEdit(d->CertificateGroup, QStringLiteral("SHA-1"), d->Info.CertificateFingerprintSHA1);
  if (!d->Info.Additional.isEmpty())
  {
    StartGroup(d->InfoGroup, d->InfoGroup->title());
    Result += d->Info.Additional.join(QLatin1Char('\n')) + QLatin1Char('\n');
  }
  return Result;
}

QTreeWidget * ServerInfoDialog::ServerView() const { return d->ServerView; }
QTreeWidget * ServerInfoDialog::ProtocolView() const { return d->ProtocolView; }
QTreeWidget * ServerInfoDialog::SpaceAvailableView() const { return d->SpaceAvailableView; }
QGroupBox * ServerInfoDialog::HostKeyGroup() const { return d->HostKeyGroup; }
QGroupBox * ServerInfoDialog::CertificateGroup() const { return d->CertificateGroup; }
QGroupBox * ServerInfoDialog::InfoGroup() const { return d->InfoGroup; }
QTabWidget * ServerInfoDialog::PageControl() const { return d->PageControl; }
QString ServerInfoDialog::HostKeyFingerprintSHA256() const { return d->HostKeyFingerprintSHA256Edit->text(); }
QString ServerInfoDialog::CertificateFingerprintSHA256() const { return d->CertificateFingerprintSha256Edit->text(); }
QString ServerInfoDialog::AdditionalInfo() const { return d->InfoMemo->toPlainText(); }

QString ServerInfoDialog::ListValue(QTreeWidget * View, const QString & Item)
{
  for (int Index = 0; Index < View->topLevelItemCount(); Index++)
  {
    if (View->topLevelItem(Index)->text(0) == Item)
    {
      return View->topLevelItem(Index)->text(1);
    }
  }
  return QString();
}

void ServerInfoDialog::Execute(QWidget * Parent, Terminal * ATerminal, const QString & SpaceAvailablePath,
  GetSpaceAvailableFunc OnGetSpaceAvailable)
{
  QPointer<Terminal> Term(ATerminal);
  ServerInfoDialog Dialog(Parent, ATerminal->ProtocolName(), ATerminal->ServerInfo(),
    [Term](FSCapability Capability) { return Term && Term->IsCapable(Capability); },
    SpaceAvailablePath, std::move(OnGetSpaceAvailable));
  Dialog.exec();
}
