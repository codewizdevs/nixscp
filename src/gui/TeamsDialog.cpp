#include "gui/TeamsDialog.h"

#include "core/CloudSettings.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamSync.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
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

}

TeamsDialog::TeamsDialog(QWidget * Parent) : QDialog(Parent)
{
  setWindowTitle(QStringLiteral("Teams"));
  auto * Layout = new QVBoxLayout(this);
  auto * Intro = new QLabel(QStringLiteral("Share site folders with your colleagues, end-to-end encrypted. A team's sites appear "
    "in the site tree as <b>Team: &lt;name&gt;</b>. Owners and admins edit them; members use them. "
    "Agent access stays each person's own setting."), this);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  FTable = NewTable(this, {QStringLiteral("Team"), QStringLiteral("Your role"), QStringLiteral("Members"), QStringLiteral("Status")});
  Layout->addWidget(FTable);
  FStatus = new QLabel(this);
  FStatus->setWordWrap(true);
  Layout->addWidget(FStatus);
  auto * Row1 = new QHBoxLayout();
  auto * New = new QPushButton(QStringLiteral("&New Team..."), this);
  auto * Join = new QPushButton(QStringLiteral("&Join with Code..."), this);
  auto * SyncNow = new QPushButton(QStringLiteral("S&ync Now"), this);
  Row1->addWidget(New);
  Row1->addWidget(Join);
  Row1->addStretch();
  Row1->addWidget(SyncNow);
  Layout->addLayout(Row1);
  auto * Row2 = new QHBoxLayout();
  FShare = new QPushButton(QStringLiteral("&Add Folder..."), this);
  FInvite = new QPushButton(QStringLiteral("&Invite..."), this);
  FMembers = new QPushButton(QStringLiteral("&Members..."), this);
  FRename = new QPushButton(QStringLiteral("&Rename..."), this);
  FLeave = new QPushButton(QStringLiteral("&Leave"), this);
  FDelete = new QPushButton(QStringLiteral("&Delete"), this);
  for (QPushButton * B : {FShare, FInvite, FMembers, FRename})
  {
    Row2->addWidget(B);
  }
  Row2->addStretch();
  Row2->addWidget(FLeave);
  Row2->addWidget(FDelete);
  Layout->addLayout(Row2);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addWidget(Buttons);

  connect(New, &QPushButton::clicked, this, [this]() { NewTeam(); });
  connect(Join, &QPushButton::clicked, this, [this]() { this->Join(); });
  connect(SyncNow, &QPushButton::clicked, this, [this]() { Run([](const auto & Done) { TeamSync::Instance().SyncAll(Done); }); });
  connect(FShare, &QPushButton::clicked, this, [this]() { ShareFolder(); });
  connect(FInvite, &QPushButton::clicked, this, [this]() { Invite(); });
  connect(FMembers, &QPushButton::clicked, this, [this]() { ShowMembers(); });
  connect(FRename, &QPushButton::clicked, this, [this]() { Rename(); });
  connect(FLeave, &QPushButton::clicked, this, [this]() { Leave(); });
  connect(FDelete, &QPushButton::clicked, this, [this]() { Delete(); });
  connect(FTable, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
  connect(&TeamSync::Instance(), &TeamSync::Changed, this, [this]() { Reload(); });
  Reload();
  resize(760, 420);
  if (!TeamSync::Instance().Available())
  {
    FStatus->setText(QStringLiteral("<b>Sign in to NixSCP Pro and unlock cloud sync</b> (Pro &gt; Cloud Sync Settings) to use teams."));
    for (QPushButton * B : {New, Join, SyncNow})
    {
      B->setEnabled(false);
    }
  }
  else
  {
    Run([](const auto & Done) { TeamSync::Instance().SyncAll(Done); });
  }
}

void TeamsDialog::Execute(QWidget * Parent)
{
  TeamsDialog Dialog(Parent);
  Dialog.exec();
}

void TeamsDialog::Reload()
{
  QString Selected = SelectedUuid();
  FTable->setRowCount(0);
  QList<TeamSync::Team> Teams = TeamSync::Instance().Teams();
  std::sort(Teams.begin(), Teams.end(), [](const TeamSync::Team & A, const TeamSync::Team & B) { return A.Name.compare(B.Name, Qt::CaseInsensitive) < 0; });
  for (const TeamSync::Team & T : Teams)
  {
    int R = FTable->rowCount();
    FTable->insertRow(R);
    QString Status = T.Key.isEmpty() ? QStringLiteral("Waiting for the team key from an admin") :
      (T.NeedsRotation ? QStringLiteral("Renewing the team key...") : QStringLiteral("Synced"));
    QStringList Cells{T.Name, RoleText(T.Role), QString::number(T.MemberCount), Status};
    for (int C = 0; C < Cells.size(); C++)
    {
      auto * Item = new QTableWidgetItem(Cells.at(C));
      Item->setData(Qt::UserRole, T.Uuid);
      FTable->setItem(R, C, Item);
    }
    if (T.Uuid == Selected)
    {
      FTable->selectRow(R);
    }
  }
  FTable->resizeColumnsToContents();
  if (!TeamSync::Instance().LastError().isEmpty())
  {
    FStatus->setText(TeamSync::Instance().LastError().toHtmlEscaped());
  }
  else if (TeamSync::Instance().Available())
  {
    FStatus->setText(Teams.isEmpty() ? QStringLiteral("You are not in any team yet. Create one, or join with an invite code.") : QString());
  }
  UpdateButtons();
}

QString TeamsDialog::SelectedUuid() const
{
  QList<QTableWidgetItem *> Items = FTable->selectedItems();
  return Items.isEmpty() ? QString() : Items.first()->data(Qt::UserRole).toString();
}

void TeamsDialog::UpdateButtons()
{
  const TeamSync::Team * T = TeamSync::Instance().Find(SelectedUuid());
  bool Owner = (T != nullptr) && (T->Role == QStringLiteral("owner"));
  bool Edit = (T != nullptr) && T->CanEdit();
  FShare->setEnabled(Edit && !T->Key.isEmpty());
  FInvite->setEnabled(Edit && !T->Key.isEmpty());
  FMembers->setEnabled(T != nullptr);
  FRename->setEnabled(Edit);
  FLeave->setEnabled((T != nullptr) && !Owner);
  FDelete->setEnabled(Owner);
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

void TeamsDialog::NewTeam()
{
  bool Ok = false;
  QString Name = QInputDialog::getText(this, QStringLiteral("New Team"), QStringLiteral("Team &name:"), QLineEdit::Normal, QString(), &Ok).trimmed();
  if (Ok && !Name.isEmpty())
  {
    Run([Name](const auto & Done) { TeamSync::Instance().CreateTeam(Name, Done); });
  }
}

void TeamsDialog::Join()
{
  bool Ok = false;
  QString Code = QInputDialog::getText(this, QStringLiteral("Join a Team"),
    QStringLiteral("Invite &code (as you received it from the team's admin):"), QLineEdit::Normal, QString(), &Ok).trimmed();
  if (Ok && !Code.isEmpty())
  {
    Run([Code](const auto & Done) { TeamSync::Instance().JoinWithCode(Code, Done); });
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
    Role->addItem(QStringLiteral("Admin (edits the sites, invites people)"), QStringLiteral("admin"));
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
    QDialog Show(Guard);
    Show.setWindowTitle(QStringLiteral("Invite Code"));
    auto * V = new QVBoxLayout(&Show);
    auto * Text = new QLabel(QStringLiteral("Give this code to the person you invite to <b>%1</b>, privately (chat, in person). "
      "They enter it in NixSCP: Pro &gt; Teams &gt; Join with Code. It works once and expires in %2 days. "
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

void TeamsDialog::ShowMembers()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  QString MyRole = T->Role;
  QDialog Dialog(this);
  Dialog.setWindowTitle(QStringLiteral("Members of %1").arg(T->Name));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Members = NewTable(&Dialog, {QStringLiteral("E-mail"), QStringLiteral("Role"), QStringLiteral("Joined")});
  Layout->addWidget(Members);
  auto * Row = new QHBoxLayout();
  auto * MakeAdmin = new QPushButton(QStringLiteral("Make &Admin"), &Dialog);
  auto * MakeMember = new QPushButton(QStringLiteral("Make &Member"), &Dialog);
  auto * Remove = new QPushButton(QStringLiteral("&Remove"), &Dialog);
  Row->addWidget(MakeAdmin);
  Row->addWidget(MakeMember);
  Row->addStretch();
  Row->addWidget(Remove);
  Layout->addLayout(Row);
  auto * InvitesLabel = new QLabel(QStringLiteral("Pending invites:"), &Dialog);
  auto * Invites = NewTable(&Dialog, {QStringLiteral("Code id"), QStringLiteral("Role"), QStringLiteral("Created by"), QStringLiteral("Expires")});
  auto * Revoke = new QPushButton(QStringLiteral("Re&voke Invite"), &Dialog);
  bool Edit = T->CanEdit();
  InvitesLabel->setVisible(Edit);
  Invites->setVisible(Edit);
  Revoke->setVisible(Edit);
  Layout->addWidget(InvitesLabel);
  Layout->addWidget(Invites);
  Layout->addWidget(Revoke, 0, Qt::AlignLeft);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Layout->addWidget(Buttons);

  QPointer<QDialog> Guard(&Dialog);
  auto Load = [Guard, Members, Invites, Uuid, Edit]()
  {
    TeamSync::Instance().Members(Uuid, [Guard, Members](bool Ok, const QString &, const QJsonArray & List)
    {
      if ((Guard == nullptr) || !Ok) return;
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
    });
    if (Edit)
    {
      TeamSync::Instance().Invites(Uuid, [Guard, Invites](bool Ok, const QString &, const QJsonArray & List)
      {
        if ((Guard == nullptr) || !Ok) return;
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
      });
    }
  };
  auto Selected = [Members](QString & UserUuid, QString & Role)
  {
    QList<QTableWidgetItem *> Items = Members->selectedItems();
    if (Items.isEmpty()) return false;
    UserUuid = Items.first()->data(Qt::UserRole).toString();
    Role = Items.first()->data(Qt::UserRole + 1).toString();
    return true;
  };
  auto Update = [Selected, MakeAdmin, MakeMember, Remove, MyRole]()
  {
    QString UserUuid, Role;
    bool Any = Selected(UserUuid, Role);
    bool IsOwner = (MyRole == QStringLiteral("owner"));
    MakeAdmin->setEnabled(Any && IsOwner && (Role == QStringLiteral("member")));
    MakeMember->setEnabled(Any && IsOwner && (Role == QStringLiteral("admin")));
    Remove->setEnabled(Any && (Role != QStringLiteral("owner")) && (IsOwner || ((MyRole == QStringLiteral("admin")) && (Role == QStringLiteral("member")))));
  };
  connect(Members, &QTableWidget::itemSelectionChanged, &Dialog, Update);
  auto Report = [Guard, Load](bool Ok, const QString & Error)
  {
    if (Guard == nullptr) return;
    if (!Ok) QMessageBox::warning(Guard, Guard->windowTitle(), Error);
    Load();
  };
  connect(MakeAdmin, &QPushButton::clicked, &Dialog, [Selected, Uuid, Report]()
  {
    QString UserUuid, Role;
    if (Selected(UserUuid, Role)) TeamSync::Instance().SetRole(Uuid, UserUuid, QStringLiteral("admin"), Report);
  });
  connect(MakeMember, &QPushButton::clicked, &Dialog, [Selected, Uuid, Report]()
  {
    QString UserUuid, Role;
    if (Selected(UserUuid, Role)) TeamSync::Instance().SetRole(Uuid, UserUuid, QStringLiteral("member"), Report);
  });
  connect(Remove, &QPushButton::clicked, &Dialog, [&Dialog, Selected, Uuid, Report, Members]()
  {
    QString UserUuid, Role;
    if (!Selected(UserUuid, Role)) return;
    QString Email = Members->selectedItems().first()->text();
    if (QMessageBox::question(&Dialog, Dialog.windowTitle(), QStringLiteral("Remove %1 from the team?\n\nThe team's sites are removed "
          "from their NixSCP and the team key is renewed. They could have copied the server passwords while they were a "
          "member: change the passwords that matter.").arg(Email)) == QMessageBox::Yes)
    {
      TeamSync::Instance().RemoveMember(Uuid, UserUuid, Report);
    }
  });
  connect(Revoke, &QPushButton::clicked, &Dialog, [Invites, Uuid, Report]()
  {
    QList<QTableWidgetItem *> Items = Invites->selectedItems();
    if (!Items.isEmpty()) TeamSync::Instance().RevokeInvite(Uuid, Items.first()->data(Qt::UserRole).toString(), Report);
  });
  Load();
  Update();
  Dialog.resize(640, 460);
  Dialog.exec();
}

void TeamsDialog::ShareFolder()
{
  QString Uuid = SelectedUuid();
  const TeamSync::Team * T = TeamSync::Instance().Find(Uuid);
  if (T == nullptr)
  {
    return;
  }
  // top folders of the personal sites (not other teams')
  QSet<QString> Folders;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    qsizetype Slash = Data->Name.lastIndexOf(QLatin1Char('/'));
    if ((Slash > 0) && !Data->Name.startsWith(CloudSettings::TeamFolderPrefix) && !Data->IsWorkspace)
    {
      Folders.insert(Data->Name.left(Slash));
    }
  }
  if (Folders.isEmpty())
  {
    QMessageBox::information(this, windowTitle(), QStringLiteral("Put the sites to share into a site folder first (Login dialog: "
      "New Folder), then add the folder here."));
    return;
  }
  QStringList List = Folders.values();
  List.sort(Qt::CaseInsensitive);
  bool Ok = false;
  QString Folder = QInputDialog::getItem(this, QStringLiteral("Add Folder to %1").arg(T->Name),
    QStringLiteral("The folder's sites move into the team (Team: %1/...) and every member gets them:").arg(T->Name),
    List, 0, false, &Ok);
  if (Ok && !Folder.isEmpty())
  {
    Run([Uuid, Folder](const auto & Done) { TeamSync::Instance().ShareFolder(Uuid, Folder, Done); });
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
