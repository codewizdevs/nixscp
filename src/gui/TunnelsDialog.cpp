#include "gui/TunnelsDialog.h"
#include "gui/GuiUserInterface.h"

#include "core/Configuration.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

//---------------------------------------------------------------------------
// TunnelManager

TunnelManager & TunnelManager::Instance()
{
  static TunnelManager * Manager = new TunnelManager();
  return *Manager;
}

TunnelManager::TunnelManager() : QObject(QCoreApplication::instance())
{
  FUI = new GuiUserInterface(nullptr);
  // stopped (and their threads joined) before the application goes away
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this]()
  {
    StopAll();
    for (auto It = FTunnels.begin(); It != FTunnels.end(); ++It)
    {
      delete It.value();
    }
    FTunnels.clear();
  });
}

QString TunnelManager::Key(const QString & Site, const QString & Name)
{
  return Site + QLatin1Char('|') + Name;
}

void TunnelManager::Start(const SessionData & Data, const PortForwardSpec & Spec)
{
  QString K = Key(Data.Name, Spec.Name);
  if (Find(K) != nullptr)
  {
    return;
  }
  FErrors.remove(K);
  auto * Tunnel = new PortForward(Data, Spec, FUI, this);
  FTunnels.insert(K, Tunnel);
  connect(Tunnel, &PortForward::Started, this, [this]() { emit Changed(); });
  connect(Tunnel, &PortForward::ConnectionsChanged, this, [this]() { emit Changed(); });
  connect(Tunnel, &PortForward::ConnectionFailed, this, [this, K](const QString & Error)
  {
    FErrors.insert(K, Error);
    emit Changed();
  });
  connect(Tunnel, &PortForward::Stopped, this, [this, K, Tunnel](const QString & Error)
  {
    if (!Error.isEmpty())
    {
      FErrors.insert(K, Error);
    }
    if (FTunnels.value(K) == Tunnel)
    {
      FTunnels.remove(K);
    }
    Tunnel->deleteLater();
    emit Changed();
  });
  Tunnel->Start();
  emit Changed();
}

void TunnelManager::Stop(const QString & K)
{
  if (PortForward * Tunnel = Find(K))
  {
    Tunnel->Stop();
  }
}

void TunnelManager::StopAll()
{
  for (const auto & Tunnel : std::as_const(FTunnels))
  {
    if (Tunnel != nullptr)
    {
      Tunnel->Stop();
    }
  }
}

PortForward * TunnelManager::Find(const QString & K) const
{
  return FTunnels.value(K).data();
}

QList<PortForward *> TunnelManager::Tunnels() const
{
  QList<PortForward *> Result;
  for (const auto & Tunnel : FTunnels)
  {
    if (Tunnel != nullptr)
    {
      Result.append(Tunnel.data());
    }
  }
  std::sort(Result.begin(), Result.end(), [](const PortForward * A, const PortForward * B)
    { return A->Spec().LocalPort < B->Spec().LocalPort; });
  return Result;
}

//---------------------------------------------------------------------------
namespace TunnelsUi
{

QString Describe(const PortForward * Tunnel, const QString & Site)
{
  const PortForwardSpec & Spec = Tunnel->Spec();
  QString Text = QStringLiteral("%1: %2  127.0.0.1:%3 \u2192 %4:%5").arg(Site, Spec.Name).arg(Spec.LocalPort).arg(Spec.Host).arg(Spec.Port);
  if (!Tunnel->Running())
  {
    return Text + QStringLiteral(" (connecting...)");
  }
  int N = Tunnel->Connections();
  return Text + ((N == 1) ? QStringLiteral(" (1 connection)") : QStringLiteral(" (%1 connections)").arg(N));
}

void SetupMenu(QMenu * Menu, QAction * Manage)
{
  Menu->setToolTipsVisible(true);
  QObject::connect(Menu, &QMenu::aboutToShow, Menu, [Menu, Manage]()
  {
    Menu->clear();
    Menu->addAction(Manage);
    Menu->addSeparator();
    TunnelManager & Manager = TunnelManager::Instance();
    int Count = 0;
    for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
    {
      if (!Data->IsSsh() || Data->IsWorkspace)
      {
        continue;
      }
      for (const PortForwardSpec & Spec : PortForwardSpec::LoadAll(Data->PortForwards))
      {
        QString K = TunnelManager::Key(Data->Name, Spec.Name);
        PortForward * Tunnel = Manager.Find(K);
        QString Caption = QStringLiteral("%1: %2  (%3)").arg(Data->Name, Spec.Name, Spec.Text());
        QAction * Action = Menu->addAction(Caption.replace(QLatin1Char('&'), QStringLiteral("&&")));
        Action->setCheckable(true);
        Action->setChecked(Tunnel != nullptr);
        Action->setToolTip((Tunnel != nullptr) ? Describe(Tunnel, Data->Name) + QStringLiteral("\nClick to stop it.") :
          (Manager.LastError(K).isEmpty() ? QStringLiteral("Click to start it.") :
            QStringLiteral("Failed: %1\nClick to start it again.").arg(Manager.LastError(K))));
        SessionData Site = *Data;
        QObject::connect(Action, &QAction::triggered, Menu, [Site, Spec, K]()
        {
          TunnelManager & M = TunnelManager::Instance();
          if (M.Find(K) != nullptr)
          {
            M.Stop(K);
          }
          else
          {
            M.Start(Site, Spec);
          }
        });
        Count++;
      }
    }
    if (Count == 0)
    {
      QAction * None = Menu->addAction(QStringLiteral("No saved tunnels"));
      None->setEnabled(false);
    }
    Menu->addSeparator();
    QAction * StopAll = Menu->addAction(QStringLiteral("&Stop All Tunnels"));
    StopAll->setEnabled(!Manager.Tunnels().isEmpty());
    QObject::connect(StopAll, &QAction::triggered, Menu, []() { TunnelManager::Instance().StopAll(); });
  });
}

QToolButton * CreateIndicator(QWidget * Window)
{
  auto * Button = new QToolButton(Window);
  Button->setObjectName(QStringLiteral("TunnelsStatusPanel"));
  Button->setAutoRaise(true);
  Button->setToolButtonStyle(Qt::ToolButtonTextOnly);
  Button->hide();
  auto Update = [Button]()
  {
    QList<PortForward *> Tunnels = TunnelManager::Instance().Tunnels();
    Button->setVisible(!Tunnels.isEmpty());
    Button->setText((Tunnels.size() == 1) ? QStringLiteral("\u21c4 1 tunnel running") :
      QStringLiteral("\u21c4 %1 tunnels running").arg(Tunnels.size()));
    QStringList Lines;
    for (const PortForward * Tunnel : Tunnels)
    {
      Lines.append(Describe(Tunnel, Tunnel->Data().Name));
    }
    Button->setToolTip(Lines.join(QLatin1Char('\n')) + QStringLiteral("\n\nClick to manage the tunnels."));
  };
  QObject::connect(&TunnelManager::Instance(), &TunnelManager::Changed, Button, Update);
  QObject::connect(Button, &QToolButton::clicked, Button, [Window]() { TunnelsDialog::Execute(Window); });
  Update();
  return Button;
}

}

//---------------------------------------------------------------------------
// TunnelsDialog

TunnelsDialog::TunnelsDialog(QWidget * Parent) : QDialog(Parent)
{
  setWindowTitle(QStringLiteral("Tunnels"));
  auto * Layout = new QVBoxLayout(this);
  auto * Intro = new QLabel(QStringLiteral("Tunnels forward a port on this computer (127.0.0.1) to a host and port as seen "
    "from the server, e.g. a database that only listens on the server. Agents can open the tunnels of sites that allow "
    "it with <b>nixscp --tunnel</b>."), this);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  FTable = new QTableWidget(0, 6, this);
  FTable->setHorizontalHeaderLabels({QStringLiteral("Site"), QStringLiteral("Name"), QStringLiteral("Local port"),
    QStringLiteral("Forwards to"), QStringLiteral("Status"), QStringLiteral("Connections")});
  FTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  FTable->setSelectionMode(QAbstractItemView::SingleSelection);
  FTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  FTable->verticalHeader()->hide();
  FTable->horizontalHeader()->setStretchLastSection(true);
  Layout->addWidget(FTable);
  auto * Row = new QHBoxLayout();
  auto * Add = new QPushButton(QStringLiteral("&Add..."), this);
  FEdit = new QPushButton(QStringLiteral("&Edit..."), this);
  FRemove = new QPushButton(QStringLiteral("&Remove"), this);
  FStart = new QPushButton(QStringLiteral("&Start"), this);
  FStop = new QPushButton(QStringLiteral("S&top"), this);
  for (QPushButton * B : {Add, FEdit, FRemove})
  {
    Row->addWidget(B);
  }
  Row->addStretch();
  Row->addWidget(FStart);
  Row->addWidget(FStop);
  Layout->addLayout(Row);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addWidget(Buttons);
  connect(Add, &QPushButton::clicked, this, [this]() { Edit(true); });
  connect(FEdit, &QPushButton::clicked, this, [this]() { Edit(false); });
  connect(FRemove, &QPushButton::clicked, this, [this]() { Remove(); });
  connect(FStart, &QPushButton::clicked, this, [this]() { StartSelected(); });
  connect(FStop, &QPushButton::clicked, this, [this]() { StopSelected(); });
  connect(FTable, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
  connect(FTable, &QTableWidget::cellDoubleClicked, this, [this]() { Edit(false); });
  connect(&TunnelManager::Instance(), &TunnelManager::Changed, this, [this]() { Reload(); });
  Reload();
  resize(860, 400);
}

void TunnelsDialog::Execute(QWidget * Parent)
{
  TunnelsDialog Dialog(Parent);
  Dialog.exec();
}

void TunnelsDialog::Reload()
{
  QString SelSite, SelName;
  bool HadSelection = Selected(SelSite, SelName);
  FTable->setRowCount(0);
  TunnelManager & Manager = TunnelManager::Instance();
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (!Data->IsSsh() || Data->IsWorkspace)
    {
      continue;
    }
    for (const PortForwardSpec & Spec : PortForwardSpec::LoadAll(Data->PortForwards))
    {
      int R = FTable->rowCount();
      FTable->insertRow(R);
      QString K = TunnelManager::Key(Data->Name, Spec.Name);
      PortForward * Tunnel = Manager.Find(K);
      QString Status = (Tunnel == nullptr) ?
        (Manager.LastError(K).isEmpty() ? QStringLiteral("Stopped") : QStringLiteral("Failed: ") + Manager.LastError(K)) :
        (Tunnel->Running() ?
          (Manager.LastError(K).isEmpty() ? QStringLiteral("Running") : QStringLiteral("Running; ") + Manager.LastError(K)) :
          QStringLiteral("Connecting..."));
      QStringList Cells{Data->Name, Spec.Name, QString::number(Spec.LocalPort), QStringLiteral("%1:%2").arg(Spec.Host).arg(Spec.Port),
        Status, (Tunnel != nullptr) ? QString::number(Tunnel->Connections()) : QString()};
      for (int C = 0; C < Cells.size(); C++)
      {
        auto * Item = new QTableWidgetItem(Cells.at(C));
        if (C == 4)
        {
          Item->setToolTip(Status);
        }
        FTable->setItem(R, C, Item);
      }
      if (HadSelection && (Data->Name == SelSite) && (Spec.Name == SelName))
      {
        FTable->selectRow(R);
      }
    }
  }
  FTable->resizeColumnsToContents();
  UpdateButtons();
}

bool TunnelsDialog::Selected(QString & Site, QString & Name) const
{
  QList<QTableWidgetItem *> Items = FTable->selectedItems();
  if (Items.isEmpty())
  {
    return false;
  }
  int R = Items.first()->row();
  Site = FTable->item(R, 0)->text();
  Name = FTable->item(R, 1)->text();
  return true;
}

void TunnelsDialog::UpdateButtons()
{
  QString Site, Name;
  bool Any = Selected(Site, Name);
  bool Running = Any && (TunnelManager::Instance().Find(TunnelManager::Key(Site, Name)) != nullptr);
  FEdit->setEnabled(Any && !Running);
  FRemove->setEnabled(Any && !Running);
  FStart->setEnabled(Any && !Running);
  FStop->setEnabled(Running);
}

void TunnelsDialog::Edit(bool New)
{
  QString SiteName, Name;
  if (!New && !Selected(SiteName, Name))
  {
    return;
  }
  StoredSessionList & Sites = Configuration::Instance().StoredSessions();
  QDialog Dialog(this);
  Dialog.setWindowTitle(New ? QStringLiteral("Add Tunnel") : QStringLiteral("Edit Tunnel"));
  auto * Form = new QFormLayout(&Dialog);
  auto * SiteCombo = new QComboBox(&Dialog);
  for (const auto & Data : Sites.Sessions())
  {
    if (Data->IsSsh() && !Data->IsWorkspace)
    {
      SiteCombo->addItem(Data->Name);
    }
  }
  if (SiteCombo->count() == 0)
  {
    QMessageBox::information(this, windowTitle(), QStringLiteral("Save an SFTP or SCP site first; tunnels go through its SSH connection."));
    return;
  }
  auto * NameEdit = new QLineEdit(&Dialog);
  auto * LocalPort = new QSpinBox(&Dialog);
  LocalPort->setRange(1024, 65535);
  LocalPort->setValue(5433);
  auto * Host = new QLineEdit(QStringLiteral("localhost"), &Dialog);
  auto * Port = new QSpinBox(&Dialog);
  Port->setRange(1, 65535);
  Port->setValue(5432);
  PortForwardSpec Old;
  if (!New)
  {
    SiteCombo->setCurrentText(SiteName);
    SiteCombo->setEnabled(false);
    for (const PortForwardSpec & Spec : PortForwardSpec::LoadAll(Sites.FindByName(SiteName)->PortForwards))
    {
      if (Spec.Name == Name)
      {
        Old = Spec;
      }
    }
    NameEdit->setText(Old.Name);
    LocalPort->setValue(Old.LocalPort);
    Host->setText(Old.Host);
    Port->setValue(Old.Port);
  }
  Form->addRow(QStringLiteral("&Site:"), SiteCombo);
  Form->addRow(QStringLiteral("&Name:"), NameEdit);
  Form->addRow(QStringLiteral("&Local port (127.0.0.1):"), LocalPort);
  Form->addRow(QStringLiteral("Forward to &host:"), Host);
  Form->addRow(QStringLiteral("Forward to &port:"), Port);
  auto * Hint = new QLabel(QStringLiteral("The host and port as seen from the server: \"localhost\" is the server itself."), &Dialog);
  Hint->setWordWrap(true);
  Form->addRow(Hint);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Form->addRow(Buttons);
  NameEdit->setFocus();
  if (Dialog.exec() != QDialog::Accepted)
  {
    return;
  }
  PortForwardSpec Spec;
  Spec.Name = NameEdit->text().trimmed().remove(QLatin1Char('|'));
  Spec.LocalPort = LocalPort->value();
  Spec.Host = Host->text().trimmed();
  Spec.Port = Port->value();
  if (Spec.Name.isEmpty())
  {
    Spec.Name = QStringLiteral("%1-%2").arg(Spec.Host).arg(Spec.Port);
  }
  auto Site = Sites.FindByName(SiteCombo->currentText());
  if ((Site == nullptr) || !Spec.IsValid())
  {
    return;
  }
  QList<PortForwardSpec> List = PortForwardSpec::LoadAll(Site->PortForwards);
  for (qsizetype I = 0; I < List.size(); I++)
  {
    if (!New && (List.at(I).Name == Old.Name))
    {
      List.removeAt(I--);
    }
    else if (List.at(I).Name.compare(Spec.Name, Qt::CaseInsensitive) == 0)
    {
      QMessageBox::warning(this, windowTitle(), QStringLiteral("The site already has a tunnel named \"%1\".").arg(Spec.Name));
      return;
    }
  }
  List.append(Spec);
  Site->PortForwards = PortForwardSpec::SaveAll(List);
  Configuration::Instance().Save();
  Reload();
}

void TunnelsDialog::Remove()
{
  QString SiteName, Name;
  if (!Selected(SiteName, Name) ||
      (QMessageBox::question(this, windowTitle(), QStringLiteral("Remove the tunnel \"%1\" of \"%2\"?").arg(Name, SiteName)) != QMessageBox::Yes))
  {
    return;
  }
  auto Site = Configuration::Instance().StoredSessions().FindByName(SiteName);
  if (Site == nullptr)
  {
    return;
  }
  QList<PortForwardSpec> List = PortForwardSpec::LoadAll(Site->PortForwards);
  List.erase(std::remove_if(List.begin(), List.end(), [&Name](const PortForwardSpec & S) { return S.Name == Name; }), List.end());
  Site->PortForwards = PortForwardSpec::SaveAll(List);
  Configuration::Instance().Save();
  Reload();
}

void TunnelsDialog::StartSelected()
{
  QString SiteName, Name;
  if (!Selected(SiteName, Name))
  {
    return;
  }
  auto Site = Configuration::Instance().StoredSessions().FindByName(SiteName);
  if (Site == nullptr)
  {
    return;
  }
  for (const PortForwardSpec & Spec : PortForwardSpec::LoadAll(Site->PortForwards))
  {
    if (Spec.Name == Name)
    {
      TunnelManager::Instance().Start(*Site, Spec);
    }
  }
}

void TunnelsDialog::StopSelected()
{
  QString SiteName, Name;
  if (Selected(SiteName, Name))
  {
    TunnelManager::Instance().Stop(TunnelManager::Key(SiteName, Name));
  }
}
