#include "gui/AgentAccessDialog.h"
#include "gui/HelpSystem.h"

#include "core/AgentAccess.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QDateTime>
#include <QSplitter>
#include <QLocale>
#include <QListWidget>
#include <QDir>
#include <QDialog>
#include <QTextCursor>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

// Read-only text (agent access log and help)
static void ShowText(QWidget * Window, const QString & Title, const QString & Text, bool ScrollToEnd)
{
  QDialog Dialog(Window);
  Dialog.setWindowTitle(Title);
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Edit = new QPlainTextEdit(&Dialog);
  Edit->setReadOnly(true);
  Edit->setLineWrapMode(QPlainTextEdit::NoWrap);
  Edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  Edit->setPlainText(Text);
  Layout->addWidget(Edit);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Layout->addWidget(Buttons);
  Dialog.resize(860, 520);
  if (ScrollToEnd)
  {
    Edit->moveCursor(QTextCursor::End);
  }
  Dialog.exec();
}

void AgentAccessDialog::ShowLog(QWidget * Window)
{
  QFile File(AgentAccess::AuditLogFileName());
  QString Text = File.open(QIODevice::ReadOnly) ?
    QStringLiteral("TIME\tSITE\tREQUEST\tRESULT\tDETAIL\n") + QString::fromUtf8(File.readAll()) :
    QStringLiteral("No agent requests yet.\n\nEnable agent access for a site in its settings (Agent Access... next to Advanced...); "
      "programs then use it with \"nixscp --connect\". See Agent Access Help.");
  ShowText(Window, QStringLiteral("Agent Access Log"), Text, true);
}

void AgentAccessDialog::ShowHelp(QWidget * Window)
{
  ShowText(Window, QStringLiteral("Agent Access Help"), AgentAccess::HelpText(), false);
}

void AgentAccessDialog::ShowSessions(QWidget * Window)
{
  QDialog Dialog(Window);
  Dialog.setWindowTitle(QStringLiteral("Agent Sessions"));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Splitter = new QSplitter(&Dialog);
  auto * List = new QListWidget(Splitter);
  auto * Text = new QPlainTextEdit(Splitter);
  Text->setReadOnly(true);
  Text->setLineWrapMode(QPlainTextEdit::NoWrap);
  Text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  Splitter->setStretchFactor(1, 3);
  Layout->addWidget(Splitter);
  QDir Dir(AgentAccess::RecordingDirectory());
  auto Fill = [List, Text, &Dir]()
  {
    List->clear();
    Text->clear();
    // file names: yyyyMMdd-HHmmss-zzz-<site>-<request>.log, newest first
    for (const QFileInfo & Info : Dir.entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Name | QDir::Reversed))
    {
      QString Base = Info.completeBaseName();
      QDateTime Time = QDateTime::fromString(Base.left(19), QStringLiteral("yyyyMMdd-HHmmss-zzz"));
      QString Rest = Base.mid(20);
      QString Request = Rest.section(QLatin1Char('-'), -1);
      QString Site = Rest.section(QLatin1Char('-'), 0, -2);
      auto * Item = new QListWidgetItem(QStringLiteral("%1  %2  %3")
        .arg(QLocale().toString(Time, QLocale::ShortFormat), Site, Request), List);
      Item->setData(Qt::UserRole, Info.absoluteFilePath());
    }
    if (List->count() == 0)
    {
      Text->setPlainText(QStringLiteral("No recorded agent sessions.\n\nSites with \"Record sessions\" on (Agent Access...) "
        "keep the commands agents run and their output here."));
    }
  };
  QObject::connect(List, &QListWidget::currentItemChanged, &Dialog, [Text](QListWidgetItem * Item)
  {
    if (Item == nullptr)
    {
      return;
    }
    bool Locked = false;
    QByteArray Data = ConfigVault::ReadFile(Item->data(Qt::UserRole).toString(), &Locked);
    Text->setPlainText(Locked ? QStringLiteral("(encrypted with another NixSCP password)") : QString::fromUtf8(Data));
  });
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  QPushButton * DeleteAll = Buttons->addButton(QStringLiteral("&Delete all"), QDialogButtonBox::ActionRole);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  QObject::connect(DeleteAll, &QPushButton::clicked, &Dialog, [&Dialog, &Dir, Fill]()
  {
    if (QMessageBox::question(&Dialog, Dialog.windowTitle(), QStringLiteral("Delete all recorded agent sessions?")) == QMessageBox::Yes)
    {
      for (const QFileInfo & Info : Dir.entryInfoList({QStringLiteral("*.log")}, QDir::Files))
      {
        QFile::remove(Info.absoluteFilePath());
      }
      Fill();
    }
  });
  Layout->addWidget(Buttons);
  Fill();
  if (List->count() > 0)
  {
    List->setCurrentRow(0);
  }
  Dialog.resize(1000, 600);
  Dialog.exec();
}

AgentAccessDialog::AgentAccessDialog(SessionData & Data, QWidget * Parent) : QDialog(Parent), FData(Data)
{
  setWindowTitle(QStringLiteral("Agent Access - %1").arg(Data.Name.isEmpty() ? Data.HostName : Data.Name));
  auto * Layout = new QVBoxLayout(this);
  auto * Intro = new QLabel(QStringLiteral(
    "Let programs such as AI agents use this site with <b>nixscp --connect</b>, <b>--get</b> and <b>--put</b> "
    "<b>without ever getting its password or private key</b>: NixSCP connects for them while it is running. "
    "Agents learn the commands with <b>nixscp --help</b>."), this);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);

  auto * Group = new QGroupBox(QStringLiteral("Access for agents"), this);
  auto * G = new QVBoxLayout(Group);
  FAccess = new QComboBox(Group);
  FAccess->addItems({QStringLiteral("Off"), QStringLiteral("Allowed without asking"), QStringLiteral("Ask me each time")});
  FAccess->setToolTip(QStringLiteral("\"Ask me each time\" shows each request (the command, the file) and lets you "
    "allow it once, for 1 hour, for 24 hours or always, or deny it."));
  G->addWidget(FAccess);
  FCommands = new QCheckBox(QStringLiteral("Run &commands and interactive shells"), Group);
  FTransfer = new QCheckBox(QStringLiteral("&Download files"), Group);
  FUpload = new QCheckBox(QStringLiteral("&Upload files"), Group);
  FTunnels = new QCheckBox(QStringLiteral("Open &tunnels (port forwarding, nixscp --tunnel)"), Group);
  FRecord = new QCheckBox(QStringLiteral("&Record sessions (commands and their output, see Commands > Agent Sessions)"), Group);
  G->addWidget(FCommands);
  G->addWidget(FTransfer);
  G->addWidget(FUpload);
  G->addWidget(FTunnels);
  G->addWidget(FRecord);
  auto * NameLabel = new QLabel(QStringLiteral("&Name for agents:"), Group);
  FAgentName = new QLineEdit(Group);
  FAgentName->setPlaceholderText(Data.Name.isEmpty() ? QStringLiteral("(the site name)") : Data.Name);
  FAgentName->setToolTip(QStringLiteral("The name agents see in \"nixscp --list\" and use with --connect, --get and --put.\n"
    "Empty: the site name. When set, the site name stays hidden from agents."));
  NameLabel->setBuddy(FAgentName);
  G->addWidget(NameLabel);
  G->addWidget(FAgentName);
  Layout->addWidget(Group);

  // optional command rules (default: everything allowed)
  auto * Rules = new QGroupBox(QStringLiteral("Command rules (optional)"), this);
  auto * R = new QGridLayout(Rules);
  auto * RulesNote = new QLabel(QStringLiteral("One pattern per line, * matches anything (e.g. <b>systemctl status *</b>). "
    "Empty: all commands are allowed. Each part of a command (split at ; &amp;&amp; || |) must match an allowed pattern and "
    "none of the denied ones. Rules are a safety net, not a sandbox: an allowed program can still do more."), Rules);
  RulesNote->setWordWrap(true);
  R->addWidget(RulesNote, 0, 0, 1, 2);
  R->addWidget(new QLabel(QStringLiteral("Allowed commands:"), Rules), 1, 0);
  R->addWidget(new QLabel(QStringLiteral("Denied commands:"), Rules), 1, 1);
  FAllow = new QPlainTextEdit(Rules);
  FDeny = new QPlainTextEdit(Rules);
  for (QPlainTextEdit * Edit : {FAllow, FDeny})
  {
    Edit->setTabChangesFocus(true);
    Edit->setFixedHeight(fontMetrics().lineSpacing() * 6);
  }
  FAllow->setPlaceholderText(QStringLiteral("(all commands)"));
  FDeny->setPlaceholderText(QStringLiteral("(none)"));
  R->addWidget(FAllow, 2, 0);
  R->addWidget(FDeny, 2, 1);
  auto * Preset = new QPushButton(QStringLiteral("Read-only &preset"), Rules);
  Preset->setToolTip(QStringLiteral("Inspection commands only (logs, status, disk, processes...) and downloads only"));
  auto * Clear = new QPushButton(QStringLiteral("&Clear rules"), Rules);
  auto * PresetRow = new QHBoxLayout();
  PresetRow->addWidget(Preset);
  PresetRow->addWidget(Clear);
  PresetRow->addStretch();
  R->addLayout(PresetRow, 3, 0, 1, 2);
  Layout->addWidget(Rules);
  connect(Preset, &QPushButton::clicked, this, [this]()
  {
    FAllow->setPlainText(AgentAccess::ReadOnlyAllowPreset());
    FDeny->setPlainText(AgentAccess::ReadOnlyDenyPreset());
    FTransfer->setChecked(true);
    FUpload->setChecked(false);
  });
  connect(Clear, &QPushButton::clicked, this, [this]()
  {
    FAllow->clear();
    FDeny->clear();
  });
  connect(FTransfer, &QCheckBox::toggled, this, [this]() { UpdateControls(); });

  FWarning = new QLabel(this);
  FWarning->setWordWrap(true);
  FWarning->setTextFormat(Qt::RichText);
  Layout->addWidget(FWarning);
  Layout->addStretch();

  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(Buttons, &QDialogButtonBox::helpRequested, this, []() { HelpSystem::ShowHelp(QStringLiteral("nixscp:faq#agent-access")); });
  Layout->addWidget(Buttons);
  HelpSystem::InstallFormHelp(this, QStringLiteral("nixscp:faq#agent-access"));

  FAccess->setCurrentIndex(qBound(0, Data.AgentAccess, 2));
  FAgentName->setText(Data.AgentName);
  FCommands->setChecked(Data.AgentPermissions & AgentAccess::apCommands);
  FTransfer->setChecked(Data.AgentPermissions & AgentAccess::apTransfer);
  FUpload->setChecked(!(Data.AgentPermissions & AgentAccess::apDownloadOnly));
  FTunnels->setChecked(Data.AgentPermissions & AgentAccess::apTunnels);
  FRecord->setChecked(Data.AgentRecord);
  FAllow->setPlainText(Data.AgentAllow);
  FDeny->setPlainText(Data.AgentDeny);
  FOtherPermissions = Data.AgentPermissions &
    ~(AgentAccess::apCommands | AgentAccess::apTransfer | AgentAccess::apDownloadOnly | AgentAccess::apTunnels);
  connect(FAccess, &QComboBox::currentIndexChanged, this, [this]() { UpdateControls(); });
  UpdateControls();
  resize(qMax(sizeHint().width(), 520), sizeHint().height());
}

void AgentAccessDialog::UpdateControls()
{
  bool Encrypted = AgentAccess::PasswordsProtected();
  bool On = (FAccess->currentIndex() != AgentAccess::amOff);
  // turning it on needs the encrypted configuration (else saved passwords are only obfuscated)
  FAccess->setEnabled(Encrypted || On);
  FCommands->setEnabled(On);
  FTransfer->setEnabled(On);
  FUpload->setEnabled(On && FTransfer->isChecked());
  FTunnels->setEnabled(On);
  FRecord->setEnabled(On);
  FAllow->setEnabled(On);
  FDeny->setEnabled(On);
  FAgentName->setEnabled(On);
  QStringList Warnings;
  if (!FData.IsSsh())
  {
    Warnings.append(QStringLiteral("<b>Agent access works with SFTP and SCP sites only.</b>"));
  }
  if (!Encrypted)
  {
    Warnings.append(QStringLiteral("<b>Encrypt your saved passwords first:</b> set a master password (Preferences &gt; "
      "Security), or sign in to NixSCP Pro, whose NixSCP password encrypts the whole configuration. Without either, saved "
      "passwords are only obfuscated in the configuration file, which any program you run could decode."));
  }
  QString Key = FData.PublicKeyFile.trimmed();
  if (On && !Key.isEmpty() && QFileInfo(Key).isFile() && !AgentAccess::KeyFileHasPassphrase(Key))
  {
    Warnings.append(QStringLiteral("<b>The private key file has no passphrase.</b> Any program you run, including an agent, "
      "can read it directly. Protect it with a passphrase (Tools &gt; Run PuTTYgen or ssh-keygen -p); NixSCP then asks "
      "you for it when an agent connects."));
  }
  if (On)
  {
    Warnings.append(QStringLiteral("Agent access needs NixSCP running. "
      "Requests are recorded in the agent access log (Commands menu)."));
  }
  FWarning->setText(Warnings.join(QStringLiteral("<br><br>")));
}

void AgentAccessDialog::accept()
{
  QString AgentName = FAgentName->text().trimmed();
  QString Effective = AgentName.isEmpty() ? FData.Name : AgentName;
  if ((FAccess->currentIndex() != AgentAccess::amOff) && !Effective.isEmpty())
  {
    // agents address a site by this name: it must be unique among the sites they may use
    for (const auto & Other : Configuration::Instance().StoredSessions().Sessions())
    {
      if ((Other->Name != FData.Name) && (Other->AgentAccess != AgentAccess::amOff) &&
          (AgentAccess::AgentSiteName(*Other) == Effective))
      {
        QMessageBox::warning(this, windowTitle(),
          QStringLiteral("Agents already use the name \"%1\" for the site \"%2\". Choose another name for agents.")
            .arg(Effective, Other->Name));
        FAgentName->setFocus();
        FAgentName->selectAll();
        return;
      }
    }
  }
  FData.AgentName = AgentName;
  FData.AgentAccess = FAccess->currentIndex();
  FData.AgentPermissions = FOtherPermissions |
    (FCommands->isChecked() ? AgentAccess::apCommands : 0) |
    (FTransfer->isChecked() ? AgentAccess::apTransfer : 0) |
    ((FTransfer->isChecked() && !FUpload->isChecked()) ? AgentAccess::apDownloadOnly : 0) |
    (FTunnels->isChecked() ? AgentAccess::apTunnels : 0);
  FData.AgentRecord = FRecord->isChecked();
  FData.AgentAllow = FAllow->toPlainText().trimmed();
  FData.AgentDeny = FDeny->toPlainText().trimmed();
  QDialog::accept();
}

QString AgentAccessDialog::Summary(const SessionData & Data)
{
  switch (Data.AgentAccess)
  {
    case AgentAccess::amAllow: return QStringLiteral("Allowed");
    case AgentAccess::amAsk: return QStringLiteral("Asks first");
    default: return QStringLiteral("Off");
  }
}
