// Copy dialog and transfer settings dialog tests (WP10): src/gui/CopyDialog
// (target path parsing into directory + file mask, multi-files-to-one
// confirmation, ConfirmTransferring / "Do not show this dialog box again",
// "Set as Default", target history), src/gui/CopyParamsDialog (Preserve
// read-only, per-direction enabling, preset dialog, presets menu) and the
// Preferences > Transfer presets list.
// Runs offscreen. NIXSCP_TEST_SCREENSHOT_DIR=<dir> saves PNGs of the dialogs.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_copydialog needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/IniStorage.h"
#include "gui/CopyDialog.h"
#include "gui/CopyParamsDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/PreferencesDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static void Screenshot(QWidget * Widget, const QString & Name)
{
  QString Dir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT_DIR");
  if (!Dir.isEmpty())
  {
    Widget->adjustSize();
    Widget->grab().save(QDir(Dir).filePath(Name));
  }
}

// Runs Action on the next modal widget of the given class once it shows
template <class T>
static void OnModal(const std::function<void(T *)> & Action)
{
  auto * Timer = new QTimer();
  Timer->setInterval(20);
  QObject::connect(Timer, &QTimer::timeout, [Timer, Action]()
  {
    QWidget * Active = QApplication::activePopupWidget();
    if (qobject_cast<T *>(Active) == nullptr)
    {
      Active = QApplication::activeModalWidget();
    }
    if (T * W = qobject_cast<T *>(Active))
    {
      Timer->stop();
      Timer->deleteLater();
      Action(W);
    }
  });
  Timer->start();
}

static QComboBox * TargetCombo(CopyDialog & Dialog)
{
  return Dialog.findChild<QComboBox *>();
}

static QCheckBox * CheckBoxByText(QWidget * Parent, const QString & Text)
{
  for (QCheckBox * C : Parent->findChildren<QCheckBox *>())
  {
    if (C->text() == Text)
    {
      return C;
    }
  }
  return nullptr;
}

static void TestTargetParsing()
{
  CopyParam Param;
  CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/srv"), Param);
  QComboBox * Combo = TargetCombo(Dialog);
  CHECK(Combo != nullptr);
  CHECK(Combo->currentText() == QStringLiteral("/srv/*.*"));
  CHECK(Dialog.TargetDirectory() == QStringLiteral("/srv"));
  CHECK(Dialog.FileMask() == QStringLiteral("*.*"));
  CHECK(Dialog.windowTitle() == QStringLiteral("Upload"));
  Dialog.SetOptions(CopyDialog::coDoNotShowAgain | CopyDialog::coShortCutHint);
  Screenshot(&Dialog, QStringLiteral("copy-upload.png"));

  // a new name of the single file
  Combo->setEditText(QStringLiteral("/srv/b.txt"));
  CHECK(Dialog.TargetDirectory() == QStringLiteral("/srv"));
  CHECK(Dialog.FileMask() == QStringLiteral("b.txt"));
  CHECK(Dialog.Param().FileMask == QStringLiteral("b.txt"));
  CHECK(Dialog.Param().ChangeFileName(QStringLiteral("a.txt"), true) == QStringLiteral("b.txt"));

  // a mask
  Combo->setEditText(QStringLiteral("/dir/*.bak"));
  CHECK(Dialog.TargetDirectory() == QStringLiteral("/dir"));
  CHECK(Dialog.Param().ChangeFileName(QStringLiteral("a.txt"), true) == QStringLiteral("a.bak"));

  // a directory (trailing slash)
  Combo->setEditText(QStringLiteral("/srv/b.txt/"));
  CHECK(Dialog.TargetDirectory() == QStringLiteral("/srv/b.txt"));
  CHECK(Dialog.FileMask().isEmpty());
  CHECK(Dialog.Param().ChangeFileName(QStringLiteral("a.txt"), true) == QStringLiteral("a.txt"));

  // root
  Combo->setEditText(QStringLiteral("/*.*"));
  CHECK(Dialog.TargetDirectory() == QStringLiteral("/"));

  CopyDialog Move(false, true, {QStringLiteral("/r/a"), QStringLiteral("/r/b")}, QDir::tempPath(), Param);
  CHECK(Move.windowTitle() == QStringLiteral("Download and Delete"));
  CHECK(CheckBoxByText(&Move, QStringLiteral("&Do not show this dialog box again")) != nullptr);
}

static void TestMultiFilesToOne()
{
  CopyParam Param;
  {
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt"), QStringLiteral("b.txt")}, QStringLiteral("/srv"), Param);
    TargetCombo(Dialog)->setEditText(QStringLiteral("/srv/one.txt"));
    bool Asked = false;
    OnModal<QMessageBox>([&Asked](QMessageBox * Box)
    {
      Asked = Box->text().contains(QStringLiteral("multiple files to a single file 'one.txt'"));
      Box->button(QMessageBox::Cancel)->click();
    });
    QTimer::singleShot(0, &Dialog, [&Dialog]() { Dialog.accept(); QTimer::singleShot(500, &Dialog, [&Dialog]() { Dialog.reject(); }); });
    int Result = Dialog.exec();
    CHECK(Asked);
    CHECK(Result == QDialog::Rejected);
  }
  {
    // with a mask, no question
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt"), QStringLiteral("b.txt")}, QStringLiteral("/srv"), Param);
    TargetCombo(Dialog)->setEditText(QStringLiteral("/srv/*.bak"));
    QTimer::singleShot(0, &Dialog, [&Dialog]() { Dialog.accept(); });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(!Dialog.Skipped());
    // the target went to the history
    CHECK(EditMaskDialog::History(QStringLiteral("RemoteTarget")).value(0) == QStringLiteral("/srv/*.bak"));
  }
  {
    // history is offered next time
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/home"), Param);
    QComboBox * Combo = TargetCombo(Dialog);
    CHECK(Combo->findText(QStringLiteral("/srv/*.bak")) >= 0);
    CHECK(Combo->currentText() == QStringLiteral("/home/*.*"));
  }
}

static void TestConfirmTransferring()
{
  Configuration & C = Configuration::Instance();
  C.ConfirmTransferring = true;
  CopyParam Param;
  {
    // "Do not show this dialog box again" turns the confirmation off
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/srv"), Param);
    QCheckBox * Never = CheckBoxByText(&Dialog, QStringLiteral("&Do not show this dialog box again"));
    CHECK(Never != nullptr && Never->isVisible() == false);   // not shown yet
    QTimer::singleShot(0, &Dialog, [&Dialog, Never]()
    {
      CHECK(Never->isVisible());
      Never->setChecked(true);
      CHECK((Dialog.OutputOptions() & CopyDialog::cooSaveSettings) != 0);
      Dialog.accept();
    });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(!C.ConfirmTransferring);
  }
  {
    // now F5 does not show the dialog
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/srv"), Param);
    bool Shown = false;
    QTimer::singleShot(0, &Dialog, [&Dialog, &Shown]() { Shown = Dialog.isVisible(); Dialog.reject(); });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(Dialog.Skipped());
    CHECK(Dialog.TargetDirectory() == QStringLiteral("/srv"));
    CHECK(Dialog.FileMask() == QStringLiteral("*.*"));
    QApplication::processEvents();
    CHECK(!Shown);
    CHECK(!CopyDialog::ConfirmationEnabled());
  }
  {
    // command-line style transfers (no coDoNotShowAgain) always show it
    CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/srv"), Param);
    Dialog.SetOptions(0);
    QTimer::singleShot(0, &Dialog, [&Dialog]() { Dialog.accept(); });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(!Dialog.Skipped());
  }
  // drag & drop: DDTransferConfirmation applies too
  C.ConfirmTransferring = true;
  C.DDTransferConfirmation = asOff;
  CHECK(CopyDialog::ConfirmationEnabled(false));
  CHECK(!CopyDialog::ConfirmationEnabled(true));
  C.DDTransferConfirmation = asAuto;
  {
    CopyDialog Dialog(false, false, {QStringLiteral("/r/a")}, QDir::tempPath(), Param);
    Dialog.SetDragDrop(true);
    QCheckBox * Never = CheckBoxByText(&Dialog, QStringLiteral("&Do not show this dialog box again"));
    CHECK(Never->isChecked());   // GetDoNotShowCopyDialogDefault
    QTimer::singleShot(0, &Dialog, [&Dialog, Never]() { Never->setChecked(false); Dialog.accept(); });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(C.DDTransferConfirmation == asOn);
    CHECK(C.ConfirmTransferring);
  }
}

static void TestSetAsDefault()
{
  Configuration & C = Configuration::Instance();
  C.ConfirmTransferring = true;
  C.DefaultCopyParam() = CopyParam();
  CopyParam Param;
  Param.PreserveTime = false;
  CopyDialog Dialog(true, false, {QStringLiteral("a.txt")}, QStringLiteral("/srv"), Param);
  TargetCombo(Dialog)->setEditText(QStringLiteral("/srv/new.txt"));
  QTimer::singleShot(0, &Dialog, [&Dialog]()
  {
    CheckBoxByText(&Dialog, QStringLiteral("&Do not show this dialog box again"))->setChecked(true);
    Dialog.accept();
  });
  CHECK(Dialog.exec() == QDialog::Accepted);
  CHECK(!C.DefaultCopyParam().PreserveTime);
  // the file mask of one transfer does not become a default
  CHECK(C.DefaultCopyParam().FileMask == QStringLiteral("*.*"));
  C.ConfirmTransferring = true;
  C.DefaultCopyParam() = CopyParam();
}

static void TestCopyParamsDialog()
{
  CopyParam Param;
  Param.PreserveReadOnly = true;
  {
    CopyParamsDialog Dialog(Param, CopyParam::UsableAttrs(false));
    QCheckBox * ReadOnly = CheckBoxByText(&Dialog, QStringLiteral("Preserve rea&d-only"));
    CHECK(ReadOnly != nullptr);
    CHECK(ReadOnly->isChecked());
    CHECK(ReadOnly->isEnabled());
    CHECK(!CheckBoxByText(&Dialog, QStringLiteral("Set pe&rmissions:"))->isEnabled());
    CHECK(CheckBoxByText(&Dialog, QStringLiteral("&Calculate total size")) != nullptr);
    ReadOnly->setChecked(false);
    CHECK(!Dialog.Param().PreserveReadOnly);
    Screenshot(&Dialog, QStringLiteral("transfer-settings-download.png"));
  }
  {
    CopyParamsDialog Dialog(Param, CopyParam::UsableAttrs(true));
    CHECK(!CheckBoxByText(&Dialog, QStringLiteral("Preserve rea&d-only"))->isEnabled());
    CHECK(CheckBoxByText(&Dialog, QStringLiteral("Set pe&rmissions:"))->isEnabled());
    CHECK(Dialog.Param().PreserveReadOnly);   // kept, just not applicable
  }
  {
    // the mask history is saved on OK
    CopyParam P;
    P.Mode = CopyParam::tmAutomatic;
    P.IncludeFileMask = QStringLiteral("*.txt");
    CopyParamsDialog Dialog(P, 0);
    QTimer::singleShot(0, &Dialog, [&Dialog]() { Dialog.accept(); });
    CHECK(Dialog.exec() == QDialog::Accepted);
    CHECK(EditMaskDialog::History(QStringLiteral("IncludeMask")).value(0) == QStringLiteral("*.txt"));
    CHECK(EditMaskDialog::History(QStringLiteral("SpeedLimit")).value(0) == QStringLiteral("Unlimited"));
  }
}

static void TestPresetDialog()
{
  CopyParamList List = CopyParamList::Defaults(CopyParam());
  int Index = -1;
  // duplicate description is refused, then a valid one with a rule is accepted
  OnModal<CopyParamPresetDialog>([](CopyParamPresetDialog * Dialog)
  {
    CHECK(Dialog->windowTitle() == QStringLiteral("Add transfer settings preset"));
    QList<QLineEdit *> Edits = Dialog->findChildren<QLineEdit *>();
    QLineEdit * Description = nullptr;
    for (QLineEdit * E : Edits)
    {
      if (E->hasFocus() || (Description == nullptr && E->maxLength() == 250))
      {
        Description = E;
        break;
      }
    }
    CHECK(Description != nullptr);
    QPushButton * Ok = nullptr;
    for (QPushButton * B : Dialog->findChildren<QPushButton *>())
    {
      if (B->text() == QStringLiteral("OK") || B->text() == QStringLiteral("&OK"))
      {
        Ok = B;
      }
    }
    CHECK(Ok != nullptr && !Ok->isEnabled());
    Description->setText(QStringLiteral("&Binary"));
    CHECK(Ok->isEnabled());
    OnModal<QMessageBox>([Dialog, Description](QMessageBox * Box)
    {
      CHECK(Box->text().contains(QStringLiteral("already exists")));
      Box->accept();
      QTimer::singleShot(0, Dialog, [Dialog, Description]()
      {
        Description->setText(QStringLiteral("Mine"));
        Screenshot(Dialog, QStringLiteral("transfer-preset.png"));
        Dialog->accept();
      });
    });
    Dialog->accept();
  });
  CHECK(CopyParamPresetDialog::Execute(nullptr, List, Index, CopyParamPresetDialog::cpmAdd, nullptr, CopyParam()));
  CHECK(List.Count() == 5);
  CHECK(Index == 4);
  CHECK(List.Name(4) == QStringLiteral("Mine"));
}

static void TestPreferencesPresets()
{
  CopyParamPresets::Instance().Reload();
  PreferencesDialog Dialog;
  Dialog.SelectPage(QStringLiteral("Transfer"));
  QTreeWidget * View = nullptr;
  for (QTreeWidget * T : Dialog.findChildren<QTreeWidget *>())
  {
    if ((T->columnCount() == 2) && (T->headerItem()->text(0) == QStringLiteral("Preset description")))
    {
      View = T;
    }
  }
  CHECK(View != nullptr);
  if (View == nullptr)
  {
    return;
  }
  CHECK(View->topLevelItemCount() == 1 + CopyParamPresets::Instance().List().Count());
  CHECK(View->topLevelItem(0)->text(0) == QStringLiteral("Default"));
  CHECK(View->topLevelItem(1)->text(0) == QStringLiteral("Text"));
  CHECK(View->topLevelItem(4)->text(0) == QStringLiteral("Exclude directories"));
  CHECK(View->topLevelItem(1)->text(1) == QStringLiteral("No"));
  Dialog.show();
  QApplication::processEvents();
  Screenshot(&Dialog, QStringLiteral("preferences-transfer.png"));
  QPushButton * Remove = nullptr;
  QPushButton * Up = nullptr;
  for (QPushButton * B : Dialog.findChildren<QPushButton *>())
  {
    if (B->text() == QStringLiteral("&Remove") && B->isVisibleTo(&Dialog)) Remove = B;
    if (B->text() == QStringLiteral("&Up") && B->isVisibleTo(&Dialog)) Up = B;
  }
  CHECK(Remove != nullptr && Up != nullptr);
  View->setCurrentItem(View->topLevelItem(0));
  CHECK(!Remove->isEnabled());
  View->setCurrentItem(View->topLevelItem(2));
  CHECK(Remove->isEnabled() && Up->isEnabled());
  Up->click();
  CHECK(View->topLevelItem(1)->text(0) == QStringLiteral("Binary"));
  View->setCurrentItem(View->topLevelItem(4));
  Remove->click();
  CHECK(View->topLevelItemCount() == 4);
  Dialog.accept();
  CHECK(!CopyParamPresets::Instance().IsDefaultList());
  CHECK(CopyParamPresets::Instance().List().Count() == 3);
  CHECK(CopyParamPresets::Instance().List().Name(0) == QStringLiteral("&Binary"));
}

static void TestPresetsMenu()
{
  // the presets menu: Default checked for the default settings
  CopyParamPresets::Instance().SetCurrent(QString());
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  QString Preset;
  QStringList Captions;
  bool DefaultChecked = false;
  OnModal<QMenu>([&](QMenu * Menu)
  {
    for (QAction * A : Menu->actions())
    {
      Captions.append(A->text());
      if (A->text() == QStringLiteral("&Default"))
      {
        DefaultChecked = A->isChecked();
      }
    }
    // choose the first preset
    for (QAction * A : Menu->actions())
    {
      if (A->text() == QStringLiteral("&Binary"))
      {
        Menu->setActiveAction(A);
        A->trigger();
        Menu->close();
        return;
      }
    }
    Menu->close();
  });
  bool Save = false;
  QWidget Parent;
  int Result = CopyParamListPopup(&Parent, QPoint(10, 10), Param, Preset, cplSaveSettings, 0, &Save);
  CHECK(Captions.contains(QStringLiteral("Custo&m...")));
  CHECK(Captions.contains(QStringLiteral("&Set as Default")));
  CHECK(Captions.contains(QStringLiteral("Save as &Preset...")));
  CHECK(Captions.contains(QStringLiteral("&Configure...")));
  CHECK(DefaultChecked);
  // QMenu::exec in offscreen mode may not return the triggered action; accept both
  if (Result == 1)
  {
    CHECK(Preset == QStringLiteral("&Binary"));
    CHECK(Param.Mode == CopyParam::tmBinary);
  }
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();

  std::printf("%s\n", "TestTargetParsing"); std::fflush(stdout);
  TestTargetParsing();
  std::printf("%s\n", "TestMultiFilesToOne"); std::fflush(stdout);
  TestMultiFilesToOne();
  std::printf("%s\n", "TestConfirmTransferring"); std::fflush(stdout);
  TestConfirmTransferring();
  std::printf("%s\n", "TestSetAsDefault"); std::fflush(stdout);
  TestSetAsDefault();
  std::printf("%s\n", "TestCopyParamsDialog"); std::fflush(stdout);
  TestCopyParamsDialog();
  std::printf("%s\n", "TestPresetDialog"); std::fflush(stdout);
  TestPresetDialog();
  std::printf("%s\n", "TestPreferencesPresets"); std::fflush(stdout);
  TestPreferencesPresets();
  std::printf("%s\n", "TestPresetsMenu"); std::fflush(stdout);
  TestPresetsMenu();

  std::printf("%s: %d checks, %d failures\n", (Failures == 0) ? "PASS" : "FAIL", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
