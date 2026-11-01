// GUI tests (NIXSCP_GUI_TESTS, offscreen) of the Properties dialog
// (forms/Properties, forms/Rights: tri-state permissions, owner/group
// combos, Calculate, Checksum tab), Create folder (permissions, "Use same
// settings next time" / NewDirectory2), Duplicate and Move dialogs
// (forms/RemoteTransfer, TRemoteMoveDialog), the link dialog (forms/Symlink)
// and the FileOperations entry points for local files (chmod of a mixed
// selection, links, sizes, checksums). With NIXSCP_TEST_SSH_PORT and
// NIXSCP_TEST_SSH_KEY also end to end against the test sshd (Properties,
// Duplicate directly and via local temporary copy, Move To, Edit link,
// space available). NIXSCP_TEST_SCREENSHOT_DIR saves the dialogs as PNG.
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"
#include "gui/FileOperations.h"
#include "gui/HistoryComboBox.h"
#include "gui/LinkDialog.h"
#include "gui/PropertiesDialog.h"
#include "gui/RemoteTransferDialog.h"

#include <QLocale>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <functional>

static int Failures = 0;
static QString ScreenshotDir;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static void Screenshot(QWidget * Widget, const QString & Name)
{
  if (!ScreenshotDir.isEmpty())
  {
    Widget->show();
    QApplication::processEvents();
    Widget->grab().save(QDir(ScreenshotDir).filePath(Name + QStringLiteral(".png")));
  }
}

// Acts on the next modal dialog matching Match (polling, as the dialogs run exec())
static void OnModal(std::function<bool(QWidget *)> Match, std::function<void(QWidget *)> Action, int TimeoutMs = 10000)
{
  auto * Elapsed = new int(0);
  auto * Timer = new QTimer();
  Timer->setInterval(20);
  QObject::connect(Timer, &QTimer::timeout, [Timer, Elapsed, Match, Action, TimeoutMs]()
  {
    *Elapsed += 20;
    QWidget * Modal = QApplication::activeModalWidget();
    if ((Modal != nullptr) && Match(Modal))
    {
      Timer->stop();
      Timer->deleteLater();
      delete Elapsed;
      Action(Modal);
    }
    else if (*Elapsed > TimeoutMs)
    {
      std::printf("FAIL: expected modal dialog did not appear\n");
      Failures++;
      Timer->stop();
      Timer->deleteLater();
      delete Elapsed;
      if (Modal != nullptr)
      {
        static_cast<QDialog *>(Modal)->reject();
      }
    }
  });
  Timer->start();
}

template<class T>
static bool IsA(QWidget * W)
{
  return qobject_cast<T *>(W) != nullptr;
}

static QPushButton * OkButtonOf(QWidget * Dialog)
{
  for (QPushButton * Button : Dialog->findChildren<QPushButton *>())
  {
    if (Button->text() == QStringLiteral("OK") || Button->text() == QStringLiteral("&OK"))
    {
      return Button;
    }
  }
  return nullptr;
}

static QCheckBox * RightsCheck(QWidget * Parent, int Who, const QString & Caption)
{
  // the R/W/X check boxes in row order: Owner, Group, Others
  QList<QCheckBox *> Found;
  for (QCheckBox * Check : Parent->findChildren<QCheckBox *>())
  {
    if (Check->text() == Caption)
    {
      Found << Check;
    }
  }
  return Found.value(Who);
}

static RemoteFile MakeFile(const QString & Name, unsigned int Mode, bool Directory = false, qint64 Size = 0)
{
  RemoteFile File;
  File.FileName = Name;
  File.Permissions = Mode;
  File.IsDirectory = Directory;
  File.FileType = Directory ? RemoteFile::Directory : RemoteFile::File;
  File.Size = Size;
  File.Owner = QStringLiteral("alice");
  File.Group = QStringLiteral("staff");
  return File;
}

static unsigned int ModeOf(const QString & FileName)
{
  struct stat St = {};
  return (lstat(QFile::encodeName(FileName).constData(), &St) == 0) ? (St.st_mode & 07777) : 0xFFFFFFFF;
}

static void WriteFile(const QString & FileName, const QByteArray & Data, unsigned int Mode)
{
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
  F.close();
  chmod(QFile::encodeName(FileName).constData(), Mode);
}

//---------------------------------------------------------------------------
static void TestRightsWidget()
{
  std::printf("[RightsWidget]\n");
  RightsWidget W;
  W.SetRights(0755);
  CHECK(W.Rights() == 0755);
  CHECK(W.RightsUnset() == 07022);
  CHECK(!W.IsUndef());
  CHECK(W.findChild<QLineEdit *>()->text() == QStringLiteral("0755"));
  // hints
  CHECK(RightsCheck(&W, 0, QStringLiteral("R"))->toolTip() == QStringLiteral("Read"));
  CHECK(RightsCheck(&W, 2, QStringLiteral("X"))->toolTip() == QStringLiteral("Execute/Enter"));
  // three-state
  W.SetAllowUndef(true);
  W.SetStates(0644, 07022);
  CHECK(W.IsUndef());
  CHECK(W.Rights() == 0644);
  CHECK(W.RightsUnset() == 07022);
  CHECK(W.findChild<QLineEdit *>()->text().isEmpty());
  CHECK(W.IsValid());
  CHECK(RightsCheck(&W, 0, QStringLiteral("X"))->checkState() == Qt::PartiallyChecked);
  // row button cycles: mixed row (Owner: R W undef) -> Yes -> No -> Undefined -> Yes
  QToolButton * OwnerButton = W.findChild<QToolButton *>(QStringLiteral("RightsButton0"));
  CHECK(OwnerButton != nullptr);
  OwnerButton->click();
  CHECK((W.Rights() & 0700) == 0700);
  OwnerButton->click();
  CHECK((W.RightsUnset() & 0700) == 0700);
  OwnerButton->click();
  CHECK(((W.Rights() | W.RightsUnset()) & 0700) == 0);
  OwnerButton->click();
  CHECK((W.Rights() & 0700) == 0700);
  // typing an octal value defines everything
  QLineEdit * Octal = W.findChild<QLineEdit *>();
  Octal->setText(QStringLiteral("0640"));
  emit Octal->textEdited(QStringLiteral("0640"));
  CHECK(!W.IsUndef());
  CHECK(W.Rights() == 0640);
  // Add X to directories: disabled when every X is set
  W.SetAddXToDirectoriesVisible(true);
  QCheckBox * AddX = nullptr;
  for (QCheckBox * C : W.findChildren<QCheckBox *>())
  {
    if (C->text() == QStringLiteral("Add &X to directories")) AddX = C;
  }
  CHECK((AddX != nullptr) && AddX->isEnabled());
  W.SetRights(0755);
  CHECK(!AddX->isEnabled());
  W.SetAllowUndef(false);
  W.SetStates(0644, 0);
  CHECK(!W.IsUndef());
  CHECK(W.RightsUnset() == (~0644u & 07777));
}

static void TestPropertiesDialog()
{
  std::printf("[PropertiesDialog: mixed selection]\n");
  QList<RemoteFile> Files = {MakeFile(QStringLiteral("a.txt"), 0644, false, 10), MakeFile(QStringLiteral("run.sh"), 0755, false, 20)};
  PropertiesDialog Dialog(Files, QStringLiteral("/home/alice"), true);
  Dialog.SetAllowedChanges(PropertiesDialog::cpMode | PropertiesDialog::cpOwner | PropertiesDialog::cpGroup);
  Dialog.SetUsersGroups({QStringLiteral("alice"), QStringLiteral("bob")}, {QStringLiteral("staff"), QStringLiteral("wheel")});
  Dialog.show();
  QApplication::processEvents();
  CHECK(Dialog.windowTitle() == QStringLiteral("a.txt, ... Properties"));
  CHECK(Dialog.findChild<QLabel *>(QStringLiteral("FileLabel"))->text() == QStringLiteral("2 files"));
  CHECK(Dialog.findChild<QLabel *>(QStringLiteral("SizeLabel"))->text() == QStringLiteral("30 B"));
  QPushButton * Ok = OkButtonOf(&Dialog);
  CHECK((Ok != nullptr) && !Ok->isEnabled());
  CHECK(Dialog.Properties().Empty());
  // the X bits differ: grayed
  CHECK(RightsCheck(&Dialog, 0, QStringLiteral("X"))->checkState() == Qt::PartiallyChecked);
  // Others W ticked: only that bit changes on each file
  RightsCheck(&Dialog, 2, QStringLiteral("W"))->setCheckState(Qt::Checked);
  CHECK(Ok->isEnabled());
  RemoteProperties P = Dialog.Properties();
  CHECK(P.ValidRights);
  CHECK(P.CombineRights(0644, false) == 0646);
  CHECK(P.CombineRights(0755, false) == 0757);
  CHECK(P.Owner.isEmpty() && P.Group.isEmpty());
  // owner change
  QComboBox * Owner = Dialog.findChild<QComboBox *>(QStringLiteral("OwnerComboBox"));
  CHECK((Owner != nullptr) && Owner->isVisible() && (Owner->currentText() == QStringLiteral("alice")));
  CHECK(Owner->count() == 2);
  Owner->setEditText(QStringLiteral("bob"));
  CHECK(Dialog.Properties().Owner == QStringLiteral("bob"));
  CHECK(Dialog.Properties().Group.isEmpty());
  // no directory: no recursive check, no Calculate
  CHECK(Dialog.findChild<QCheckBox *>(QStringLiteral("RecursiveCheck"))->isHidden());
  CHECK(Dialog.findChild<QPushButton *>(QStringLiteral("CalculateSizeButton"))->isHidden());
  // Checksum tab disabled without a handler
  QTabWidget * Tabs = Dialog.findChild<QTabWidget *>();
  CHECK(Tabs->count() == 2);
  CHECK(Tabs->tabText(1) == QStringLiteral("Checksum"));
  CHECK(!Tabs->widget(1)->isEnabled());
  Screenshot(&Dialog, QStringLiteral("properties-multi"));
  Dialog.reject();

  std::printf("[PropertiesDialog: directory, calculate, checksum]\n");
  QList<RemoteFile> Dirs = {MakeFile(QStringLiteral("dir"), 0755, true)};
  PropertiesDialog DirDialog(Dirs, QStringLiteral("/home/alice"), true);
  DirDialog.SetAllowedChanges(PropertiesDialog::cpMode);
  int CalcCalls = 0;
  DirDialog.SetOnCalculateSize([&](qint64 & Size, PropertiesDialog::SizeStats & Stats)
  {
    CalcCalls++;
    Size = 2048;
    Stats.Files = 3;
    Stats.Directories = 2;
    return true;
  });
  QStringList ChecksumAlgs;
  DirDialog.SetChecksum({QStringLiteral("sha-512"), QStringLiteral("sha-256"), QStringLiteral("md5")},
    [&](const QString & Alg, const PropertiesDialog::ChecksumResultFunc & OnChecksum)
    {
      ChecksumAlgs << Alg;
      OnChecksum(QStringLiteral("dir/x"), QStringLiteral("sha-256"), QStringLiteral("abcd"));
      OnChecksum(QStringLiteral("dir/y"), QStringLiteral("sha-256"), QStringLiteral("ef01"));
    });
  DirDialog.show();
  QApplication::processEvents();
  // ChecksumAlg default "sha1" is not in the list: the first is selected
  CHECK(DirDialog.findChild<QComboBox *>(QStringLiteral("ChecksumAlgEdit"))->currentText() == QStringLiteral("sha-512"));
  CHECK(DirDialog.findChild<QLabel *>(QStringLiteral("SizeLabel"))->text() == QStringLiteral("Unknown"));
  QPushButton * Calculate = DirDialog.findChild<QPushButton *>(QStringLiteral("CalculateSizeButton"));
  CHECK(Calculate->isVisible());
  Calculate->click();
  CHECK(CalcCalls == 1);
  CHECK(DirDialog.findChild<QLabel *>(QStringLiteral("SizeLabel"))->text() == QStringLiteral("2.00 KB (2,048 B)"));
  CHECK(DirDialog.findChild<QLabel *>(QStringLiteral("FileLabel"))->text() == QStringLiteral("3 files, 2 folders"));
  // recursive alone enables OK and pushes the rights down
  QPushButton * DirOk = OkButtonOf(&DirDialog);
  CHECK(!DirOk->isEnabled());
  QCheckBox * Recursive = DirDialog.findChild<QCheckBox *>(QStringLiteral("RecursiveCheck"));
  CHECK(Recursive->isVisible());
  Recursive->setChecked(true);
  CHECK(DirOk->isEnabled());
  CHECK(DirDialog.Properties().ValidRights && DirDialog.Properties().Recursive);
  CHECK(DirDialog.Properties().RightsSet == 0755);
  // checksum is calculated when the tab is opened; the used algorithm is remembered
  QTabWidget * DirTabs = DirDialog.findChild<QTabWidget *>();
  DirTabs->setCurrentIndex(1);
  QApplication::processEvents();
  CHECK(ChecksumAlgs == QStringList{QStringLiteral("sha-512")});
  QTreeWidget * View = DirDialog.findChild<QTreeWidget *>(QStringLiteral("ChecksumView"));
  CHECK(View->isVisible() && (View->topLevelItemCount() == 2));
  CHECK(PropertiesDialog::ConfiguredChecksumAlg() == QStringLiteral("sha-256"));
  // the checksum button is the default while the algorithm combo has the focus
  DirDialog.activateWindow();
  DirDialog.findChild<QComboBox *>(QStringLiteral("ChecksumAlgEdit"))->setFocus();
  QApplication::processEvents();
  CHECK(DirDialog.findChild<QPushButton *>(QStringLiteral("ChecksumButton"))->isDefault());
  Screenshot(&DirDialog, QStringLiteral("properties-checksum"));
  DirDialog.reject();

  std::printf("[PropertiesDialog: single file checksum, invalid owner]\n");
  QList<RemoteFile> One = {MakeFile(QStringLiteral("a.txt"), 0644, false, 6)};
  PropertiesDialog OneDialog(One, QStringLiteral("/home/alice"), true);
  OneDialog.SetAllowedChanges(PropertiesDialog::cpMode | PropertiesDialog::cpOwner | PropertiesDialog::cpGroup);
  OneDialog.SetChecksum({QStringLiteral("sha-1"), QStringLiteral("sha-256")},
    [&](const QString &, const PropertiesDialog::ChecksumResultFunc & OnChecksum)
    {
      OnChecksum(QStringLiteral("a.txt"), QStringLiteral("sha-256"), QStringLiteral("1234abcd"));
    });
  OneDialog.show();
  // remembered sha-256 selected
  CHECK(OneDialog.findChild<QComboBox *>(QStringLiteral("ChecksumAlgEdit"))->currentText() == QStringLiteral("sha-256"));
  OneDialog.findChild<QTabWidget *>()->setCurrentIndex(1);
  QApplication::processEvents();
  CHECK(OneDialog.findChild<QLineEdit *>(QStringLiteral("ChecksumEdit"))->text() == QStringLiteral("1234abcd"));
  CHECK(OneDialog.findChild<QLineEdit *>(QStringLiteral("ChecksumEdit"))->isVisible());
  // single file: no grayed boxes
  CHECK(RightsCheck(&OneDialog, 0, QStringLiteral("R"))->checkState() == Qt::Checked);
  CHECK(!RightsCheck(&OneDialog, 0, QStringLiteral("R"))->isTristate());
  OneDialog.findChild<QComboBox *>(QStringLiteral("GroupComboBox"))->setEditText(QStringLiteral("staff [abc]"));
  OnModal([](QWidget * W) { return W->windowTitle().isEmpty() || !IsA<PropertiesDialog>(W); },
    [](QWidget * W) { static_cast<QDialog *>(W)->accept(); });
  OneDialog.accept();
  CHECK(OneDialog.isVisible());   // "Enter valid group name."
  OneDialog.reject();
}

static void TestCreateDirectoryDialog()
{
  std::printf("[CreateDirectoryDialog]\n");
  RemoteProperties Defaults = CreateDirectoryDialog::LoadNewDirectoryProperties();
  CHECK(!Defaults.ValidRights);
  CHECK(Defaults.RightsSet == 0755);
  CreateDirectoryDialog Dialog(true);
  Dialog.SetAllowedChanges(PropertiesDialog::cpMode);
  Dialog.SetProperties(Defaults);
  Dialog.show();
  CHECK(!Dialog.findChild<QCheckBox *>(QStringLiteral("SetRightsCheck"))->isChecked());
  Dialog.SetDirectoryName(QStringLiteral("a/b"));
  CHECK(OkButtonOf(&Dialog)->isEnabled());   // a path is allowed
  Dialog.findChild<QCheckBox *>(QStringLiteral("SetRightsCheck"))->setChecked(true);
  RightsCheck(&Dialog, 1, QStringLiteral("W"))->setChecked(true);
  CHECK(Dialog.Properties().ValidRights && (Dialog.Properties().RightsSet == 0775));
  Dialog.findChild<QCheckBox *>(QStringLiteral("SaveSettingsCheck"))->setChecked(true);
  CHECK(Dialog.SaveSettings());
  Screenshot(&Dialog, QStringLiteral("create-folder"));
  CreateDirectoryDialog::SaveNewDirectoryProperties(Dialog.Properties());
  RemoteProperties Loaded = CreateDirectoryDialog::LoadNewDirectoryProperties();
  CHECK(Loaded.ValidRights && (Loaded.RightsSet == 0775));
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    CHECK(Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\NewDirectory2"), false));
    CHECK(Storage.ReadStringRaw(QStringLiteral("Valid"), QString()) == QStringLiteral("01"));
    CHECK(Storage.ReadString(QStringLiteral("Rights"), QString()) == QStringLiteral("rwxrwxr-x"));
  }
  // without fcModeChanging "Set permissions" is disabled
  CreateDirectoryDialog NoMode(true);
  NoMode.SetAllowedChanges(0);
  NoMode.SetProperties(Loaded);
  CHECK(!NoMode.findChild<QCheckBox *>(QStringLiteral("SetRightsCheck"))->isEnabled());
  CHECK(!NoMode.SetRights());
  CHECK(!NoMode.findChild<QCheckBox *>(QStringLiteral("SaveSettingsCheck"))->isEnabled());
  // local: no attributes
  CreateDirectoryDialog Local(false);
  CHECK(Local.findChild<QCheckBox *>(QStringLiteral("SetRightsCheck")) == nullptr);
  CreateDirectoryDialog::SaveNewDirectoryProperties(Defaults);
}

static void TestRemoteTransferDialogs()
{
  std::printf("[RemoteTransferDialog]\n");
  int A = 1, B = 2;
  QList<RemoteTransferDialog::SessionEntry> Sessions = {
    {QStringLiteral("alice@one"), QStringLiteral("/home/alice"), &A},
    {QStringLiteral("bob@two"), QStringLiteral("/srv"), &B}};
  QStringList Checked;
  RemoteTransferDialog Dialog;
  Dialog.Init(false, Sessions, Terminal::drcAllow, &A, [&](void * Session, const QString & Path)
  {
    Checked << Path;
    return (Session == &A) && (Path == QStringLiteral("/home/alice/sub"));
  });
  CHECK(Dialog.windowTitle() == QStringLiteral("Duplicate"));
  void * Session = &A;
  QString Target = QStringLiteral("/home/alice/");
  QString Mask = QStringLiteral("a.txt");
  bool DirectCopy = true;
  OnModal([](QWidget * W) { return IsA<RemoteTransferDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteTransferDialog *>(W);
    CHECK(D->DirectoryEdit()->currentText() == QStringLiteral("/home/alice/a.txt"));
    QCheckBox * NotDirect = D->findChild<QCheckBox *>(QStringLiteral("NotDirectCopyCheck"));
    CHECK(NotDirect->isEnabled() && !NotDirect->isChecked());
    // another session: path follows, temporary copy forced
    D->findChild<QComboBox *>(QStringLiteral("SessionCombo"))->setCurrentIndex(1);
    CHECK(D->DirectoryEdit()->currentText() == QStringLiteral("/srv/a.txt"));
    CHECK(!NotDirect->isEnabled() && NotDirect->isChecked());
    D->findChild<QComboBox *>(QStringLiteral("SessionCombo"))->setCurrentIndex(0);
    CHECK(!NotDirect->isChecked());
    // a directory as target gets the "*.*" mask
    D->DirectoryEdit()->setEditText(QStringLiteral("/home/alice/sub"));
    Screenshot(D, QStringLiteral("duplicate"));
    D->accept();
  });
  CHECK(Dialog.Execute(Session, Target, Mask, DirectCopy));
  CHECK(Session == &A);
  CHECK(Target == QStringLiteral("/home/alice/sub/"));
  CHECK(Mask == QStringLiteral("*.*"));
  CHECK(DirectCopy);
  CHECK(Checked.contains(QStringLiteral("/home/alice/sub")));
  CHECK(HistoryComboBox::History(QStringLiteral("RemoteTarget")).value(0) == QStringLiteral("/home/alice/sub/*.*"));

  // command session confirmation (REMOTE_COPY_COMMAND_SESSION3), "Never ask again"
  Configuration::Instance().ConfirmCommandSession = true;
  RemoteTransferDialog Confirm;
  Confirm.Init(false, Sessions, Terminal::drcConfirmCommandSessionDirs, &A, [](void *, const QString &) { return false; });
  QString ConfirmText;
  OnModal([](QWidget * W) { return IsA<RemoteTransferDialog>(W); }, [&](QWidget * W)
  {
    OnModal([](QWidget * M) { return !IsA<RemoteTransferDialog>(M); }, [&](QWidget * M)
    {
      for (QLabel * L : M->findChildren<QLabel *>())
      {
        ConfirmText += L->text();
      }
      for (QCheckBox * C : M->findChildren<QCheckBox *>())
      {
        C->setChecked(true);
      }
      OkButtonOf(M)->click();
    });
    static_cast<QDialog *>(W)->accept();
  });
  Session = &A;
  Target = QStringLiteral("/tmp/");
  Mask = QStringLiteral("x");
  DirectCopy = true;
  CHECK(Confirm.Execute(Session, Target, Mask, DirectCopy));
  CHECK_MSG(ConfirmText.contains(QStringLiteral("separate shell session to duplicate the directory(ies)")), ConfirmText);
  CHECK(!Configuration::Instance().ConfirmCommandSession);
  Configuration::Instance().ConfirmCommandSession = true;

  std::printf("[RemoteMoveDialog]\n");
  RemoteMoveDialog Move(true, [](void *, const QString & Path) { return Path == QStringLiteral("/target"); });
  CHECK(Move.windowTitle() == QStringLiteral("Move"));
  OnModal([](QWidget * W) { return IsA<RemoteMoveDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteMoveDialog *>(W);
    CHECK(D->DirectoryEdit()->currentText() == QStringLiteral("/home/alice/*.*"));
    D->DirectoryEdit()->setEditText(QStringLiteral("/target"));
    Screenshot(D, QStringLiteral("move"));
    D->accept();
  });
  QString MoveTarget = QStringLiteral("/home/alice");
  QString MoveMask = QStringLiteral("*.*");
  CHECK(Move.Execute(MoveTarget, MoveMask));
  CHECK(MoveTarget == QStringLiteral("/target/"));
  CHECK(MoveMask == QStringLiteral("*.*"));

  // several files to one name: MULTI_FILES_TO_ONE, Cancel keeps the dialog open
  RemoteMoveDialog Multi(true, [](void *, const QString &) { return false; });
  QString MultiText;
  OnModal([](QWidget * W) { return IsA<RemoteMoveDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteMoveDialog *>(W);
    D->DirectoryEdit()->setEditText(QStringLiteral("/single.txt"));
    OnModal([](QWidget * M) { return !IsA<RemoteMoveDialog>(M); }, [&](QWidget * M)
    {
      for (QLabel * L : M->findChildren<QLabel *>())
      {
        MultiText += L->text();
      }
      static_cast<QDialog *>(M)->reject();
      QTimer::singleShot(100, D, [D]()
      {
        CHECK(D->isVisible());
        D->reject();
      });
    });
    D->accept();
  });
  QString T2 = QStringLiteral("/"), M2 = QStringLiteral("*.*");
  CHECK(!Multi.Execute(T2, M2));
  CHECK_MSG(MultiText.contains(QStringLiteral("multiple files to a single file")), MultiText);
}

static void TestLinkDialog()
{
  std::printf("[LinkDialog]\n");
  LinkDialog Dialog;
  QString FileName = QStringLiteral("link");
  QString PointTo = QStringLiteral("target");
  bool Symbolic = true;
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [&](QWidget * W)
  {
    CHECK(W->windowTitle() == QStringLiteral("Edit link"));
    CHECK(W->findChild<QLineEdit *>(QStringLiteral("FileNameEdit"))->isReadOnly());
    CHECK(!W->findChild<QCheckBox *>(QStringLiteral("HardLinkCheck"))->isEnabled());
    W->findChild<QLineEdit *>(QStringLiteral("PointToEdit"))->setText(QStringLiteral("other"));
    Screenshot(W, QStringLiteral("link-edit"));
    static_cast<QDialog *>(W)->accept();
  });
  CHECK(Dialog.Execute(FileName, PointTo, Symbolic, true, true));
  CHECK(PointTo == QStringLiteral("other"));
  CHECK(Symbolic);

  LinkDialog Add;
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [&](QWidget * W)
  {
    CHECK(W->windowTitle() == QStringLiteral("Add link"));
    QLineEdit * Name = W->findChild<QLineEdit *>(QStringLiteral("FileNameEdit"));
    CHECK(!Name->isReadOnly());
    CHECK(!OkButtonOf(W)->isEnabled());
    Name->setText(QStringLiteral("hard"));
    CHECK(OkButtonOf(W)->isEnabled());
    QCheckBox * Hard = W->findChild<QCheckBox *>(QStringLiteral("HardLinkCheck"));
    CHECK(Hard->isEnabled());
    Hard->setChecked(true);
    static_cast<QDialog *>(W)->accept();
  });
  FileName.clear();
  PointTo = QStringLiteral("a.txt");
  Symbolic = true;
  CHECK(Add.Execute(FileName, PointTo, Symbolic, false, true));
  CHECK(FileName == QStringLiteral("hard"));
  CHECK(!Symbolic);
}

static void TestLocalOperations(const QString & Base)
{
  std::printf("[FileOperations: local]\n");
  QDir().mkpath(Base);
  const QString A = Base + QStringLiteral("/a.txt");
  const QString Run = Base + QStringLiteral("/run.sh");
  WriteFile(A, QByteArrayLiteral("hello\n"), 0644);
  WriteFile(Run, QByteArrayLiteral("#!/bin/sh\n"), 0755);
  QList<RemoteFile> Files = {MakeFile(QStringLiteral("a.txt"), 0644), MakeFile(QStringLiteral("run.sh"), 0755)};
  // Others W on a mixed selection keeps run.sh executable
  OnModal([](QWidget * W) { return IsA<PropertiesDialog>(W); }, [](QWidget * W)
  {
    RightsCheck(W, 2, QStringLiteral("W"))->setCheckState(Qt::Checked);
    OkButtonOf(W)->click();
  });
  bool Changed = false;
  FileOperations::Properties(nullptr, nullptr, Files, {A, Run}, Base, FileOperations::Runner(), [&](bool C) { Changed = C; });
  CHECK(Changed);
  CHECK_MSG(ModeOf(A) == 0646, Rights::ToOctal(ModeOf(A)));
  CHECK_MSG(ModeOf(Run) == 0757, Rights::ToOctal(ModeOf(Run)));

  // links
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [](QWidget * W)
  {
    W->findChild<QLineEdit *>(QStringLiteral("FileNameEdit"))->setText(QStringLiteral("soft"));
    OkButtonOf(W)->click();
  });
  RemoteFile Focused = Files[0];
  bool Created = false;
  FileOperations::AddEditLink(nullptr, nullptr, Base, &Focused, [&](bool C) { Created = C; });
  CHECK(Created);
  CHECK(QFileInfo(Base + QStringLiteral("/soft")).isSymLink());
  CHECK(QFile::symLinkTarget(Base + QStringLiteral("/soft")) == QFileInfo(A).absoluteFilePath());
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [](QWidget * W)
  {
    W->findChild<QLineEdit *>(QStringLiteral("FileNameEdit"))->setText(QStringLiteral("hard"));
    W->findChild<QCheckBox *>(QStringLiteral("HardLinkCheck"))->setChecked(true);
    OkButtonOf(W)->click();
  });
  Created = false;
  FileOperations::AddEditLink(nullptr, nullptr, Base, &Focused, [&](bool C) { Created = C; });
  CHECK(Created);
  struct stat S1 = {}, S2 = {};
  stat(QFile::encodeName(A).constData(), &S1);
  stat(QFile::encodeName(Base + QStringLiteral("/hard")).constData(), &S2);
  CHECK(S1.st_ino == S2.st_ino);
  // edit the symlink
  RemoteFile Link = MakeFile(QStringLiteral("soft"), 0777);
  Link.IsSymLink = true;
  Link.LinkTo = QStringLiteral("a.txt");
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [](QWidget * W)
  {
    CHECK(W->windowTitle() == QStringLiteral("Edit link"));
    W->findChild<QLineEdit *>(QStringLiteral("PointToEdit"))->setText(QStringLiteral("run.sh"));
    OkButtonOf(W)->click();
  });
  FileOperations::AddEditLink(nullptr, nullptr, Base, &Link, nullptr);
  CHECK(QFile::symLinkTarget(Base + QStringLiteral("/soft")) == QFileInfo(Run).absoluteFilePath());

  // sizes and checksums
  QDir().mkpath(Base + QStringLiteral("/d/e"));
  WriteFile(Base + QStringLiteral("/d/e/f"), QByteArray(100, 'x'), 0644);
  qint64 Size = 0;
  Terminal::CalculateSizeStats Stats;
  CHECK(FileOperations::CalculateSize(nullptr, nullptr, {Base + QStringLiteral("/d"), A}, Size, Stats));
  CHECK(Size == 106);
  CHECK(Stats.Files == 2);
  CHECK(Stats.Directories == 2);
  QHash<QString, qint64> Sizes;
  FileOperations::CalculateDirectorySizes(nullptr, nullptr, {Base + QStringLiteral("/d")}, [&](const QHash<QString, qint64> & S) { Sizes = S; });
  CHECK(Sizes.value(Base + QStringLiteral("/d")) == 100);
  QMap<QString, QString> Sums;
  FileOperations::CalculateChecksum(nullptr, nullptr, QStringLiteral("md5"), {A, Base + QStringLiteral("/d")},
    [&](const QString & Name, const QString &, const QString & Hash) { Sums.insert(Name, Hash); });
  CHECK(Sums.value(QStringLiteral("a.txt")) == QString::fromLatin1(QCryptographicHash::hash("hello\n", QCryptographicHash::Md5).toHex()));
  CHECK(Sums.contains(QStringLiteral("d/e/f")));

  // create folder (local: a path)
  OnModal([](QWidget * W) { return IsA<CreateDirectoryDialog>(W); }, [](QWidget * W)
  {
    W->findChild<QLineEdit *>(QStringLiteral("DirectoryEdit"))->setText(QStringLiteral("x/y"));
    OkButtonOf(W)->click();
  });
  QString NewPath;
  FileOperations::CreateDirectory(nullptr, nullptr, Base, FileOperations::Runner(), [&](const QString & P) { NewPath = P; });
  CHECK(QFileInfo(Base + QStringLiteral("/x/y")).isDir());
}

//---------------------------------------------------------------------------
struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  bool Called = false;
  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
  }
  OperationResult Wait()
  {
    if (!Called)
    {
      QTimer::singleShot(60000, &Loop, [this]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
      Loop.exec();
    }
    return Result;
  }
};

class SilentUI : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override { return HostKeyOnce; }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    std::printf("  query: %s\n", qPrintable(Query.left(120)));
    return qaYes;
  }
};

// Waits until Pred holds (operations run asynchronously after the dialogs)
static bool WaitUntil(const std::function<bool()> & Pred, int TimeoutMs = 30000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Pred() && (Timer.elapsed() < TimeoutMs))
  {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return Pred();
}

static void TestRemote(const QString & Base)
{
  QString PortStr = qEnvironmentVariable("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("[FileOperations: SFTP] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("[FileOperations: SFTP]\n");
  QDir().mkpath(Base);
  SessionData Data;
  Data.FSProtocol = fsSFTPonly;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Base + QStringLiteral("/known_hosts");
  SilentUI UI;
  Terminal T(Data, &UI);
  {
    Waiter W;
    T.Open(W.Cb());
    CHECK(W.Wait().Ok);
  }
  const QString A = Base + QStringLiteral("/a.txt");
  const QString Run = Base + QStringLiteral("/run.sh");
  WriteFile(A, QByteArrayLiteral("hello\n"), 0644);
  WriteFile(Run, QByteArrayLiteral("#!/bin/sh\n"), 0755);
  QDir().mkpath(Base + QStringLiteral("/d"));
  WriteFile(Base + QStringLiteral("/d/f"), QByteArray(50, 'y'), 0600);
  QList<RemoteFile> Files = {MakeFile(QStringLiteral("a.txt"), 0644), MakeFile(QStringLiteral("run.sh"), 0755)};

  // Properties: owner/group combos offered (SFTP v3), tri-state permissions
  OnModal([](QWidget * W) { return IsA<PropertiesDialog>(W); }, [](QWidget * W)
  {
    CHECK(W->findChild<QComboBox *>(QStringLiteral("OwnerComboBox"))->isVisible());
    CHECK(W->findChild<QComboBox *>(QStringLiteral("GroupComboBox"))->isVisible());
    RightsCheck(W, 1, QStringLiteral("W"))->setCheckState(Qt::Checked);
    OkButtonOf(W)->click();
  });
  bool Changed = false;
  FileOperations::Properties(nullptr, &T, Files, {A, Run}, Base, FileOperations::Runner(), [&](bool C) { Changed = C; });
  CHECK(WaitUntil([&]() { return Changed; }));
  CHECK_MSG(ModeOf(A) == 0664, Rights::ToOctal(ModeOf(A)));
  CHECK_MSG(ModeOf(Run) == 0775, Rights::ToOctal(ModeOf(Run)));

  // Properties: checksum tab against the server (secondary shell)
  QString Hash;
  OnModal([](QWidget * W) { return IsA<PropertiesDialog>(W); }, [&](QWidget * W)
  {
    W->findChild<QComboBox *>(QStringLiteral("ChecksumAlgEdit"))->setCurrentText(QStringLiteral("md5"));
    W->findChild<QTabWidget *>()->setCurrentIndex(1);
    QTimer::singleShot(0, W, [W, &Hash]()
    {
      Hash = W->findChild<QLineEdit *>(QStringLiteral("ChecksumEdit"))->text();
      static_cast<QDialog *>(W)->reject();
    });
  });
  FileOperations::Properties(nullptr, &T, {Files[0]}, {A}, Base, FileOperations::Runner(), nullptr);
  CHECK_MSG(Hash == QString::fromLatin1(QCryptographicHash::hash("hello\n", QCryptographicHash::Md5).toHex()), Hash);

  // Duplicate directly (copy-data) and of a directory (separate shell, confirmed)
  bool Duplicated = false;
  OnModal([](QWidget * W) { return IsA<RemoteTransferDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteTransferDialog *>(W);
    CHECK(D->DirectoryEdit()->currentText() == Base + QStringLiteral("/a.txt"));
    D->DirectoryEdit()->setEditText(Base + QStringLiteral("/b.txt"));
    D->accept();
  });
  FileOperations::Duplicate(nullptr, &T, {Files[0]}, {A}, Base, {}, FileOperations::Runner(),
    [&](Terminal *) { Duplicated = true; });
  CHECK(WaitUntil([&]() { return Duplicated; }));
  CHECK(QFileInfo(Base + QStringLiteral("/b.txt")).size() == 6);

  Configuration::Instance().ConfirmCommandSession = false;
  Duplicated = false;
  RemoteFile Dir = MakeFile(QStringLiteral("d"), 0755, true);
  OnModal([](QWidget * W) { return IsA<RemoteTransferDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteTransferDialog *>(W);
    D->DirectoryEdit()->setEditText(Base + QStringLiteral("/d2"));
    D->accept();
  });
  FileOperations::Duplicate(nullptr, &T, {Dir}, {Base + QStringLiteral("/d")}, Base, {}, FileOperations::Runner(),
    [&](Terminal *) { Duplicated = true; });
  CHECK(WaitUntil([&]() { return Duplicated; }));
  CHECK(QFileInfo(Base + QStringLiteral("/d2/f")).size() == 50);

  // Duplicate via local temporary copy, with a mask
  Duplicated = false;
  QDir().mkpath(Base + QStringLiteral("/tmpcopy"));
  OnModal([](QWidget * W) { return IsA<RemoteTransferDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteTransferDialog *>(W);
    D->DirectoryEdit()->setEditText(Base + QStringLiteral("/tmpcopy/*.bak"));
    D->findChild<QCheckBox *>(QStringLiteral("NotDirectCopyCheck"))->setChecked(true);
    D->accept();
  });
  FileOperations::Duplicate(nullptr, &T, Files, {A, Run}, Base, {}, FileOperations::Runner(),
    [&](Terminal *) { Duplicated = true; });
  CHECK(WaitUntil([&]() { return Duplicated; }));
  CHECK(QFileInfo(Base + QStringLiteral("/tmpcopy/a.bak")).size() == 6);
  CHECK(QFileInfo(Base + QStringLiteral("/tmpcopy/run.bak")).exists());

  // Move To
  bool Moved = false;
  QDir().mkpath(Base + QStringLiteral("/moved"));
  OnModal([](QWidget * W) { return IsA<RemoteMoveDialog>(W); }, [&](QWidget * W)
  {
    auto * D = static_cast<RemoteMoveDialog *>(W);
    // an existing directory gets "/*.*"
    D->DirectoryEdit()->setEditText(Base + QStringLiteral("/moved"));
    D->accept();
  });
  FileOperations::MoveTo(nullptr, &T, {Base + QStringLiteral("/b.txt")}, Base, FileOperations::Runner(), [&](bool M) { Moved = M; });
  CHECK(WaitUntil([&]() { return Moved; }));
  CHECK(QFileInfo(Base + QStringLiteral("/moved/b.txt")).exists());

  // Add a hard link, then edit a symlink
  bool Created = false;
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [](QWidget * W)
  {
    CHECK(W->findChild<QCheckBox *>(QStringLiteral("HardLinkCheck"))->isEnabled());
    W->findChild<QLineEdit *>(QStringLiteral("FileNameEdit"))->setText(QStringLiteral("hl"));
    W->findChild<QCheckBox *>(QStringLiteral("HardLinkCheck"))->setChecked(true);
    OkButtonOf(W)->click();
  });
  FileOperations::AddEditLink(nullptr, &T, Base, &Files[0], [&](bool C) { Created = C; });
  CHECK(WaitUntil([&]() { return Created; }));
  struct stat S1 = {}, S2 = {};
  stat(QFile::encodeName(A).constData(), &S1);
  stat(QFile::encodeName(Base + QStringLiteral("/hl")).constData(), &S2);
  CHECK(S1.st_ino == S2.st_ino);
  QFile::link(QStringLiteral("a.txt"), Base + QStringLiteral("/sl"));
  RemoteFile Link = MakeFile(QStringLiteral("sl"), 0777);
  Link.IsSymLink = true;
  Link.LinkTo = QStringLiteral("a.txt");
  Created = false;
  OnModal([](QWidget * W) { return IsA<LinkDialog>(W); }, [](QWidget * W)
  {
    W->findChild<QLineEdit *>(QStringLiteral("PointToEdit"))->setText(QStringLiteral("run.sh"));
    OkButtonOf(W)->click();
  });
  FileOperations::AddEditLink(nullptr, &T, Base, &Link, [&](bool C) { Created = C; });
  CHECK(WaitUntil([&]() { return Created; }));
  CHECK(QFile::symLinkTarget(Base + QStringLiteral("/sl")) == QFileInfo(Run).absoluteFilePath());

  // Create folder with permissions
  QString NewPath;
  OnModal([](QWidget * W) { return IsA<CreateDirectoryDialog>(W); }, [](QWidget * W)
  {
    W->findChild<QLineEdit *>(QStringLiteral("DirectoryEdit"))->setText(QStringLiteral("nd"));
    W->findChild<QCheckBox *>(QStringLiteral("SetRightsCheck"))->setChecked(true);
    for (QLineEdit * E : W->findChildren<QLineEdit *>())
    {
      if (E->maxLength() == 4)
      {
        E->setText(QStringLiteral("0711"));
        emit E->textEdited(QStringLiteral("0711"));
      }
    }
    OkButtonOf(W)->click();
  });
  FileOperations::CreateDirectory(nullptr, &T, Base, FileOperations::Runner(), [&](const QString & P) { NewPath = P; });
  CHECK(WaitUntil([&]() { return !NewPath.isEmpty(); }));
  CHECK_MSG(ModeOf(Base + QStringLiteral("/nd")) == 0711, Rights::ToOctal(ModeOf(Base + QStringLiteral("/nd"))));

  // Calculate Directory Sizes and space available
  QHash<QString, qint64> Sizes;
  FileOperations::CalculateDirectorySizes(nullptr, &T, {Base + QStringLiteral("/d")}, [&](const QHash<QString, qint64> & S) { Sizes = S; });
  CHECK(Sizes.value(Base + QStringLiteral("/d")) == 50);
  ServerInfoDialog::GetSpaceAvailableFunc Space = FileOperations::SpaceAvailableFunc(nullptr, &T);
  CHECK(static_cast<bool>(Space));
  ServerInfoDialog::SpaceAvailable Info;
  QString Error;
  CHECK(Space && Space(Base, Info, Error));
  CHECK(Info.BytesOnDevice > 0);
  CHECK(FileOperations::RemoteDirectoryExists(&T, Base + QStringLiteral("/d/")));
  CHECK(!FileOperations::RemoteDirectoryExists(&T, A));
  T.Close();
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication App(argc, argv);
  // Expected texts use English digit grouping ("2,048"), whatever the host locale
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  ScreenshotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT_DIR");

  TestRightsWidget();
  TestPropertiesDialog();
  TestCreateDirectoryDialog();
  TestRemoteTransferDialogs();
  TestLinkDialog();
  TestLocalOperations(Scratch.filePath(QStringLiteral("local")));
  TestRemote(Scratch.filePath(QStringLiteral("remote")));

  std::printf(Failures ? "test_propertiesdialog: %d FAILURES\n" : "test_propertiesdialog: OK\n", Failures);
  return Failures ? 1 : 0;
}
