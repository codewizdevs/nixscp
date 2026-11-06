// The Teams window against the fake cloud (NIXSCP_GUI_TESTS): fingerprints, the
// banners of a failed verification and of a conflict, and a sync that lands while
// one of its dialogs is open (T10: the team a dialog works on may be replaced).
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

#include "TeamsFake.h"

#include "gui/TeamsDialog.h"

static QPushButton * Button(QWidget * Parent, const QString & Text)
{
  for (QPushButton * Item : Parent->findChildren<QPushButton *>())
  {
    if (Item->text() == Text) return Item;
  }
  return nullptr;
}

static QStringList LabelTexts(QWidget * Parent)
{
  QStringList Result;
  for (QLabel * Item : Parent->findChildren<QLabel *>())
  {
    Result.append(Item->text());
  }
  return Result;
}

// Runs Step when the next modal dialog is on the screen
static void WhenModal(const std::function<void(QWidget *)> & Step)
{
  auto * Timer = new QTimer();
  Timer->setInterval(20);
  QObject::connect(Timer, &QTimer::timeout, [Timer, Step]()
  {
    QWidget * Modal = QApplication::activeModalWidget();
    if (Modal != nullptr)
    {
      Timer->stop();
      Timer->deleteLater();
      Step(Modal);
    }
  });
  Timer->start();
}

// A sync that replaces the teams, with a new name for the team
static void ReplaceTeams(const Setup & S, const QString & Name)
{
  Srv->Teams[S.Uuid].Key = S.Key;
  Srv->PeerPush(S.Bob, S.Uuid, Content(Name, {{"db", "db.internal", "S3cret-password"}}));
  CHECK(SyncAll());
  CHECK(Teams().Find(S.Uuid)->Name == Name);
}

int main(int argc, char ** argv)
{
  QApplication App(argc, argv);
  QTemporaryDir Temporary;
  Dir = Temporary.path();
  Fake Server;
  Server.Start();
  Srv = &Server;
  qputenv("NIXSCP_CLOUD_URL", Server.Base.toLatin1());
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  // the team of this window is not the same object after every sync
  TeamsDialog Window(nullptr, S.Uuid);
  Window.show();
  Spin([&]() { return !Window.findChild<QTableWidget *>()->rowCount() == 0; }, 5000);
  QStringList Fingerprints;
  for (QTableWidget * Table : Window.findChildren<QTableWidget *>())
  {
    for (int Row = 0; Row < Table->rowCount(); Row++)
    {
      if (Table->columnCount() == 4) Fingerprints.append(Table->item(Row, 3)->text());
    }
  }
  Spin([&]() { return LabelTexts(&Window).join(QLatin1Char('\n')).contains(Teams().Fingerprint()); }, 5000);
  CHECK(Fingerprints.contains(TeamCrypto::Fingerprint(S.Bob.Identity.Public)));
  CHECK(LabelTexts(&Window).join(QLatin1Char('\n')).contains(Teams().Fingerprint()));

  // T10: a sync lands while the Invite dialog is open
  bool Shown = false;
  QPushButton * Invite = Button(&Window, QStringLiteral("&Invite..."));
  CHECK(Invite != nullptr);
  WhenModal([&](QWidget * Modal)
  {
    ReplaceTeams(S, QStringLiteral("Renamed while inviting"));
    // the code dialog comes next and names the team the invite was made for
    WhenModal([&](QWidget * Code)
    {
      Shown = LabelTexts(Code).join(QLatin1Char('\n')).contains(QStringLiteral("Prod"));
      static_cast<QDialog *>(Code)->reject();
    });
    QDialogButtonBox * Box = Modal->findChild<QDialogButtonBox *>();
    Box->button(QDialogButtonBox::Ok)->click();
  });
  Invite->click();
  Spin([&]() { return Shown; }, 8000);
  CHECK(Shown);

  // T10: and while the Rename dialog is open
  QMenu * TeamMenu = Window.findChild<QToolButton *>()->menu();
  emit TeamMenu->aboutToShow();
  QAction * Rename = nullptr;
  for (QAction * Item : TeamMenu->actions())
  {
    if (Item->text() == QStringLiteral("&Rename...")) Rename = Item;
  }
  CHECK(Rename != nullptr);
  WhenModal([&](QWidget * Modal)
  {
    ReplaceTeams(S, QStringLiteral("Renamed while renaming"));
    static_cast<QInputDialog *>(Modal)->setTextValue(QStringLiteral("Final name"));
    static_cast<QDialog *>(Modal)->accept();
  });
  Rename->trigger();
  Spin([&]() { return Teams().Find(S.Uuid)->Name == QStringLiteral("Final name"); }, 8000);
  CHECK(Teams().Find(S.Uuid)->Name == QStringLiteral("Final name"));

  // a team that fails verification says so in the window
  QLabel * Banner = nullptr;
  Srv->Teams[S.Uuid].ServeWrap = QByteArray(92, '\1');
  CHECK(!SyncAll());
  Spin([&]() { return LabelTexts(&Window).join(QLatin1Char('\n')).contains(QStringLiteral("failed verification")); }, 5000);
  for (QLabel * Item : Window.findChildren<QLabel *>())
  {
    if (Item->text().contains(QStringLiteral("failed verification"))) Banner = Item;
  }
  CHECK((Banner != nullptr) && Banner->isVisible());
  QPushButton * ForgetButton = Button(&Window, QStringLiteral("Remove from This &Computer..."));
  CHECK((ForgetButton != nullptr) && ForgetButton->isVisible());
  Srv->Teams[S.Uuid].ServeWrap.clear();
  CHECK(SyncAll());
  Spin([&]() { return !ForgetButton->isVisible(); }, 5000);
  CHECK(!ForgetButton->isVisible());
  Spin([&]() { return !LabelTexts(&Window).join(QLatin1Char('\n')).contains(QStringLiteral("failed verification")); }, 5000);
  CHECK(!LabelTexts(&Window).join(QLatin1Char('\n')).contains(QStringLiteral("failed verification")));

  // a conflict offers the two ways out
  Site(FolderOf(S.Uuid) + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/mine");
  Configuration::Instance().Save();
  Srv->Teams[S.Uuid].Key = S.Key;
  Srv->PeerPush(S.Bob, S.Uuid, Content(Teams().Find(S.Uuid)->Name, {{"db", "theirs.internal", "pw"}}));
  CHECK(!SyncAll());
  QPushButton * Keep = Button(&Window, QStringLiteral("&Keep My Version for Everyone"));
  QPushButton * Use = Button(&Window, QStringLiteral("&Use the Team's Version"));
  Spin([&]() { return Keep->isVisible(); }, 5000);
  CHECK(Keep->isVisible() && Use->isVisible());
  WhenModal([](QWidget * Question) { static_cast<QDialog *>(Question)->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Yes)->click(); });
  Use->click();
  Spin([&]() { return !Keep->isVisible(); }, 8000);
  CHECK(!Keep->isVisible());
  CHECK(Site(FolderOf(S.Uuid) + QStringLiteral("/db"))->HostName == QStringLiteral("theirs.internal"));

  // the way out of a team that cannot be verified
  QString Folder = FolderOf(S.Uuid);
  Srv->Teams[S.Uuid].ServeWrap = QByteArray(92, '\1');
  CHECK(!SyncAll());
  Spin([&]() { return ForgetButton->isVisible(); }, 5000);
  CHECK(ForgetButton->isVisible());
  WhenModal([](QWidget * Question)
  {
    for (QPushButton * Item : Question->findChildren<QPushButton *>())
    {
      if (Item->text().contains(QStringLiteral("Keep the Sites"))) Item->click();
    }
  });
  ForgetButton->click();
  Spin([&]() { return Teams().Find(S.Uuid) == nullptr; }, 8000);
  CHECK(Teams().Find(S.Uuid) == nullptr);
  CHECK(Site(Folder + QStringLiteral("/db")) == nullptr);

  if (Failures == 0)
  {
    std::printf("All Teams window tests passed\n");
  }
  std::fflush(stdout);
  Temporary.remove();
  _exit((Failures == 0) ? 0 : 1);
}
