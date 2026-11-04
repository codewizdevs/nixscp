#include "gui/TeamsDialog.h"

#include "core/CloudSettings.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamSync.h"
#include "gui/Icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{

QString RoleText(const QString & Role)
{
  if (Role == QStringLiteral("owner")) return QStringLiteral("Owner");
  if (Role == QStringLiteral("admin")) return QStringLiteral("Admin");
  return QStringLiteral("Member");
}

QTableWidget * NewTable(QWidget * Parent, const QStringList & Headers)
{
  auto * Table = new QTableWidget(0, static_cast<int>(Headers.size()), Parent);
  Table->setHorizontalHeaderLabels(Headers);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->verticalHeader()->hide();
  Table->horizontalHeader()->setStretchLastSection(true);
  return Table;
}

QList<TeamSync::Team> SortedTeams()
{
  QList<TeamSync::Team> Teams = TeamSync::Instance().Teams();
  std::sort(Teams.begin(), Teams.end(), [](const TeamSync::Team & A, const TeamSync::Team & B)
  {
    return A.Name.compare(B.Name, Qt::CaseInsensitive) < 0;
  });
  return Teams;
}

// Runs a TeamSync operation and waits for it (the dialogs stay responsive)
bool Wait(QWidget * Parent, const QString & Title,
  const std::function<void(const std::function<void(bool, const QString &)> &)> & Operation)
{
  QEventLoop Loop;
  bool Result = false;
  bool Finished = false;
  QString Message;
  QApplication::setOverrideCursor(Qt::BusyCursor);
  Operation([&](bool Ok, const QString & Error)
  {
    Result = Ok;
    Message = Error;
    Finished = true;
    Loop.quit();
  });
  if (!Finished)
  {
    Loop.exec();
  }
  QApplication::restoreOverrideCursor();
  if (!Result && !Message.isEmpty())
  {
    QMessageBox::warning(Parent, Title, Message);
  }
  return Result;
}

// The personal sites (not in a team, not workspaces), for picking
QStringList PersonalSites()
{
  QStringList Result;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (!Data->IsWorkspace && !Data->Name.startsWith(CloudSettings::TeamFolderPrefix))
    {
      Result.append(Data->Name);
    }
  }
  std::sort(Result.begin(), Result.end(), [](const QString & A, const QString & B)
  {
    return StoredSessionList::CompareLogicalText(A, B) < 0;
  });
  return Result;
}

// Fills Tree with Names ("folder/sub/site") as folders and sites; the site
// items carry their full name (Qt::UserRole); folder items "folder/sub/"
void FillTree(QTreeWidget * Tree, const QStringList & Names, bool Checkable)
{
  Tree->clear();
  QHash<QString, QTreeWidgetItem *> Folders;
  std::function<QTreeWidgetItem *(const QString &)> FolderItem = [&](const QString & Path) -> QTreeWidgetItem *
  {
    if (Path.isEmpty())
    {
      return nullptr;
    }
    auto It = Folders.constFind(Path);
    if (It != Folders.cend())
    {
      return *It;
    }
    qsizetype Slash = Path.lastIndexOf(QLatin1Char('/'));
    QTreeWidgetItem * Parent = FolderItem((Slash > 0) ? Path.left(Slash) : QString());
    auto * Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(Tree);
    Item->setText(0, Path.mid(Slash + 1));
    Item->setIcon(0, Icons::Get(QStringLiteral("site-folder")));
    Item->setData(0, Qt::UserRole, Path + QLatin1Char('/'));
    if (Checkable)
    {
      Item->setFlags(Item->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
      Item->setCheckState(0, Qt::Unchecked);
    }
    Folders.insert(Path, Item);
    return Item;
  };
  for (const QString & Name : Names)
  {
    qsizetype Slash = Name.lastIndexOf(QLatin1Char('/'));
    QTreeWidgetItem * Parent = FolderItem((Slash > 0) ? Name.left(Slash) : QString());
    auto * Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(Tree);
    Item->setText(0, Name.mid(Slash + 1));
    Item->setIcon(0, Icons::Get(QStringLiteral("site")));
    Item->setData(0, Qt::UserRole, Name);
    if (Checkable)
    {
      Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
      Item->setCheckState(0, Qt::Unchecked);
    }
  }
  Tree->expandAll();
}

// The checked sites of a tree filled by FillTree
QStringList CheckedSites(QTreeWidget * Tree)
{
  QStringList Result;
  QTreeWidgetItemIterator It(Tree);
  while (*It)
  {
    QString Name = (*It)->data(0, Qt::UserRole).toString();
    if (!Name.endsWith(QLatin1Char('/')) && ((*It)->checkState(0) == Qt::Checked))
    {
      Result.append(Name);
    }
    ++It;
  }
  return Result;
}

}

//---------------------------------------------------------------------------
// Shared helpers

bool TeamsDialog::TeamSite(const QString & SiteName, QString & TeamUuid, QString & Relative)
{
  for (const TeamSync::Team & T : TeamSync::Instance().Teams())
  {
    QString Prefix = TeamSync::FolderName(T.Name) + QLatin1Char('/');
    if (SiteName.startsWith(Prefix))
    {
      TeamUuid = T.Uuid;
      Relative = SiteName.mid(Prefix.size());
      return true;
    }
  }
  return false;
}

bool TeamsDialog::ShareSites(QWidget * Parent, const QStringList & Names, const QString & Subfolder, const QString & TeamUuid)
{
  const QString Title = QStringLiteral("Share with Team");
  if (!TeamSync::Instance().Available())
  {
    QMessageBox::information(Parent, Title, QStringLiteral("Sign in to NixSCP Pro and unlock cloud sync to share sites with a team."));
    return false;
  }
  QList<TeamSync::Team> Teams;
  for (const TeamSync::Team & T : SortedTeams())
  {
    if (T.CanEdit() && !T.Key.isEmpty())
    {
      Teams.append(T);
    }
  }
  QDialog Dialog(Parent);
  Dialog.setWindowTitle(Title);
  auto * Layout = new QVBoxLayout(&Dialog);
  QString What = (Names.size() == 1)
    ? QStringLiteral("Share the site <b>%1</b> with a team.").arg(Names.first().section(QLatin1Char('/'), -1).toHtmlEscaped())
    : QStringLiteral("Share %1 sites with a team.").arg(Names.size());
  auto * Intro = new QLabel(What + QStringLiteral(" Every member gets it with its stored password, end-to-end encrypted. "
    "You can stop sharing it at any time."), &Dialog);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  auto * Form = new QFormLayout();
  auto * Team = new QComboBox(&Dialog);
  for (const TeamSync::Team & T : Teams)
  {
    Team->addItem(T.Name, T.Uuid);
    if (T.Uuid == TeamUuid)
    {
      Team->setCurrentIndex(Team->count() - 1);
    }
  }
  Team->addItem(QStringLiteral("New team..."), QString());
  auto * Folder = new QComboBox(&Dialog);
  Folder->setEditable(true);
  Folder->setToolTip(QStringLiteral("A folder inside the team's folder; leave empty for the team's top level"));
  auto FillFolders = [Folder, Team, Subfolder]()
  {
    QString Current = Folder->currentText();
    Folder->clear();
    Folder->addItem(QString());
    QSet<QString> Seen;
    for (const QString & Name : TeamSync::Instance().SharedSites(Team->currentData().toString()))
    {
      qsizetype Slash = Name.lastIndexOf(QLatin1Char('/'));
      if ((Slash > 0) && !Seen.contains(Name.left(Slash)))
      {
        Seen.insert(Name.left(Slash));
        Folder->addItem(Name.left(Slash));
      }
    }
    Folder->setCurrentText(Current.isEmpty() ? Subfolder : Current);
  };
  FillFolders();
  connect(Team, &QComboBox::currentIndexChanged, &Dialog, [&Dialog, Team, FillFolders]()
  {
    if (Team->currentData().toString().isEmpty() && (Team->currentIndex() == Team->count() - 1))
    {
      QString Uuid = NewTeam(&Dialog);
      if (!Uuid.isEmpty())
      {
        const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
        Team->insertItem(Team->count() - 1, T ? T->Name : QString(), Uuid);
        Team->setCurrentIndex(Team->count() - 2);
      }
      else
      {
        Team->setCurrentIndex(0);
      }
    }
    FillFolders();
  });
  Form->addRow(QStringLiteral("&Team:"), Team);
  Form->addRow(QStringLiteral("In &folder:"), Folder);
  Layout->addLayout(Form);
  auto * Keep = new QCheckBox(QStringLiteral("&Keep my own copy in my sites as well"), &Dialog);
  Keep->setToolTip(QStringLiteral("Unticked, the site moves into the team's folder (one copy, the shared one)"));
  Layout->addWidget(Keep);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
  Buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Share"));
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Layout->addWidget(Buttons);
  auto Update = [Buttons, Team]() { Buttons->button(QDialogButtonBox::Ok)->setEnabled(!Team->currentData().toString().isEmpty()); };
  connect(Team, &QComboBox::currentIndexChanged, &Dialog, Update);
  Update();
  if (Teams.isEmpty())
  {
    // straight to creating the first team
    Team->setCurrentIndex(0);
    QString Uuid = NewTeam(Parent);
    if (Uuid.isEmpty())
    {
      return false;
    }
    const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
    Team->insertItem(0, T ? T->Name : QString(), Uuid);
    Team->setCurrentIndex(0);
  }
  Dialog.resize(480, Dialog.sizeHint().height());
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  QString Uuid = Team->currentData().toString();
  QString Sub = Folder->currentText();
  bool Move = !Keep->isChecked();
  return Wait(Parent, Title, [Uuid, Names, Sub, Move](const auto & Done) { TeamSync::Instance().ShareSites(Uuid, Names, Sub, Move, Done); });
}

bool TeamsDialog::StopSharing(QWidget * Parent, const QString & TeamUuid, const QStringList & Names)
{
  const TeamSync::Team * T = TeamSync::Instance().Find(TeamUuid);
  if ((T == nullptr) || Names.isEmpty())
  {
    return false;
  }
  const QString Title = QStringLiteral("Stop Sharing");
  if (!T->CanEdit())
  {
    QMessageBox::information(Parent, Title, QStringLiteral("Only the owner and the admins of the team \"%1\" can stop sharing its sites.").arg(T->Name));
    return false;
  }
  QString What = (Names.size() == 1)
    ? (Names.first().endsWith(QLatin1Char('/'))
        ? QStringLiteral("the folder <b>%1</b>").arg(Names.first().chopped(1).toHtmlEscaped())
        : QStringLiteral("<b>%1</b>").arg(Names.first().section(QLatin1Char('/'), -1).toHtmlEscaped()))
    : QStringLiteral("%1 items").arg(Names.size());
  QMessageBox Box(QMessageBox::Question, Title,
    QStringLiteral("Stop sharing %1 with the team <b>%2</b>?<br><br>The members lose it at their next sync. "
      "They could have copied the stored passwords while it was shared: change the passwords that matter.")
      .arg(What, T->Name.toHtmlEscaped()), QMessageBox::Cancel, Parent);
  QPushButton * Keep = Box.addButton(QStringLiteral("Stop Sharing, &Keep My Copy"), QMessageBox::AcceptRole);
  QPushButton * Delete = Box.addButton(QStringLiteral("Stop Sharing and &Delete"), QMessageBox::DestructiveRole);
  Box.setDefaultButton(Keep);
  Box.exec();
  if ((Box.clickedButton() != Keep) && (Box.clickedButton() != Delete))
  {
    return false;
  }
  bool KeepCopy = (Box.clickedButton() == Keep);
  return Wait(Parent, Title, [TeamUuid, Names, KeepCopy](const auto & Done) { TeamSync::Instance().Unshare(TeamUuid, Names, KeepCopy, Done); });
}

QString TeamsDialog::NewTeam(QWidget * Parent)
{
  bool Ok = false;
  QString Name = QInputDialog::getText(Parent, QStringLiteral("New Team"),
    QStringLiteral("Team &name (your colleagues see it too):"), QLineEdit::Normal, QString(), &Ok).trimmed();
  if (!Ok || Name.isEmpty())
  {
    return QString();
  }
  QSet<QString> Before;
  for (const TeamSync::Team & T : TeamSync::Instance().Teams()) Before.insert(T.Uuid);
  if (!Wait(Parent, QStringLiteral("New Team"), [Name](const auto & Done) { TeamSync::Instance().CreateTeam(Name, Done); }))
  {
    return QString();
  }
  for (const TeamSync::Team & T : TeamSync::Instance().Teams())
  {
    if (!Before.contains(T.Uuid)) return T.Uuid;
  }
  return QString();
}

QString TeamsDialog::Join(QWidget * Parent)
{
  bool Ok = false;
  QString Code = QInputDialog::getText(Parent, QStringLiteral("Join a Team"),
    QStringLiteral("Invite &code (as you received it from the team's owner or admin):"), QLineEdit::Normal, QString(), &Ok).trimmed();
  if (!Ok || Code.isEmpty())
  {
    return QString();
  }
  QSet<QString> Before;
  for (const TeamSync::Team & T : TeamSync::Instance().Teams()) Before.insert(T.Uuid);
  if (!Wait(Parent, QStringLiteral("Join a Team"), [Code](const auto & Done) { TeamSync::Instance().JoinWithCode(Code, Done); }))
  {
    return QString();
  }
  for (const TeamSync::Team & T : TeamSync::Instance().Teams())
  {
    if (!Before.contains(T.Uuid))
    {
      QMessageBox::information(Parent, QStringLiteral("Join a Team"),
        QStringLiteral("You joined the team \"%1\". Its sites are in the folder \"%2\" of your sites.").arg(T.Name, TeamSync::FolderName(T.Name)));
      return T.Uuid;
    }
  }
  return QString();
}

void TeamsDialog::SetupMenu(QMenu * Menu, QWidget * Window, const std::function<QString()> & CurrentSite)
{
  auto Fill = [Menu, Window, CurrentSite]()
  {
    Menu->clear();
    TeamSync & Teams = TeamSync::Instance();
    Menu->addAction(QStringLiteral("&Manage Teams..."), Window, [Window]() { Execute(Window); });
    if (!Teams.Available())
    {
      Menu->addSeparator();
      Menu->addAction(QStringLiteral("Sign in to NixSCP Pro (Pro menu) to use teams"))->setEnabled(false);
      return;
    }
    QString Site = CurrentSite ? CurrentSite() : QString();
    QString TeamUuid, Relative;
    bool InTeam = !Site.isEmpty() && TeamSite(Site, TeamUuid, Relative);
    const TeamSync::Team * SiteTeam = InTeam ? Teams.Find(TeamUuid) : nullptr;
    if (InTeam && (SiteTeam != nullptr))
    {
      QAction * Stop = Menu->addAction(QStringLiteral("&Stop Sharing Current Site..."), Window,
        [Window, TeamUuid, Relative]() { StopSharing(Window, TeamUuid, {Relative}); });
      Stop->setEnabled(SiteTeam->CanEdit());
      Stop->setToolTip(QStringLiteral("Shared in the team %1").arg(SiteTeam->Name));
    }
    else
    {
      QAction * Share = Menu->addAction(QStringLiteral("&Share Current Site with Team..."), Window,
        [Window, Site]() { ShareSites(Window, {Site}); });
      Share->setEnabled(!Site.isEmpty());
    }
    QList<TeamSync::Team> List = SortedTeams();
    if (!List.isEmpty())
    {
      Menu->addSection(QStringLiteral("Your Teams"));
      for (const TeamSync::Team & T : List)
      {
        QString Uuid = T.Uuid;
        Menu->addAction(QStringLiteral("%1 (%2, %3 shared)").arg(T.Name, RoleText(T.Role).toLower())
          .arg(Teams.SharedSites(T.Uuid).size()), Window, [Window, Uuid]() { Execute(Window, Uuid); });
      }
    }
    Menu->addSeparator();
    Menu->addAction(QStringLiteral("&New Team..."), Window, [Window]()
    {
      QString Uuid = NewTeam(Window);
      if (!Uuid.isEmpty()) Execute(Window, Uuid);
    });
    Menu->addAction(QStringLiteral("&Join Team with Code..."), Window, [Window]() { Join(Window); });
    Menu->addAction(QStringLiteral("Sync &Teams Now"), Window, []() { TeamSync::Instance().SyncAll(); });
  };
  QObject::connect(Menu, &QMenu::aboutToShow, Menu, Fill);
  // Keyboard navigation needs entries before the first opening
  Fill();
}

//---------------------------------------------------------------------------
// The dialog

TeamsDialog::TeamsDialog(QWidget * Parent, const QString & SelectUuid) : QDialog(Parent)
{
  setWindowTitle(QStringLiteral("Teams"));
  auto * Layout = new QVBoxLayout(this);

  auto * Top = new QHBoxLayout();
  auto * Intro = new QLabel(QStringLiteral("Share sites with your colleagues, end-to-end encrypted. Owners and admins share and edit "
    "a team's sites; members use them. A team's sites are in the folder <b>Team: &lt;name&gt;</b> of your sites."), this);
  Intro->setWordWrap(true);
  Top->addWidget(Intro, 1);
  Layout->addLayout(Top);

  auto * Splitter = new QSplitter(this);
  // left: the teams
  auto * Left = new QWidget(Splitter);
  auto * LeftLayout = new QVBoxLayout(Left);
  LeftLayout->setContentsMargins(0, 0, 0, 0);
  FTeams = new QListWidget(Left);
  FTeams->setMinimumWidth(180);
  LeftLayout->addWidget(FTeams, 1);
  auto * New = new QPushButton(QStringLiteral("&New Team..."), Left);
  auto * JoinButton = new QPushButton(QStringLiteral("&Join with Code..."), Left);
  LeftLayout->addWidget(New);
  LeftLayout->addWidget(JoinButton);

  // right: the selected team
  auto * Right = new QWidget(Splitter);
  auto * RightLayout = new QVBoxLayout(Right);
  RightLayout->setContentsMargins(0, 0, 0, 0);
  FEmpty = new QLabel(Right);
  FEmpty->setWordWrap(true);
  FEmpty->setAlignment(Qt::AlignCenter);
  RightLayout->addWidget(FEmpty, 1);
  FDetails = new QWidget(Right);
  auto * DetailsLayout = new QVBoxLayout(FDetails);
  DetailsLayout->setContentsMargins(0, 0, 0, 0);
  auto * Header = new QHBoxLayout();
  auto * Titles = new QVBoxLayout();
  FTitle = new QLabel(FDetails);
  QFont TitleFont = FTitle->font();
  TitleFont.setBold(true);
  TitleFont.setPointSizeF(TitleFont.pointSizeF() * 1.25);
  FTitle->setFont(TitleFont);
  FSubtitle = new QLabel(FDetails);
  Titles->addWidget(FTitle);
  Titles->addWidget(FSubtitle);
  Header->addLayout(Titles, 1);
  FTeamMenu = new QToolButton(FDetails);
  FTeamMenu->setText(QStringLiteral("&Team"));
  FTeamMenu->setPopupMode(QToolButton::InstantPopup);
  FTeamMenu->setToolButtonStyle(Qt::ToolButtonTextOnly);
  Header->addWidget(FTeamMenu, 0, Qt::AlignTop);
  DetailsLayout->addLayout(Header);

  FTabs = new QTabWidget(FDetails);
  // Shared sites
  auto * SitesPage = new QWidget(FTabs);
  auto * SitesLayout = new QVBoxLayout(SitesPage);
  FSites = new QTreeWidget(SitesPage);
  FSites->setHeaderHidden(true);
  FSites->setSelectionMode(QAbstractItemView::ExtendedSelection);
  SitesLayout->addWidget(FSites, 1);
  FSitesHint = new QLabel(SitesPage);
  FSitesHint->setWordWrap(true);
  SitesLayout->addWidget(FSitesHint);
  auto * SitesButtons = new QHBoxLayout();
  FShareSites = new QPushButton(QStringLiteral("Share &Sites..."), SitesPage);
  FShareFolder = new QPushButton(QStringLiteral("Share &Folder..."), SitesPage);
  FUnshare = new QPushButton(QStringLiteral("Stop &Sharing..."), SitesPage);
  SitesButtons->addWidget(FShareSites);
  SitesButtons->addWidget(FShareFolder);
  SitesButtons->addStretch();
  SitesButtons->addWidget(FUnshare);
  SitesLayout->addLayout(SitesButtons);
  FTabs->addTab(SitesPage, QStringLiteral("Shared Sites"));
  // Members
  auto * MembersPage = new QWidget(FTabs);
  auto * MembersLayout = new QVBoxLayout(MembersPage);
  FMembers = NewTable(MembersPage, {QStringLiteral("E-mail"), QStringLiteral("Role"), QStringLiteral("Joined")});
  MembersLayout->addWidget(FMembers, 1);
  auto * MembersButtons = new QHBoxLayout();
  FInvite = new QPushButton(QStringLiteral("&Invite..."), MembersPage);
  FMakeAdmin = new QPushButton(QStringLiteral("Make &Admin"), MembersPage);
  FMakeMember = new QPushButton(QStringLiteral("Make &Member"), MembersPage);
  FRemoveMember = new QPushButton(QStringLiteral("&Remove..."), MembersPage);
  MembersButtons->addWidget(FInvite);
  MembersButtons->addStretch();
  MembersButtons->addWidget(FMakeAdmin);
  MembersButtons->addWidget(FMakeMember);
  MembersButtons->addWidget(FRemoveMember);
  MembersLayout->addLayout(MembersButtons);
  FTabs->addTab(MembersPage, QStringLiteral("Members"));
  // Invites
  auto * InvitesPage = new QWidget(FTabs);
  auto * InvitesLayout = new QVBoxLayout(InvitesPage);
  FInvites = NewTable(InvitesPage, {QStringLiteral("Code id"), QStringLiteral("Role"), QStringLiteral("Created by"), QStringLiteral("Expires")});
  InvitesLayout->addWidget(FInvites, 1);
  FInvitesHint = new QLabel(QStringLiteral("Codes not used yet. Each works once; revoke the ones you no longer need."), InvitesPage);
  FInvitesHint->setWordWrap(true);
  InvitesLayout->addWidget(FInvitesHint);
  auto * InvitesButtons = new QHBoxLayout();
  auto * InviteToo = new QPushButton(QStringLiteral("&New Invite..."), InvitesPage);
  FRevoke = new QPushButton(QStringLiteral("Re&voke"), InvitesPage);
  InvitesButtons->addWidget(InviteToo);
  InvitesButtons->addStretch();
  InvitesButtons->addWidget(FRevoke);
  InvitesLayout->addLayout(InvitesButtons);
  FTabs->addTab(InvitesPage, QStringLiteral("Invites"));
  DetailsLayout->addWidget(FTabs, 1);
  RightLayout->addWidget(FDetails, 1);
  Splitter->addWidget(Left);
  Splitter->addWidget(Right);
  Splitter->setStretchFactor(1, 1);
  Layout->addWidget(Splitter, 1);

  FStatus = new QLabel(this);
  FStatus->setWordWrap(true);
  Layout->addWidget(FStatus);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  QPushButton * SyncNow = Buttons->addButton(QStringLiteral("S&ync Now"), QDialogButtonBox::ActionRole);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addWidget(Buttons);

  // the team menu
  auto * TeamMenu = new QMenu(FTeamMenu);
  connect(TeamMenu, &QMenu::aboutToShow, this, [this, TeamMenu]()
  {
    TeamMenu->clear();
    const TeamSync::Team * T = TeamSync::Instance().Find(SelectedUuid());
    if (T == nullptr) return;
    bool Owner = (T->Role == QStringLiteral("owner"));
    TeamMenu->addAction(QStringLiteral("&Rename..."), this, [this]() { Rename(); })->setEnabled(T->CanEdit());
    TeamMenu->addSeparator();
    if (Owner)
    {
      TeamMenu->addAction(QStringLiteral("&Delete Team..."), this, [this]() { Delete(); });
    }
    else
    {
      TeamMenu->addAction(QStringLiteral("&Leave Team..."), this, [this]() { Leave(); });
    }
  });
  FTeamMenu->setMenu(TeamMenu);

  connect(New, &QPushButton::clicked, this, [this]()
  {
    QString Uuid = NewTeam(this);
    Reload();
    if (!Uuid.isEmpty())
    {
      for (int R = 0; R < FTeams->count(); R++)
        if (FTeams->item(R)->data(Qt::UserRole).toString() == Uuid) FTeams->setCurrentRow(R);
    }
  });
  connect(JoinButton, &QPushButton::clicked, this, [this]()
  {
    QString Uuid = Join(this);
    Reload();
    if (!Uuid.isEmpty())
    {
      for (int R = 0; R < FTeams->count(); R++)
        if (FTeams->item(R)->data(Qt::UserRole).toString() == Uuid) FTeams->setCurrentRow(R);
    }
  });
  connect(SyncNow, &QPushButton::clicked, this, [this]()
  {
    Run([](const auto & Done) { TeamSync::Instance().SyncAll(Done); });
    LoadMembers(true);
  });
  connect(FShareSites, &QPushButton::clicked, this, [this]() { ShareSitesClicked(); });
  connect(FShareFolder, &QPushButton::clicked, this, [this]() { ShareFolderClicked(); });
  connect(FUnshare, &QPushButton::clicked, this, [this]() { UnshareClicked(); });
  connect(FInvite, &QPushButton::clicked, this, [this]() { Invite(); });
  connect(InviteToo, &QPushButton::clicked, this, [this]() { Invite(); });
  connect(FMakeAdmin, &QPushButton::clicked, this, [this]() { SetRole(QStringLiteral("admin")); });
  connect(FMakeMember, &QPushButton::clicked, this, [this]() { SetRole(QStringLiteral("member")); });
  connect(FRemoveMember, &QPushButton::clicked, this, [this]() { RemoveMember(); });
  connect(FRevoke, &QPushButton::clicked, this, [this]()
  {
    QList<QTableWidgetItem *> Items = FInvites->selectedItems();
    if (Items.isEmpty()) return;
    QString Id = Items.first()->data(Qt::UserRole).toString();
    QString Uuid = SelectedUuid();
    Run([Uuid, Id](const auto & Done) { TeamSync::Instance().RevokeInvite(Uuid, Id, Done); });
    LoadMembers(true);
  });
  connect(FTeams, &QListWidget::currentRowChanged, this, [this]()
  {
    ReloadSites();
    LoadMembers();
    UpdateButtons();
  });
  connect(FSites, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
  connect(FMembers, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
  connect(FInvites, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
  connect(&TeamSync::Instance(), &TeamSync::Changed, this, [this]() { Reload(); });
  connect(&Configuration::Instance(), &Configuration::StoredSessionsChanged, this, [this]() { ReloadSites(); });

  Reload();
  for (int R = 0; R < FTeams->count(); R++)
  {
    if (FTeams->item(R)->data(Qt::UserRole).toString() == SelectUuid)
    {
      FTeams->setCurrentRow(R);
    }
  }
  if ((FTeams->currentRow() < 0) && (FTeams->count() > 0))
  {
    FTeams->setCurrentRow(0);
  }
  resize(860, 520);
  if (!TeamSync::Instance().Available())
  {
    for (QPushButton * B : {New, JoinButton, SyncNow})
    {
      B->setEnabled(false);
    }
  }
  else
  {
    Run([](const auto & Done) { TeamSync::Instance().SyncAll(Done); });
  }
}

void TeamsDialog::Execute(QWidget * Parent, const QString & SelectUuid)
{
  TeamsDialog Dialog(Parent, SelectUuid);
  Dialog.exec();
}

void TeamsDialog::Reload()
{
  QString Selected = SelectedUuid();
  QSignalBlocker Block(FTeams);
  FTeams->clear();
  for (const TeamSync::Team & T : SortedTeams())
  {
    auto * Item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3 %4").arg(T.Name, RoleText(T.Role))
      .arg(T.MemberCount).arg((T.MemberCount == 1) ? QStringLiteral("person") : QStringLiteral("people")), FTeams);
    Item->setData(Qt::UserRole, T.Uuid);
    Item->setIcon(Icons::Get(QStringLiteral("site-folder")));
    if (T.Uuid == Selected)
    {
      FTeams->setCurrentItem(Item);
    }
  }
  if ((FTeams->currentRow() < 0) && (FTeams->count() > 0))
  {
    FTeams->setCurrentRow(0);
  }
  if (!TeamSync::Instance().Available())
  {
    FEmpty->setText(QStringLiteral("<b>Sign in to NixSCP Pro and unlock cloud sync</b> (Pro &gt; Cloud Sync Settings) to use teams."));
  }
  else
  {
    FEmpty->setText(QStringLiteral("You are not in a team yet.<br><br>Create one with <b>New Team</b> and invite your colleagues, "
      "or join a colleague's team with the invite code they gave you."));
  }
  FStatus->setText(TeamSync::Instance().LastError().toHtmlEscaped());
  ReloadSites();
  LoadMembers();
  UpdateButtons();
}

QString TeamsDialog::SelectedUuid() const
{
  QListWidgetItem * Item = FTeams->currentItem();
  return Item ? Item->data(Qt::UserRole).toString() : QString();
}

void TeamsDialog::ReloadSites()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  FEmpty->setVisible(T == nullptr);
  FDetails->setVisible(T != nullptr);
  if (T == nullptr)
  {
    FSites->clear();
    return;
  }
  FTitle->setText(T->Name);
  FSubtitle->setText(QStringLiteral("You are %1 · %2 %3 · folder \"%4\"").arg(RoleText(T->Role).toLower())
    .arg(T->MemberCount).arg((T->MemberCount == 1) ? QStringLiteral("person") : QStringLiteral("people"), TeamSync::FolderName(T->Name)));
  QStringList Shared = TeamSync::Instance().SharedSites(Uuid);
  FillTree(FSites, Shared, false);
  FTabs->setTabText(0, QStringLiteral("Shared Sites (%1)").arg(Shared.size()));
  if (T->Key.isEmpty())
  {
    FSitesHint->setText(QStringLiteral("Waiting for the team key: an owner or admin's NixSCP hands it over at its next sync."));
  }
  else if (Shared.isEmpty())
  {
    FSitesHint->setText(T->CanEdit() ? QStringLiteral("Nothing is shared yet. Share single sites or a whole folder; every member gets them.")
                                     : QStringLiteral("Nothing is shared with this team yet."));
  }
  else
  {
    FSitesHint->setText(T->CanEdit() ? QStringLiteral("Changes you make to these sites are shared with the team.")
                                     : QStringLiteral("You can use these sites; only the owner and admins can change them."));
  }
}

void TeamsDialog::LoadMembers(bool Force)
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    FMembers->setRowCount(0);
    FInvites->setRowCount(0);
    FLoadedMembers.clear();
    return;
  }
  if (!Force && (FLoadedMembers == Uuid))
  {
    return;
  }
  FLoadedMembers = Uuid;
  bool Edit = T->CanEdit();
  FTabs->setTabVisible(2, Edit);
  QPointer<TeamsDialog> Guard(this);
  TeamSync::Instance().Members(Uuid, [Guard, Uuid](bool Ok, const QString &, const QJsonArray & List)
  {
    if ((Guard == nullptr) || !Ok || (Guard->SelectedUuid() != Uuid)) return;
    QTableWidget * Members = Guard->FMembers;
    Members->setRowCount(0);
    for (const QJsonValue & V : List)
    {
      QJsonObject M = V.toObject();
      int R = Members->rowCount();
      Members->insertRow(R);
      QStringList Cells{M.value(QStringLiteral("email")).toString(), RoleText(M.value(QStringLiteral("role")).toString()),
        M.value(QStringLiteral("joined_at")).toString().left(10)};
      for (int C = 0; C < Cells.size(); C++)
      {
        auto * Item = new QTableWidgetItem(Cells.at(C));
        Item->setData(Qt::UserRole, M.value(QStringLiteral("user_uuid")).toString());
        Item->setData(Qt::UserRole + 1, M.value(QStringLiteral("role")).toString());
        Members->setItem(R, C, Item);
      }
    }
    Members->resizeColumnsToContents();
    Guard->FTabs->setTabText(1, QStringLiteral("Members (%1)").arg(List.size()));
    Guard->UpdateButtons();
  });
  if (Edit)
  {
    TeamSync::Instance().Invites(Uuid, [Guard, Uuid](bool Ok, const QString &, const QJsonArray & List)
    {
      if ((Guard == nullptr) || !Ok || (Guard->SelectedUuid() != Uuid)) return;
      QTableWidget * Invites = Guard->FInvites;
      Invites->setRowCount(0);
      for (const QJsonValue & V : List)
      {
        QJsonObject I = V.toObject();
        int R = Invites->rowCount();
        Invites->insertRow(R);
        QStringList Cells{I.value(QStringLiteral("id")).toString(), RoleText(I.value(QStringLiteral("role")).toString()),
          I.value(QStringLiteral("created_by_email")).toString(), I.value(QStringLiteral("expires_at")).toString().left(10)};
        for (int C = 0; C < Cells.size(); C++)
        {
          auto * Item = new QTableWidgetItem(Cells.at(C));
          Item->setData(Qt::UserRole, I.value(QStringLiteral("id")).toString());
          Invites->setItem(R, C, Item);
        }
      }
      Invites->resizeColumnsToContents();
      Guard->FTabs->setTabText(2, QStringLiteral("Invites (%1)").arg(List.size()));
      Guard->UpdateButtons();
    });
  }
}

QStringList TeamsDialog::SelectedShares() const
{
  QStringList Result;
  for (QTreeWidgetItem * Item : FSites->selectedItems())
  {
    Result.append(Item->data(0, Qt::UserRole).toString());
  }
  return Result;
}

void TeamsDialog::UpdateButtons()
{
  const TeamSync::Team * T = TeamSync::Instance().Find(SelectedUuid());
  bool Edit = (T != nullptr) && T->CanEdit() && !T->Key.isEmpty();
  bool Owner = (T != nullptr) && (T->Role == QStringLiteral("owner"));
  FShareSites->setVisible((T != nullptr) && T->CanEdit());
  FShareFolder->setVisible((T != nullptr) && T->CanEdit());
  FUnshare->setVisible((T != nullptr) && T->CanEdit());
  FShareSites->setEnabled(Edit);
  FShareFolder->setEnabled(Edit);
  FUnshare->setEnabled(Edit && !FSites->selectedItems().isEmpty());
  FInvite->setEnabled(Edit);
  QString Role;
  QList<QTableWidgetItem *> Members = FMembers->selectedItems();
  if (!Members.isEmpty())
  {
    Role = Members.first()->data(Qt::UserRole + 1).toString();
  }
  bool Admin = (T != nullptr) && (T->Role == QStringLiteral("admin"));
  FMakeAdmin->setEnabled(Owner && (Role == QStringLiteral("member")));
  FMakeMember->setEnabled(Owner && (Role == QStringLiteral("admin")));
  FRemoveMember->setEnabled(!Role.isEmpty() && (Role != QStringLiteral("owner")) && (Owner || (Admin && (Role == QStringLiteral("member")))));
  for (QPushButton * B : {FMakeAdmin, FMakeMember, FRemoveMember})
  {
    B->setVisible((T != nullptr) && T->CanEdit());
  }
  FRevoke->setEnabled(Edit && !FInvites->selectedItems().isEmpty());
}

void TeamsDialog::Run(const std::function<void(const std::function<void(bool, const QString &)> &)> & Operation)
{
  QApplication::setOverrideCursor(Qt::BusyCursor);
  QPointer<TeamsDialog> Guard(this);
  Operation([Guard](bool Ok, const QString & Error)
  {
    QApplication::restoreOverrideCursor();
    if (Guard == nullptr)
    {
      return;
    }
    Guard->Reload();
    if (!Ok && !Error.isEmpty())
    {
      QMessageBox::warning(Guard, Guard->windowTitle(), Error);
    }
  });
}

void TeamsDialog::ShareSitesClicked()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  QStringList Personal = PersonalSites();
  if (Personal.isEmpty())
  {
    QMessageBox::information(this, windowTitle(), QStringLiteral("You have no sites of your own to share yet."));
    return;
  }
  QDialog Pick(this);
  Pick.setWindowTitle(QStringLiteral("Share Sites with %1").arg(T->Name));
  auto * Layout = new QVBoxLayout(&Pick);
  auto * Intro = new QLabel(QStringLiteral("Tick the sites to share. Every member of <b>%1</b> gets them with their stored passwords.")
    .arg(T->Name.toHtmlEscaped()), &Pick);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  auto * Tree = new QTreeWidget(&Pick);
  Tree->setHeaderHidden(true);
  FillTree(Tree, Personal, true);
  Layout->addWidget(Tree, 1);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Pick);
  Buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Next..."));
  connect(Buttons, &QDialogButtonBox::accepted, &Pick, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Pick, &QDialog::reject);
  Layout->addWidget(Buttons);
  auto Update = [Buttons, Tree]() { Buttons->button(QDialogButtonBox::Ok)->setEnabled(!CheckedSites(Tree).isEmpty()); };
  connect(Tree, &QTreeWidget::itemChanged, &Pick, Update);
  Update();
  Pick.resize(440, 480);
  if (Pick.exec() == QDialog::Accepted)
  {
    ShareSites(this, CheckedSites(Tree), QString(), Uuid);
    Reload();
  }
}

void TeamsDialog::ShareFolderClicked()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  QSet<QString> Folders;
  for (const QString & Name : PersonalSites())
  {
    qsizetype Slash = Name.lastIndexOf(QLatin1Char('/'));
    while (Slash > 0)
    {
      Folders.insert(Name.left(Slash));
      Slash = Name.lastIndexOf(QLatin1Char('/'), Slash - 1);
    }
  }
  if (Folders.isEmpty())
  {
    QMessageBox::information(this, windowTitle(), QStringLiteral("You have no site folders. Use Share Sites to share single sites."));
    return;
  }
  QStringList List = Folders.values();
  std::sort(List.begin(), List.end(), [](const QString & A, const QString & B) { return StoredSessionList::CompareLogicalText(A, B) < 0; });
  bool Ok = false;
  QString Folder = QInputDialog::getItem(this, QStringLiteral("Share Folder with %1").arg(T->Name),
    QStringLiteral("The folder's sites move into the team's folder (Team: %1/<folder>) and every member gets them:").arg(T->Name),
    List, 0, false, &Ok);
  if (Ok && !Folder.isEmpty())
  {
    Run([Uuid, Folder](const auto & Done) { TeamSync::Instance().ShareFolder(Uuid, Folder, Done); });
  }
}

void TeamsDialog::UnshareClicked()
{
  if (StopSharing(this, SelectedUuid(), SelectedShares()))
  {
    Reload();
  }
}

void TeamsDialog::Invite()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  QDialog Dialog(this);
  Dialog.setWindowTitle(QStringLiteral("Invite to %1").arg(T->Name));
  auto * Form = new QFormLayout(&Dialog);
  auto * Role = new QComboBox(&Dialog);
  Role->addItem(QStringLiteral("Member (uses the sites)"), QStringLiteral("member"));
  if (T->Role == QStringLiteral("owner"))
  {
    Role->addItem(QStringLiteral("Admin (shares and edits sites, invites people)"), QStringLiteral("admin"));
  }
  auto * Days = new QSpinBox(&Dialog);
  Days->setRange(1, 30);
  Days->setValue(7);
  Days->setSuffix(QStringLiteral(" days"));
  Form->addRow(QStringLiteral("&Role:"), Role);
  Form->addRow(QStringLiteral("&Valid for:"), Days);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
  Buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create Code"));
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Form->addRow(Buttons);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return;
  }
  QString RoleValue = Role->currentData().toString();
  int DaysValue = Days->value();
  QApplication::setOverrideCursor(Qt::BusyCursor);
  QPointer<TeamsDialog> Guard(this);
  QString TeamName = T->Name;
  TeamSync::Instance().CreateInvite(Uuid, RoleValue, DaysValue, [Guard, TeamName, DaysValue](bool Ok, const QString & Error, const QString & Code)
  {
    QApplication::restoreOverrideCursor();
    if (Guard == nullptr)
    {
      return;
    }
    if (!Ok)
    {
      QMessageBox::warning(Guard, Guard->windowTitle(), Error);
      return;
    }
    Guard->LoadMembers(true);
    QDialog Show(Guard);
    Show.setWindowTitle(QStringLiteral("Invite Code"));
    auto * V = new QVBoxLayout(&Show);
    auto * Text = new QLabel(QStringLiteral("Give this code to the person you invite to <b>%1</b>, privately (chat, in person). "
      "They enter it in NixSCP: <b>Teams &gt; Join Team with Code</b> (on Android: Teams, Join). It works once and expires in %2 days. "
      "NixSCP does not keep it: copy it now.").arg(TeamName.toHtmlEscaped()).arg(DaysValue), &Show);
    Text->setWordWrap(true);
    V->addWidget(Text);
    auto * Edit = new QLineEdit(Code, &Show);
    Edit->setReadOnly(true);
    Edit->setFont(QFont(QStringLiteral("monospace")));
    V->addWidget(Edit);
    auto * Box = new QDialogButtonBox(QDialogButtonBox::Close, &Show);
    QPushButton * Copy = Box->addButton(QStringLiteral("&Copy"), QDialogButtonBox::ActionRole);
    connect(Copy, &QPushButton::clicked, &Show, [Code]() { QGuiApplication::clipboard()->setText(Code); });
    connect(Box, &QDialogButtonBox::rejected, &Show, &QDialog::reject);
    V->addWidget(Box);
    Show.resize(560, Show.sizeHint().height());
    Show.exec();
  });
}

void TeamsDialog::SetRole(const QString & Role)
{
  QList<QTableWidgetItem *> Items = FMembers->selectedItems();
  if (Items.isEmpty()) return;
  QString User = Items.first()->data(Qt::UserRole).toString();
  QString Uuid = SelectedUuid();
  Run([Uuid, User, Role](const auto & Done) { TeamSync::Instance().SetRole(Uuid, User, Role, Done); });
  LoadMembers(true);
}

void TeamsDialog::RemoveMember()
{
  QList<QTableWidgetItem *> Items = FMembers->selectedItems();
  if (Items.isEmpty()) return;
  QString User = Items.first()->data(Qt::UserRole).toString();
  QString Email = FMembers->item(Items.first()->row(), 0)->text();
  QString Uuid = SelectedUuid();
  if (QMessageBox::question(this, windowTitle(), QStringLiteral("Remove %1 from the team?\n\nThe team's sites are removed "
        "from their NixSCP and the team key is renewed. They could have copied the server passwords while they were a "
        "member: change the passwords that matter.").arg(Email)) == QMessageBox::Yes)
  {
    Run([Uuid, User](const auto & Done) { TeamSync::Instance().RemoveMember(Uuid, User, Done); });
    LoadMembers(true);
  }
}

void TeamsDialog::Rename()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  bool Ok = false;
  QString Name = QInputDialog::getText(this, QStringLiteral("Rename Team"), QStringLiteral("Team &name:"), QLineEdit::Normal, T->Name, &Ok).trimmed();
  if (Ok && !Name.isEmpty() && (Name != T->Name))
  {
    Run([Uuid, Name](const auto & Done) { TeamSync::Instance().Rename(Uuid, Name, Done); });
  }
}

void TeamsDialog::Leave()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if ((T != nullptr) &&
      (QMessageBox::question(this, windowTitle(), QStringLiteral("Leave the team \"%1\"? Its sites are removed from this computer.").arg(T->Name)) ==
       QMessageBox::Yes))
  {
    Run([Uuid](const auto & Done) { TeamSync::Instance().Leave(Uuid, Done); });
  }
}

void TeamsDialog::Delete()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if ((T != nullptr) &&
      (QMessageBox::question(this, windowTitle(), QStringLiteral("Delete the team \"%1\" for everyone? All members lose its sites, "
         "and its synced data is deleted. This cannot be undone.").arg(T->Name)) == QMessageBox::Yes))
  {
    Run([Uuid](const auto & Done) { TeamSync::Instance().DeleteTeam(Uuid, Done); });
  }
}
