// File panel tests (WP01): src/gui/FileListModel (sorting, marks, size formats,
// file colors), src/gui/FilePanel (Norton-like keyboard/mouse marking, local
// listing on a worker thread, auto-refresh, DirViewParams persistence,
// incremental search, operation on double-click, reading indicator).
//
// The GUI part needs Qt Widgets and the GUI sources, which the default test
// targets do not link (they link nixscpcore only). Build with
// -DNIXSCP_GUI_TESTS and src/gui/FilePanel.cpp + src/gui/FileListModel.cpp +
// Qt6::Widgets to run it; otherwise the test only reports that it was skipped.
// Runs offscreen (QT_QPA_PLATFORM=offscreen is set when unset).
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_filepanel needs NIXSCP_GUI_TESTS (Qt Widgets + src/gui/FilePanel.cpp, FileListModel.cpp)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"
#include "gui/FileListModel.h"
#include "gui/FilePanel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeView>

#include <cstdio>
#include <functional>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { Checks++; auto CheckA = (A); auto CheckB = (B); if (!(CheckA == CheckB)) { \
    std::printf("FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #A, #B, \
      qPrintable(QVariant::fromValue(CheckA).toString()), qPrintable(QVariant::fromValue(CheckB).toString())); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 5000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition() && (Timer.elapsed() < Ms))
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return Condition();
}

static void Pump(int Ms)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Timer.elapsed() < Ms)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
}

static void Key(QWidget * Widget, int KeyCode, Qt::KeyboardModifiers Mods = Qt::NoModifier, const QString & Text = QString())
{
  QKeyEvent Press(QEvent::KeyPress, KeyCode, Mods, Text);
  QApplication::sendEvent(Widget, &Press);
  QKeyEvent Release(QEvent::KeyRelease, KeyCode, Mods, Text);
  QApplication::sendEvent(Widget, &Release);
}

static void Click(QTreeView * View, int Row, Qt::KeyboardModifiers Mods = Qt::NoModifier,
  Qt::MouseButton Button = Qt::LeftButton)
{
  QPoint Pos = View->visualRect(View->model()->index(Row, 0)).center();
  QPoint Global = View->viewport()->mapToGlobal(Pos);
  QMouseEvent Press(QEvent::MouseButtonPress, Pos, Global, Button, Button, Mods);
  QApplication::sendEvent(View->viewport(), &Press);
  QMouseEvent Release(QEvent::MouseButtonRelease, Pos, Global, Button, Qt::NoButton, Mods);
  QApplication::sendEvent(View->viewport(), &Release);
}

static void DoubleClick(QTreeView * View, int Row)
{
  Click(View, Row);
  QPoint Pos = View->visualRect(View->model()->index(Row, 0)).center();
  QPoint Global = View->viewport()->mapToGlobal(Pos);
  QMouseEvent Double(QEvent::MouseButtonDblClick, Pos, Global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(View->viewport(), &Double);
  QMouseEvent Release(QEvent::MouseButtonRelease, Pos, Global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(View->viewport(), &Release);
}

static void WriteFile(const QString & Path, qint64 Size)
{
  QFile File(Path);
  if (File.open(QIODevice::WriteOnly))
  {
    File.write(QByteArray(Size, 'x'));
  }
}

static RemoteFile MakeFile(const QString & Name, qint64 Size, bool Directory = false, int AgeSeconds = 0)
{
  RemoteFile File;
  File.FileName = Name;
  File.Size = Size;
  File.IsDirectory = Directory;
  File.FileType = Directory ? RemoteFile::Directory : RemoteFile::File;
  File.Modification = QDateTime::fromSecsSinceEpoch(1700000000 - AgeSeconds);
  File.Permissions = Directory ? 0755 : 0644;
  File.Owner = QStringLiteral("user");
  File.Group = QStringLiteral("users");
  File.IsHidden = Name.startsWith(QLatin1Char('.'));
  return File;
}

static QStringList RowNames(const FileListModel & Model)
{
  QStringList Result;
  for (int Row = 0; Row < Model.rowCount(); Row++)
  {
    Result.append(Model.File(Row).FileName);
  }
  return Result;
}

//---------------------------------------------------------------------------

static void TestFormats()
{
  // BaseUtils.pas FormatBytes / FormatPanelBytes (en_US separators)
  CHECK_EQ(FileListModel::FormatPanelBytes(1234, fbNone), QStringLiteral("1,234"));
  CHECK_EQ(FileListModel::FormatPanelBytes(0, fbKilobytes), QStringLiteral("0 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1, fbKilobytes), QStringLiteral("1 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1024, fbKilobytes), QStringLiteral("1 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1025, fbKilobytes), QStringLiteral("2 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(5 * 1024 * 1024, fbKilobytes), QStringLiteral("5,120 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1023, fbShort), QStringLiteral("1,023 B"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1024, fbShort), QStringLiteral("1.00 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1536, fbShort), QStringLiteral("1.50 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(10 * 1024 + 1000, fbShort), QStringLiteral("10.9 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(150 * 1024, fbShort), QStringLiteral("150 KB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(1000 * 1024, fbShort), QStringLiteral("0.97 MB"));
  CHECK_EQ(FileListModel::FormatPanelBytes(qint64(5) * 1024 * 1024 * 1024, fbShort), QStringLiteral("5.00 GB"));
  CHECK_EQ(FileListModel::FormatBytes(0), QStringLiteral("0 B"));
  CHECK_EQ(FileListModel::FormatSizeKB(2047), QStringLiteral("2 KB"));
  // Date modified: short date + long time (with seconds)
  QString Changed = FileListModel::FormatModification(QDateTime(QDate(2024, 3, 5), QTime(14, 7, 9)));
  CHECK(Changed.contains(QStringLiteral(":07:09")));
}

static void TestFileColors()
{
  // TFileColorData: "BBGGRR:mask" in TStringList::CommaText
  QList<FileColorRule> Rules = FileColorRule::LoadList(QStringLiteral("0000FF:*.txt,\"00FF00:my file*.log\",FF0000:*/"));
  CHECK_EQ(Rules.size(), 3);
  if (Rules.size() == 3)
  {
    CHECK(Rules.at(0).Color == QColor(255, 0, 0));
    CHECK_EQ(Rules.at(0).Mask, QStringLiteral("*.txt"));
    CHECK(Rules.at(1).Color == QColor(0, 255, 0));
    CHECK_EQ(Rules.at(1).Mask, QStringLiteral("my file*.log"));
    CHECK(Rules.at(2).Color == QColor(0, 0, 255));
  }
  QString Saved = FileColorRule::SaveList(Rules);
  CHECK_EQ(Saved, QStringLiteral("0000FF:*.txt,\"00FF00:my file*.log\",FF0000:*/"));
  CHECK(FileColorRule::LoadList(QStringLiteral("zz:*.txt")).isEmpty());
}

static void TestModel()
{
  FileListModel Model(true);
  RemoteFileList Files;
  Files << MakeFile(QStringLiteral("file10.txt"), 100, false, 10)
        << MakeFile(QStringLiteral("file2.txt"), 3000, false, 30)
        << MakeFile(QStringLiteral("b.log"), 5000, false, 5)
        << MakeFile(QStringLiteral("zdir"), 0, true, 100)
        << MakeFile(QStringLiteral("Adir"), 0, true, 1)
        << MakeFile(QStringLiteral(".hidden"), 7, false, 1)
        << MakeFile(QStringLiteral("noext"), 1, false, 2);
  Model.SetFiles(QStringLiteral("/home/user"), Files);
  // ".." first, directories grouped first, natural order, hidden not shown
  CHECK_EQ(RowNames(Model), QStringList({"..", "Adir", "zdir", "b.log", "file2.txt", "file10.txt", "noext"}));
  CHECK_EQ(Model.HiddenCount(), 1);
  CHECK_EQ(Model.FileCount(), 6);
  CHECK_EQ(Model.TotalSize(), qint64(100 + 3000 + 5000 + 1));
  CHECK_EQ(Model.RowOfName(QStringLiteral("file10.txt")), 5);
  CHECK_EQ(Model.RowOfName(QStringLiteral(".hidden")), -1);

  Model.sort(FileListModel::ColSize, Qt::DescendingOrder);
  CHECK_EQ(RowNames(Model), QStringList({"..", "zdir", "Adir", "b.log", "file2.txt", "file10.txt", "noext"}));
  Model.sort(FileListModel::ColChanged, Qt::AscendingOrder);
  CHECK_EQ(RowNames(Model), QStringList({"..", "zdir", "Adir", "file2.txt", "file10.txt", "b.log", "noext"}));
  Model.SetAlwaysSortDirectoriesByName(true);
  CHECK_EQ(RowNames(Model), QStringList({"..", "Adir", "zdir", "file2.txt", "file10.txt", "b.log", "noext"}));
  Model.SetAlwaysSortDirectoriesByName(false);
  Model.sort(FileListModel::ColExt, Qt::AscendingOrder);
  CHECK_EQ(RowNames(Model).mid(3), QStringList({"noext", "b.log", "file2.txt", "file10.txt"}));
  Model.SetNaturalOrderNumericalSorting(false);
  Model.sort(FileListModel::ColName, Qt::AscendingOrder);
  CHECK_EQ(RowNames(Model).mid(3), QStringList({"b.log", "file10.txt", "file2.txt", "noext"}));
  Model.SetNaturalOrderNumericalSorting(true);
  CHECK_EQ(RowNames(Model).mid(3), QStringList({"b.log", "file2.txt", "file10.txt", "noext"}));
  CHECK_EQ(Model.RowOfName(QStringLiteral("file10.txt")), 5);

  // Columns
  CHECK_EQ(Model.index(5, FileListModel::ColExt).data().toString(), QStringLiteral(".txt"));
  CHECK_EQ(Model.index(5, FileListModel::ColSize).data().toString(), QStringLiteral("1 KB"));
  Model.SetFormatSizeBytes(fbNone);
  CHECK_EQ(Model.index(4, FileListModel::ColSize).data().toString(), QStringLiteral("3,000"));
  Model.SetFormatSizeBytes(fbKilobytes);
  CHECK_EQ(FileListModel::ColumnCaption(FileListModel::ColChanged), QStringLiteral("Date modified"));
  CHECK_EQ(FileListModel::ColumnCaption(FileListModel::ColLinkTarget), QStringLiteral("Link target"));
  FileListModel LocalModel(false);
  RemoteFile LocalFile = MakeFile(QStringLiteral("a.tar.gz"), 1);
  CHECK_EQ(LocalModel.Extension(LocalFile), QStringLiteral("GZ"));
  CHECK_EQ(Model.Extension(LocalFile), QStringLiteral(".gz"));

  // Marks: ".." cannot be marked; marks survive sorting and same-directory refresh
  int MarksChanged = 0;
  QObject::connect(&Model, &FileListModel::MarksChanged, [&MarksChanged]() { MarksChanged++; });
  Model.SetMarked(0, true);
  CHECK(!Model.IsMarked(0));
  CHECK_EQ(Model.MarkedCount(), 0);
  Model.SetMarked(Model.RowOfName(QStringLiteral("file2.txt")), true);
  Model.SetMarked(Model.RowOfName(QStringLiteral("zdir")), true);
  CHECK_EQ(Model.MarkedCount(), 2);
  CHECK_EQ(Model.MarkedSize(), qint64(3000));
  CHECK(MarksChanged >= 2);
  Model.sort(FileListModel::ColSize, Qt::AscendingOrder);
  CHECK(Model.IsMarked(Model.RowOfName(QStringLiteral("file2.txt"))));
  CHECK_EQ(Model.MarkedNames().size(), 2);
  Model.SetFiles(QStringLiteral("/home/user"), Files);
  CHECK_EQ(Model.MarkedCount(), 2);
  CHECK(Model.UnmarkFile(QStringLiteral("zdir")));
  CHECK(!Model.UnmarkFile(QStringLiteral("zdir")));
  CHECK_EQ(Model.MarkedCount(), 1);
  Model.MarkAll(true);
  CHECK_EQ(Model.MarkedCount(), 6);
  CHECK(!Model.IsMarked(0));
  Model.InvertMarks();
  CHECK_EQ(Model.MarkedCount(), 0);
  Model.SetMarkedRange(1, 3, true);
  CHECK_EQ(Model.MarkedCount(), 3);
  // Files hidden by a filter cannot stay marked
  Model.SetFilterMask(QStringLiteral("*.txt"));
  CHECK_EQ(Model.FilteredCount(), 2);
  CHECK_EQ(Model.MarkedCount(), 2);   // the two directories stay
  Model.SetFilterMask(QString());
  Model.SetFiles(QStringLiteral("/home/other"), Files);
  CHECK_EQ(Model.MarkedCount(), 0);

  // File colors
  Model.SetFileColors(FileColorRule::LoadList(QStringLiteral("0000FF:*.log")));
  QVariant Color = Model.index(Model.RowOfName(QStringLiteral("b.log")), 0).data(Qt::ForegroundRole);
  CHECK(Color.value<QColor>() == QColor(255, 0, 0));
  CHECK(!Model.index(Model.RowOfName(QStringLiteral("noext")), 0).data(Qt::ForegroundRole).value<QColor>().isValid());

  // Inaccessible directories (remote)
  RemoteFileList Dirs;
  RemoteFile Closed = MakeFile(QStringLiteral("closed"), 0, true);
  Closed.Permissions = 0700;
  Closed.Owner = QStringLiteral("root");
  RemoteFile Mine = Closed;
  Mine.FileName = QStringLiteral("mine");
  Mine.Owner = QStringLiteral("me");
  Dirs << Closed << Mine;
  Model.SetFiles(QStringLiteral("/"), Dirs);
  Model.SetHideInaccessibleDirectories(true, QStringLiteral("me"));
  CHECK_EQ(RowNames(Model), QStringList({"mine"}));
  Model.SetHideInaccessibleDirectories(false, QStringLiteral("me"));
  CHECK_EQ(Model.rowCount(), 2);
}

static void TestLocalListing(const QString & Root)
{
  QString Dir = Root + QStringLiteral("/listing");
  QDir().mkpath(Dir + QStringLiteral("/sub"));
  WriteFile(Dir + QStringLiteral("/a.txt"), 10);
  WriteFile(Dir + QStringLiteral("/.dot"), 3);
  CHECK(::symlink("a.txt", QFile::encodeName(Dir + QStringLiteral("/link")).constData()) == 0);
  CHECK(::symlink("missing", QFile::encodeName(Dir + QStringLiteral("/broken")).constData()) == 0);
  CHECK(::symlink("sub", QFile::encodeName(Dir + QStringLiteral("/sublink")).constData()) == 0);

  QString Canonical, Error, More;
  RemoteFileList Files;
  CHECK(LocalDirectoryProvider::ReadDirectory(Dir, Canonical, Files, Error, More));
  CHECK_EQ(Canonical, QFileInfo(Dir).canonicalFilePath());
  CHECK_EQ(Files.size(), 6);
  QHash<QString, RemoteFile> ByName;
  for (const RemoteFile & File : Files)
  {
    ByName.insert(File.FileName, File);
  }
  CHECK_EQ(ByName.value(QStringLiteral("a.txt")).Size, qint64(10));
  CHECK(ByName.value(QStringLiteral(".dot")).IsHidden);
  CHECK(ByName.value(QStringLiteral("sub")).IsDirectory);
  CHECK(ByName.value(QStringLiteral("link")).IsSymLink);
  CHECK_EQ(ByName.value(QStringLiteral("link")).LinkTo, QStringLiteral("a.txt"));
  CHECK_EQ(ByName.value(QStringLiteral("link")).Size, qint64(10));
  CHECK(ByName.value(QStringLiteral("sublink")).IsDirectory);
  CHECK(ByName.value(QStringLiteral("broken")).IsSymLink);
  CHECK(!ByName.value(QStringLiteral("broken")).IsDirectory);
  CHECK(!ByName.value(QStringLiteral("a.txt")).Owner.isEmpty());
  CHECK(ByName.value(QStringLiteral("a.txt")).Modification.isValid());
  CHECK(!LocalDirectoryProvider::ReadDirectory(Dir + QStringLiteral("/nope"), Canonical, Files, Error, More));
  CHECK_EQ(More, QStringLiteral("No such file or directory."));

  // Broken link overlay (local)
  FileListModel Model(false);
  CHECK(LocalDirectoryProvider::ReadDirectory(Dir, Canonical, Files, Error, More));
  Model.SetFiles(Canonical, Files);
  CHECK(Model.Overlays(Model.RowOfName(QStringLiteral("broken"))) & FileListModel::ovBrokenLink);
  CHECK(Model.Overlays(Model.RowOfName(QStringLiteral("link"))) & FileListModel::ovLink);

  // Asynchronous load on the worker thread; a newer load cancels the older one
  LocalDirectoryProvider Provider;
  int Calls = 0;
  bool LastOk = false;
  QString LastDir;
  Provider.Load(Dir + QStringLiteral("/sub"), [&](bool, const QString &, const RemoteFileList &, const QString &, const QString &)
  {
    Calls += 100;
  });
  Provider.Load(Dir, [&](bool Ok, const QString & Directory, const RemoteFileList &, const QString &, const QString &)
  {
    Calls++;
    LastOk = Ok;
    LastDir = Directory;
  });
  CHECK(WaitFor([&Calls]() { return Calls > 0; }));
  Pump(100);
  CHECK_EQ(Calls, 1);
  CHECK(LastOk);
  CHECK_EQ(LastDir, Canonical);
}

class StallingProvider : public DirectoryProvider
{
public:
  bool IsRemote() const override { return true; }
  void Load(const QString &, LoadCallback) override { Loads++; }
  void CancelLoad() override { Cancels++; }
  QString HomeDirectory() const override { return QStringLiteral("/"); }
  int Loads = 0;
  int Cancels = 0;
};

static void TestPanel(const QString & Root)
{
  Configuration & Config = Configuration::Instance();
  Config.NortonLikeMode = nlKeyboard;
  Config.DoubleClickAction = dcaEdit;
  Config.PanelSearch = isNameStartOnly;
  QString Dir = Root + QStringLiteral("/panel");
  QDir().mkpath(Dir + QStringLiteral("/dir1"));
  WriteFile(Dir + QStringLiteral("/alpha.txt"), 100);
  WriteFile(Dir + QStringLiteral("/beta.txt"), 200);
  WriteFile(Dir + QStringLiteral("/gamma.log"), 300);
  WriteFile(Dir + QStringLiteral("/.secret"), 1);

  LocalDirectoryProvider Provider;
  FilePanel Panel(false);
  Panel.resize(700, 500);
  Panel.show();
  Panel.SetProvider(&Provider);
  Panel.ChangeDirectory(Dir);
  QTreeView * View = Panel.View();
  FileListModel * Model = qobject_cast<FileListModel *>(View->model());
  CHECK(Model != nullptr);
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.CurrentDirectory() == QFileInfo(Dir).canonicalFilePath()); }));
  // "..", dir1, alpha.txt, beta.txt, gamma.log
  CHECK_EQ(Model->rowCount(), 5);
  CHECK_EQ(Model->HiddenCount(), 1);
  View->setFocus();

  // Insert on ".." does not mark it but moves down
  CHECK_EQ(View->currentIndex().row(), 0);
  Key(View, Qt::Key_Insert);
  CHECK_EQ(Panel.MarkedCount(), 0);
  CHECK_EQ(View->currentIndex().row(), 1);
  // Insert marks dir1 and moves to alpha.txt; arrows keep the marks
  Key(View, Qt::Key_Insert);
  CHECK_EQ(Panel.MarkedCount(), 1);
  CHECK_EQ(View->currentIndex().row(), 2);
  Key(View, Qt::Key_Down);
  Key(View, Qt::Key_Up);
  Key(View, Qt::Key_End);
  Key(View, Qt::Key_Home);
  CHECK_EQ(Panel.MarkedCount(), 1);
  CHECK_EQ(View->currentIndex().row(), 0);
  // Space toggles the focused file
  Key(View, Qt::Key_Down);
  Key(View, Qt::Key_Down);
  Key(View, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
  CHECK_EQ(Panel.MarkedCount(), 2);
  Key(View, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
  CHECK_EQ(Panel.MarkedCount(), 1);
  // Shift+Down extends
  Key(View, Qt::Key_Down, Qt::ShiftModifier);
  CHECK_EQ(Panel.MarkedCount(), 3);   // dir1 + alpha + beta
  CHECK_EQ(Panel.SelectedPaths().size(), 3);
  Panel.ClearSelection();
  CHECK_EQ(Panel.MarkedCount(), 0);
  // Nothing marked: the focused file is the operation target
  CHECK_EQ(Panel.SelectedPaths(), QStringList({Panel.FullPath(QStringLiteral("beta.txt"))}));
  // Left/Right go to the first/last item
  Key(View, Qt::Key_Right);
  CHECK_EQ(View->currentIndex().row(), 4);
  Key(View, Qt::Key_Left);
  CHECK_EQ(View->currentIndex().row(), 0);

  // Mouse click selects implicitly ("Mouse only"); an arrow key drops it
  Click(View, 3);
  CHECK_EQ(Panel.MarkedCount(), 1);
  CHECK_EQ(View->currentIndex().row(), 3);
  Key(View, Qt::Key_Down);
  CHECK_EQ(Panel.MarkedCount(), 0);
  // Ctrl+click marks explicitly; arrows keep it
  Click(View, 2, Qt::ControlModifier);
  Click(View, 3, Qt::ControlModifier);
  CHECK_EQ(Panel.MarkedCount(), 2);
  Key(View, Qt::Key_Down);
  CHECK_EQ(Panel.MarkedCount(), 2);
  // Clicking ".." never marks it
  Click(View, 0);
  CHECK_EQ(Panel.MarkedCount(), 0);
  Panel.ClearSelection();

  // Never explorer-style (nlOn): a click only moves the cursor
  Config.NortonLikeMode = nlOn;
  Panel.ApplyConfiguration();
  Click(View, 2);
  CHECK_EQ(Panel.MarkedCount(), 0);
  CHECK_EQ(View->currentIndex().row(), 2);
  // Explorer-style keyboard (nlOff): arrows select, nothing marked = no target
  Config.NortonLikeMode = nlOff;
  Panel.ApplyConfiguration();
  Key(View, Qt::Key_Down);
  CHECK_EQ(Panel.MarkedNames(), QStringList({QStringLiteral("beta.txt")}));
  Panel.ClearSelection();
  CHECK(Panel.SelectedPaths().isEmpty());
  Config.NortonLikeMode = nlKeyboard;
  Panel.ApplyConfiguration();

  // UnmarkFile / Restore Selection
  Panel.SelectAll();
  CHECK_EQ(Panel.MarkedCount(), 4);
  Panel.UnmarkFile(Panel.FullPath(QStringLiteral("alpha.txt")));
  CHECK_EQ(Panel.MarkedCount(), 3);
  Panel.ClearOperationSelection();
  CHECK_EQ(Panel.MarkedCount(), 0);
  CHECK(Panel.SelectedNamesSaved());
  Panel.RestoreSelectedNames();
  CHECK_EQ(Panel.MarkedCount(), 4);
  Panel.ClearSelection();
  Panel.SelectByMask(QStringLiteral("*.txt"), true);
  CHECK_EQ(Panel.MarkedCount(), 2);
  Panel.FocusFile(QStringLiteral("gamma.log"));
  Panel.SelectSameExtension(true);
  CHECK_EQ(Panel.MarkedCount(), 3);
  Panel.ClearSelection();

  // Incremental search
  Key(View, Qt::Key_Home);
  Key(View, Qt::Key_G, Qt::NoModifier, QStringLiteral("g"));
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("gamma.log"));
  Key(View, Qt::Key_Escape);
  Key(View, Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("beta.txt"));
  Key(View, Qt::Key_Escape);

  // Incremental search in any part of the name, Tab goes to the next match
  Config.PanelSearch = isName;
  Panel.ApplyConfiguration();
  Key(View, Qt::Key_Home);
  Key(View, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("alpha.txt"));
  Key(View, Qt::Key_Tab);
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("beta.txt"));
  Key(View, Qt::Key_Backtab, Qt::ShiftModifier);
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("alpha.txt"));
  Key(View, Qt::Key_Escape);
  Config.PanelSearch = isNameStartOnly;
  Panel.ApplyConfiguration();

  // Click-to-rename (slow second click) selects the base name (RenameWholeName off)
  Panel.activateWindow();
  View->setFocus();
  if (WaitFor([View]() { return View->hasFocus(); }, 1000))
  {
    Pump(800);
    int Row = Model->RowOfName(QStringLiteral("alpha.txt"));
    Click(View, Row);
    Pump(QApplication::doubleClickInterval() + 100);
    Click(View, Row);
    CHECK(WaitFor([]() { return qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr; }, 2000));
    if (QLineEdit * Edit = qobject_cast<QLineEdit *>(QApplication::focusWidget()))
    {
      Pump(50);
      CHECK_EQ(Edit->selectedText(), QStringLiteral("alpha"));
      Key(Edit, Qt::Key_Escape);
    }
    View->setFocus();
    Panel.ClearSelection();
  }
  else
  {
    std::printf("click-to-rename: SKIPPED (no keyboard focus on this platform)\n");
  }

  // Operation on double-click / Enter
  int Edits = 0, Opens = 0, Copies = 0, Menus = 0, MenusOnItem = 0, Properties = 0;
  QObject::connect(&Panel, &FilePanel::EditRequested, [&Edits](const RemoteFile &, const QString &) { Edits++; });
  QObject::connect(&Panel, &FilePanel::OpenFileRequested, [&Opens](const RemoteFile &, const QString &) { Opens++; });
  QObject::connect(&Panel, &FilePanel::CopyRequested, [&Copies](const RemoteFile &, const QString &, bool) { Copies++; });
  QObject::connect(&Panel, &FilePanel::PropertiesRequested, [&Properties]() { Properties++; });
  QObject::connect(&Panel, &FilePanel::ContextMenuRequested, [&Menus, &MenusOnItem](const QPoint &, bool OnItem)
  {
    Menus++;
    if (OnItem) MenusOnItem++;
  });
  Panel.FocusFile(QStringLiteral("alpha.txt"));
  Key(View, Qt::Key_Return);
  CHECK_EQ(Edits, 1);
  Config.DoubleClickAction = dcaOpen;
  Panel.ApplyConfiguration();
  DoubleClick(View, Model->RowOfName(QStringLiteral("beta.txt")));
  CHECK_EQ(Opens, 1);
  Config.DoubleClickAction = dcaCopy;
  Panel.ApplyConfiguration();
  Key(View, Qt::Key_Return);
  CHECK_EQ(Copies, 1);
  Config.DoubleClickAction = dcaEdit;
  Panel.ApplyConfiguration();
  Key(View, Qt::Key_Return, Qt::AltModifier);
  CHECK_EQ(Properties, 1);
  // Context menu hook: Menu key and Shift+F10
  Key(View, Qt::Key_Menu);
  Key(View, Qt::Key_F10, Qt::ShiftModifier);
  CHECK_EQ(Menus, 2);
  CHECK_EQ(MenusOnItem, 2);
  Key(View, Qt::Key_Home);
  Key(View, Qt::Key_Menu);
  CHECK_EQ(MenusOnItem, 2);   // ".." is not an item

  // Hidden files: the listing has them, the panel only filters
  Panel.SetShowHiddenFiles(true);
  CHECK_EQ(Model->rowCount(), 6);
  Panel.SetShowHiddenFiles(false);
  CHECK_EQ(Model->rowCount(), 5);

  // Sort API
  Panel.SetSort(FileListModel::ColSize, false);
  CHECK_EQ(Model->File(2).FileName, QStringLiteral("gamma.log"));
  Panel.SortBy(FileListModel::ColSize);
  CHECK(Panel.SortAscending());
  Panel.SortBy(FileListModel::ColName);
  CHECK(Panel.SortAscending());
  CHECK_EQ(Panel.SortColumn(), static_cast<int>(FileListModel::ColName));

  // Enter a directory and come back: Backspace focuses the directory we came from
  Panel.FocusFile(QStringLiteral("dir1"));
  Key(View, Qt::Key_Return);
  CHECK(WaitFor([&]() { return Panel.CurrentDirectory().endsWith(QStringLiteral("/dir1")) && !Panel.IsLoading(); }));
  Key(View, Qt::Key_Backspace);
  CHECK(WaitFor([&]() { return !Panel.CurrentDirectory().endsWith(QStringLiteral("/dir1")) && !Panel.IsLoading(); }));
  CHECK_EQ(Model->File(View->currentIndex().row()).FileName, QStringLiteral("dir1"));
  CHECK_EQ(Panel.BackCount(), 2);
  CHECK_EQ(Panel.HistoryPath(-1), Panel.CurrentDirectory() + QStringLiteral("/dir1"));
  Panel.HistoryGo(-2);
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.ForwardCount() == 2); }));
  CHECK_EQ(Panel.BackCount(), 0);

  // Automatic refresh after an external change (inotify), keeping the marks
  Panel.SelectFiles({QStringLiteral("alpha.txt")});
  WriteFile(Dir + QStringLiteral("/delta.txt"), 5);
  CHECK(WaitFor([&]() { return Model->RowOfName(QStringLiteral("delta.txt")) >= 0; }, 5000));
  CHECK_EQ(Panel.MarkedNames(), QStringList({QStringLiteral("alpha.txt")}));
  QFile::remove(Dir + QStringLiteral("/delta.txt"));
  CHECK(WaitFor([&]() { return Model->RowOfName(QStringLiteral("delta.txt")) < 0; }, 5000));

  // The watched directory is removed and its parent cannot be read: the panel
  // climbs on to the grandparent instead of retrying the parent forever
  if (::geteuid() != 0)
  {
    QString Top = Root + QStringLiteral("/climb");
    QString Locked = Top + QStringLiteral("/locked");
    QString Leaf = Locked + QStringLiteral("/leaf");
    QDir().mkpath(Leaf);
    LocalDirectoryProvider ClimbProvider;
    FilePanel Climb(false);
    Climb.show();
    Climb.SetProvider(&ClimbProvider);
    Climb.ChangeDirectory(Leaf);
    CHECK(WaitFor([&]() { return !Climb.IsLoading() && (Climb.CurrentDirectory() == QFileInfo(Leaf).canonicalFilePath()); }));
    QFile::setPermissions(Locked, QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QDir().rmdir(Leaf);
#ifdef Q_OS_MACOS
    // FSEvents may not report the removal while the parent is unreadable:
    // then the next refresh (Ctrl+R) climbs
    if (!WaitFor([&]() { return Climb.CurrentDirectory() == QFileInfo(Top).canonicalFilePath(); }, 3000))
    {
      std::printf("INFO climb: no change noticed, in '%s' (loading %d); refreshing\n",
        qPrintable(Climb.CurrentDirectory()), Climb.IsLoading() ? 1 : 0);
      Climb.Refresh();
      WaitFor([&]() { return !Climb.IsLoading(); }, 5000);
      std::printf("INFO climb: after refresh in '%s'\n", qPrintable(Climb.CurrentDirectory()));
    }
#endif
    CHECK(WaitFor([&]() { return !Climb.IsLoading() && (Climb.CurrentDirectory() == QFileInfo(Top).canonicalFilePath()); }, 5000));
    Pump(300);
    CHECK(!Climb.IsLoading());
    QFile::setPermissions(Locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }

  // Columns: show Ext, persisted in WinSCP's DirViewParams key
  Panel.SetColumnVisible(FileListModel::ColExt, true);
  CHECK(Panel.IsColumnVisible(FileListModel::ColExt));
  QString Params = Panel.DirViewParams();
  CHECK(Params.startsWith(QStringLiteral("0;1|")));
  CHECK(Params.contains(QStringLiteral(";@96|")));
  Pump(500);
  {
    IniStorage Storage(&Config.Ini());
    CHECK(Storage.OpenSubKey(QStringLiteral("Configuration"), false) && Storage.OpenSubKey(QStringLiteral("Interface"), false) &&
      Storage.OpenSubKey(QStringLiteral("Commander"), false) && Storage.OpenSubKey(QStringLiteral("LocalPanel"), false));
    CHECK_EQ(Storage.ReadString(QStringLiteral("DirViewParams"), QString()), Params);
  }
  FilePanel Second(false);
  CHECK(Second.IsColumnVisible(FileListModel::ColExt));
  CHECK_EQ(Second.DirViewParams(), Params);
  Panel.ResetColumnLayout();
  CHECK(!Panel.IsColumnVisible(FileListModel::ColExt));
  CHECK(Panel.IsColumnVisible(FileListModel::ColType));
  CHECK_EQ(Panel.DirViewParams(), QStringLiteral("0;1|150,1;70,1;120,1;150,1;55,0;55,0;62,0,6;55,0,7;150,0,8;@96|5;0;1;2;3;4"));
  // A WinSCP remote string: Type visible and moved first, sorted by size descending
  FilePanel Remote(true);
  Remote.SetDirViewParams(QStringLiteral("1;0|150,1;70,1;150,1;79,1;62,1;55,0;20,0;150,0;125,1;@192|8;0;1;2;3;4;5;6;7"));
  CHECK(Remote.IsColumnVisible(FileListModel::ColType));
  CHECK_EQ(Remote.View()->header()->visualIndex(FileListModel::ColType), 0);
  CHECK_EQ(Remote.View()->header()->sectionSize(FileListModel::ColName), 75);
  CHECK_EQ(Remote.SortColumn(), static_cast<int>(FileListModel::ColSize));
  CHECK(!Remote.SortAscending());
  Remote.SetStatusBarVisible(false);
  CHECK(!Remote.IsStatusBarVisible());
  Remote.SetStatusBarVisible(true);

  // Screenshot for the offscreen GUI check
  QString Shot = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
  if (!Shot.isEmpty())
  {
    Panel.SetActive(true);
    Panel.SelectFiles({QStringLiteral("alpha.txt"), QStringLiteral("gamma.log")});
    Panel.FocusFile(QStringLiteral("beta.txt"));
    View->setFocus();
    Pump(100);
    Panel.grab().save(Shot);
  }

  // The provider may die before the panel (tab closing deletes the page later)
  {
    auto * Doomed = new LocalDirectoryProvider();
    FilePanel Orphan(false);
    Orphan.show();
    Orphan.SetProvider(Doomed);
    Orphan.ChangeDirectory(Dir);
    CHECK(WaitFor([&]() { return !Orphan.IsLoading() && !Orphan.CurrentDirectory().isEmpty(); }));
    delete Doomed;
    WriteFile(Dir + QStringLiteral("/epsilon.txt"), 1);
    Pump(800);
    Orphan.Refresh();
    Orphan.HomeDirectory();
    Orphan.Back();
    Orphan.SetShowHiddenFiles(true);
    CHECK(!Orphan.IsLoading());
    QFile::remove(Dir + QStringLiteral("/epsilon.txt"));
  }

  // Reading indicator and cancel (provider that never answers)
  StallingProvider Stalling;
  FilePanel Slow(true);
  Slow.resize(400, 300);
  Slow.show();
  int LoadingChanges = 0;
  QObject::connect(&Slow, &FilePanel::LoadingChanged, [&LoadingChanges](bool) { LoadingChanges++; });
  Slow.SetProvider(&Stalling);
  Slow.ChangeDirectory(QStringLiteral("/slow"));
  CHECK(Slow.IsLoading());
  Pump(700);
  CHECK(Slow.IsLoading());
  Key(Slow.View(), Qt::Key_Escape);
  CHECK(!Slow.IsLoading());
  CHECK(Stalling.Cancels >= 1);
  CHECK(LoadingChanges >= 2);
}

// Remote panel over SFTP against the test sshd (skipped unless NIXSCP_TEST_SSH_PORT
// and NIXSCP_TEST_SSH_KEY are set). The server runs on this machine as the same
// user, so local scratch directories are browsed remotely.
class AcceptingUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override { return qaCancel; }
};

static void TestRemotePanel(const QString & Root)
{
  QByteArray Port = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (Port.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("remote panel: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  QString Dir = QFileInfo(Root).canonicalFilePath() + QStringLiteral("/remote");
  QDir().mkpath(Dir + QStringLiteral("/sub"));
  WriteFile(Dir + QStringLiteral("/one.txt"), 1500);
  WriteFile(Dir + QStringLiteral("/.hidden"), 1);

  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = Port.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Root + QStringLiteral("/known_hosts");
  Data.RemoteDirectory = Dir;
  Data.UpdateDirectories = false;
  AcceptingUserInterface UI;
  Terminal Term(Data, &UI);
  bool Opened = false;
  bool OpenDone = false;
  Term.Open([&](const OperationResult & Result) { Opened = Result.Ok; OpenDone = true; });
  CHECK(WaitFor([&OpenDone]() { return OpenDone; }, 20000));
  CHECK(Opened);
  if (!Opened)
  {
    return;
  }
  RemoteDirectoryProvider Provider(&Term);
  CHECK_EQ(Provider.UserName(), Data.UserName);
  Configuration & Config = Configuration::Instance();
  FilePanel Panel(true);
  Panel.resize(600, 400);
  Panel.show();
  Panel.SetProvider(&Provider);
  Panel.ChangeDirectory(Term.CurrentDirectory());
  FileListModel * Model = qobject_cast<FileListModel *>(Panel.View()->model());
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.CurrentDirectory() == Dir); }, 10000));
  CHECK(Model->RowOfName(QStringLiteral("one.txt")) >= 0);
  CHECK(Model->RowOfName(QStringLiteral("sub")) >= 0);
  CHECK_EQ(Model->HiddenCount(), 1);
  CHECK_EQ(Model->index(Model->RowOfName(QStringLiteral("one.txt")), FileListModel::ColOwner).data().toString(), Data.UserName);
  CHECK_EQ(Model->index(Model->RowOfName(QStringLiteral("one.txt")), FileListModel::ColExt).data().toString(), QStringLiteral(".txt"));

  // Home Directory: the site directory with "Default directory is home directory"
  Panel.ChangeDirectory(Dir + QStringLiteral("/sub"));
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && Panel.CurrentDirectory().endsWith(QStringLiteral("/sub")); }, 10000));
  Config.DefaultDirIsHome = true;
  Panel.HomeDirectory();
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.CurrentDirectory() == Dir); }, 10000));
  // ... and the real home directory without it
  Config.DefaultDirIsHome = false;
  Panel.HomeDirectory();
  QString Home = QFileInfo(QDir::homePath()).canonicalFilePath();
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.CurrentDirectory() == Home); }, 10000));
  Config.DefaultDirIsHome = true;

  // Refresh remote panel every N seconds
  Panel.ChangeDirectory(Dir);
  CHECK(WaitFor([&]() { return !Panel.IsLoading() && (Panel.CurrentDirectory() == Dir); }, 10000));
  Config.RefreshRemotePanel = true;
  Config.RefreshRemotePanelInterval = 1;
  Panel.ApplyConfiguration();
  WriteFile(Dir + QStringLiteral("/two.txt"), 1);
  CHECK(WaitFor([&]() { return Model->RowOfName(QStringLiteral("two.txt")) >= 0; }, 6000));
  Config.RefreshRemotePanel = false;
  Panel.ApplyConfiguration();

  Panel.SetProvider(nullptr);
  Term.Close();
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QTemporaryDir ConfigDir;
  qputenv("XDG_CONFIG_HOME", QFile::encodeName(ConfigDir.path()));
  QStandardPaths::setTestModeEnabled(true);
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
  QApplication App(argc, argv);
  Configuration::Instance().SetIniFileName(ConfigDir.path() + QStringLiteral("/nixscp.ini"));

  QTemporaryDir Root;
  TestFormats();
  TestFileColors();
  TestModel();
  TestLocalListing(Root.path());
  TestPanel(Root.path());
  TestRemotePanel(Root.path());

  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
