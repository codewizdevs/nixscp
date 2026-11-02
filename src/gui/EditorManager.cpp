// Port of WinSCP source/windows/EditorManager.cpp and the edit/open parts of
// forms/CustomScpExplorer.cpp, forms/NonVisual.cpp, forms/EditorPreferences.cpp
// and windows/Setup.cpp (see EditorManager.h)
#include "gui/EditorManager.h"
#include "gui/HelpSystem.h"

#include "core/CopyParam.h"
#include "core/FileMasks.h"
#include "core/Queue.h"
#include "core/IniStorage.h"
#include "core/RemoteFile.h"
#include "core/SessionData.h"
#include "gui/EditorWindow.h"
#include "gui/GuiUserInterface.h"
#include "gui/PreferencesDialog.h"
#include "gui/ProgressDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#else
#include <sys/syscall.h>
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#endif

#include "core/LocalFileSystem.h"
#include "core/Platform.h"

static const QString TemporaryFolderPrefix = QStringLiteral("nixscp-");
static const int KeepUpToDateChangeDelay = 500;   // TGUIConfiguration::KeepUpToDateChangeDelay (ms)
static const int UploadRetryDelay = 1000;

//---------------------------------------------------------------------------
// TEditedFileData + TEditorManager::TFileData
struct EditorManager::EditedFile
{
  quint64 Id = 0;
  QString FileName;              // local (temporary) file
  QString LocalRootDirectory;    // unique folder deleted with the file (empty = deterministic path)
  std::unique_ptr<QLockFile> Lock;
  QString RemoteDirectory;
  QString OriginalFileName;
  QString SessionName;
  SessionData Data;              // site of the session (re-attach)
  QPointer<Terminal> Term;
  QString Command;               // external editor command (early close)
  bool ForceText = false;
  bool External = false;
  bool NewFile = false;          // ExecuteNewRemoteFile: not on the server yet
  QPointer<EditorWindow> Editor; // internal editor
  qint64 Pid = 0;                // tracked external editor process (SDI)
#ifdef Q_OS_WIN
  HANDLE PidHandle = nullptr;    // signalled when the process exits
  QWinEventNotifier * PidNotifier = nullptr;
#else
  int PidFd = -1;                // pidfd: readable when the process exits
  QSocketNotifier * PidNotifier = nullptr;
#endif
  QTimer * PidPoll = nullptr;
  QElapsedTimer Opened;
  // local file state last uploaded/downloaded (mtime in ns, size)
  qint64 StampTime = 0;
  qint64 StampSize = -1;
  QDateTime SourceTimestamp;     // remote modification (EditorCheckNotModified)
  bool Closed = false;           // editor closed, upload still running
  bool Uploading = false;
  bool Reupload = false;
  bool Reloading = false;
  bool RetryPending = false;
  bool Released = false;

  bool HasProcess() const { return Pid > 0; }
  QString RemotePath() const { return UnixPath::Combine(RemoteDirectory, OriginalFileName); }
};

//---------------------------------------------------------------------------
// Editor\MaxEditors, Editor\EarlyClose, Editor\SDIShellEditor (WinSCP keys
// Configuration does not model; read as stored)
static int EditorConfigInteger(const QString & Name, int Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), false) &&
      Storage.OpenSubKey(QStringLiteral("Editor"), false))
  {
    return Storage.ReadInteger(Name, Default);
  }
  return Default;
}

static bool SameSite(const SessionData & A, const SessionData & B)
{
  // TSessionData::IsSameSite
  return (A.FSProtocol == B.FSProtocol) &&
    (A.HostNameExpanded().compare(B.HostNameExpanded(), Qt::CaseInsensitive) == 0) &&
    (A.PortNumber == B.PortNumber) &&
    (A.UserNameExpanded() == B.UserNameExpanded());
}

// Local name for a remote file/directory name component
static QString ValidLocalName(const QString & Name)
{
  QString Result = Name;
  Result.replace(QLatin1Char('/'), QLatin1Char('_'));
  Result.remove(QChar(0));
  // Windows: no ":", "?", ... nor reserved names (no change on Linux)
  Result = CopyParam::ValidLocalFileName(Result);
  if (Result.isEmpty() || (Result == QStringLiteral(".")) || (Result == QStringLiteral("..")))
  {
    Result = QStringLiteral("_") + Result;
  }
  return Result;
}

static bool IsProcessAlive(qint64 Pid)
{
#ifdef Q_OS_WIN
  if (Pid <= 0)
  {
    return false;
  }
  HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(Pid));
  if (Process == nullptr)
  {
    // access denied: it exists (of another user)
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD Code = 0;
  bool Alive = GetExitCodeProcess(Process, &Code) && (Code == STILL_ACTIVE);
  CloseHandle(Process);
  return Alive;
#else
  return (Pid > 0) && ((::kill(static_cast<pid_t>(Pid), 0) == 0) || (errno == EPERM));
#endif
}

//---------------------------------------------------------------------------
// "Editor" dialog in ad-hoc mode (TEditorPreferencesDialog, epmAdHoc)
//---------------------------------------------------------------------------
namespace
{

class EditWithDialog : public QDialog
{
public:
  EditWithDialog(QWidget * Parent, bool MayRemote) : QDialog(Parent)
  {
    setWindowTitle(QStringLiteral("Editor"));   // EDITOR_AD_HOC
    setObjectName(QStringLiteral("EditorPreferencesDialog"));
    auto * Layout = new QVBoxLayout(this);

    auto * EditorGroup = new QGroupBox(QStringLiteral("Editor"));
    auto * EG = new QVBoxLayout(EditorGroup);
    ExternalEditorEdit = new QLineEdit();
    ExternalEditorEdit->setObjectName(QStringLiteral("ExternalEditorEdit"));
    auto * Label = new QLabel(QStringLiteral("&External editor:"));
    Label->setBuddy(ExternalEditorEdit);
    EG->addWidget(Label);
    auto * Row = new QHBoxLayout();
    Row->addWidget(ExternalEditorEdit, 1);
    auto * BrowseButton = new QPushButton(QStringLiteral("B&rowse..."));
    Row->addWidget(BrowseButton);
    EG->addLayout(Row);
    auto * DefaultButton = new QPushButton(QStringLiteral("Use default system editor"));
    DefaultButton->setObjectName(QStringLiteral("DefaultButton"));
    auto * DefaultRow = new QHBoxLayout();
    DefaultRow->addWidget(DefaultButton);
    DefaultRow->addStretch(1);
    EG->addLayout(DefaultRow);
    Layout->addWidget(EditorGroup);

    auto * ExternalGroup = new QGroupBox(QStringLiteral("External editor options (affects editing remote files only)"));
    auto * XG = new QVBoxLayout(ExternalGroup);
    TextCheck = new QCheckBox(QStringLiteral("Force &text transfer mode for files edited in external editor"));
    SDICheck = new QCheckBox(QStringLiteral("E&xternal editor opens each file in separate window (process)"));
    XG->addWidget(TextCheck);
    XG->addWidget(SDICheck);
    ExternalGroup->setEnabled(MayRemote);
    Layout->addWidget(ExternalGroup);

    RememberCheck = new QCheckBox(QStringLiteral("&Remember this editor"));
    RememberCheck->setObjectName(QStringLiteral("RememberCheck"));
    Layout->addWidget(RememberCheck);

    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help);
    OkButton = Buttons->button(QDialogButtonBox::Ok);
    connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    HelpSystem::InstallFormHelp(this, QStringLiteral("ui_editor_preferences"), Buttons->button(QDialogButtonBox::Help));
    Layout->addWidget(Buttons);

    connect(BrowseButton, &QPushButton::clicked, this, [this]()
    {
      QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Select editor"), QStringLiteral("/usr/bin"));
      if (!File.isEmpty())
      {
        ExternalEditorEdit->setText((File.contains(QLatin1Char(' ')) ? (QLatin1Char('"') + File + QLatin1Char('"')) : File) +
          QStringLiteral(" !.!"));
      }
    });
    connect(DefaultButton, &QPushButton::clicked, this, [this]()
    {
      QString Command;
      QString Description;
      if (EditorManager::DetectSystemExternalEditor(Command, Description))
      {
        ExternalEditorEdit->setText(Command);
      }
    });
    connect(ExternalEditorEdit, &QLineEdit::textChanged, this, [this]() { UpdateControls(); });
    layout()->setSizeConstraint(QLayout::SetFixedSize);
    UpdateControls();
  }

  void UpdateControls()
  {
    OkButton->setEnabled(!ExternalEditorEdit->text().trimmed().isEmpty());
  }

  QLineEdit * ExternalEditorEdit;
  QCheckBox * TextCheck;
  QCheckBox * SDICheck;
  QCheckBox * RememberCheck;
  QPushButton * OkButton;
};

} // namespace

// ReformatFileNameCommand: the command gets the "!.!" pattern
static QString ReformatFileNameCommand(const QString & Command)
{
  QString Result = Command.trimmed();
  if (!Result.isEmpty() && !Result.contains(QStringLiteral("!.!")) && !Result.contains(QStringLiteral("%1")))
  {
    Result += QStringLiteral(" !.!");
  }
  return Result;
}

//---------------------------------------------------------------------------
EditorManager::EditorManager(QWidget * Window) :
  QObject(Window),
  FWindow(Window)
{
  FWatcher = new QFileSystemWatcher(this);
  FCheckTimer = new QTimer(this);
  FCheckTimer->setInterval(2000);
  // Watch notifications start a short delayed check (the editor may still
  // be writing the file); the timer also catches saves the watch missed
  connect(FWatcher, &QFileSystemWatcher::fileChanged, this, [this]() { QTimer::singleShot(KeepUpToDateChangeDelay, this, &EditorManager::Check); });
  connect(FWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() { QTimer::singleShot(KeepUpToDateChangeDelay, this, &EditorManager::Check); });
  connect(FCheckTimer, &QTimer::timeout, this, &EditorManager::Check);

  // forms/NonVisual.dfm
  FEditAction = new QAction(QStringLiteral("&Edit"), this);
  FEditAction->setStatusTip(QStringLiteral("Edit selected file(s)"));
  FEditAction->setToolTip(QStringLiteral("Edit"));
  FEditAction->setShortcut(QKeySequence(Qt::Key_F4));
  FEditAction->setIcon(QIcon::fromTheme(QStringLiteral("document-edit")));
  FEditInternalAction = new QAction(QStringLiteral("&Internal editor"), this);
  FEditInternalAction->setStatusTip(QStringLiteral("Edit selected file(s) using internal editor"));
  FEditInternalAction->setToolTip(QStringLiteral("Edit (internal)"));
  FEditInternalAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F4));
  FEditWithAction = new QAction(QStringLiteral("Edit &With..."), this);
  FEditWithAction->setStatusTip(QStringLiteral("Edit selected file(s) using editor of your choice"));
  FEditWithAction->setToolTip(QStringLiteral("Edit With"));
  FConfigureAction = new QAction(QStringLiteral("&Configure..."), this);
  FConfigureAction->setStatusTip(QStringLiteral("Customize editors"));
  for (QAction * Action : {FEditAction, FEditInternalAction})
  {
    Action->setShortcutContext(Qt::WindowShortcut);
    Window->addAction(Action);
  }
  connect(FEditAction, &QAction::triggered, this, [this]() { ExecuteSelection(efDefaultEditor); });
  connect(FEditInternalAction, &QAction::triggered, this, [this]() { ExecuteSelection(efInternalEditor); });
  connect(FEditWithAction, &QAction::triggered, this, &EditorManager::ExecuteSelectionWith);
  connect(FConfigureAction, &QAction::triggered, this, [this]() { PreferencesDialog::Execute(FWindow, QStringLiteral("Editors")); });

  FEditorListMenu = new QMenu(QStringLiteral("&Edit"), Window);
  connect(FEditorListMenu, &QMenu::aboutToShow, this, &EditorManager::RebuildEditorListMenu);
  RebuildEditorListMenu();
}

EditorManager::~EditorManager()
{
  // Files still open (exit with PENDING_EDITORS ignored): their folders are kept
  for (EditedFile * File : FFiles)
  {
    if (File->Editor != nullptr)
    {
      File->Editor->disconnect(this);
    }
    CloseProcess(File);
    if (File->Lock)
    {
      File->Lock->unlock();
    }
    delete File;
  }
  FFiles.clear();
}

void EditorManager::SetHooks(const Hooks & AHooks)
{
  FHooks = AHooks;
  UpdateActions();
}

void EditorManager::ShowStatus(const QString & Message, int Timeout)
{
  if (FHooks.ShowStatus)
  {
    FHooks.ShowStatus(Message, Timeout);
  }
}

bool EditorManager::TerminalAvailable(Terminal * Term) const
{
  // IsAvailableTerminal
  if ((Term == nullptr) || !Term->Active())
  {
    return false;
  }
  if (FHooks.Terminals)
  {
    return FHooks.Terminals().contains(Term);
  }
  return true;
}

bool EditorManager::TerminalIdle(Terminal * Term) const
{
  return (Term != nullptr) && !Term->Busy() && (!FHooks.CanStartOperation || FHooks.CanStartOperation(Term));
}

void EditorManager::RunOperation(Terminal * Term, const std::function<void(Terminal::Callback)> & Start,
  const Terminal::Callback & Done, bool ShowProgress)
{
  QPointer<Terminal> Guard(Term);
  if (FHooks.SetOperationRunning)
  {
    FHooks.SetOperationRunning(Term, true);
  }
  ProgressDialog * Progress = nullptr;
  if (ShowProgress && (FWindow != nullptr))
  {
    Progress = new ProgressDialog(Term->Progress(), FWindow);
    Progress->Start();
  }
  QPointer<EditorManager> Self(this);
  Start([Self, Guard, Progress, Done](const OperationResult & Result)
  {
    if (Progress != nullptr)
    {
      Progress->Finish();
      Progress->deleteLater();
    }
    if (Self.isNull())
    {
      return;
    }
    if (!Guard.isNull() && Self->FHooks.SetOperationRunning)
    {
      Self->FHooks.SetOperationRunning(Guard, false);
    }
    Done(Result);
  });
}

//---------------------------------------------------------------------------
// Actions and the editor list menu
//---------------------------------------------------------------------------
void EditorManager::UpdateActions()
{
  bool Enabled = true;
  if (FHooks.CurrentSelection)
  {
    Selection S = FHooks.CurrentSelection();
    Enabled = !S.Paths.isEmpty() && ((S.Term == nullptr) || S.Term->Active());
  }
  bool Disabled = Configuration::Instance().DisableOpenEdit();
  FEditAction->setEnabled(Enabled && !Disabled);
  FEditInternalAction->setEnabled(Enabled && !Disabled);
  FEditWithAction->setEnabled(Enabled && !Disabled);
}

void EditorManager::RebuildEditorListMenu()
{
  // TNonVisualDataModule::CreateEditorListMenu
  UpdateActions();
  FEditorListMenu->clear();
  FEditorListMenu->addAction(FEditAction);
  FEditorListMenu->addSeparator();
  FEditorListMenu->addAction(FEditInternalAction);
  FEditorListMenu->addSeparator();
  QStringList UsedEditors;
  const QList<EditorPreferences> & Editors = Configuration::Instance().Editors;
  for (int Index = 0; Index < Editors.size(); Index++)
  {
    const EditorPreferences & Editor = Editors[Index];
    if ((Editor.Editor == edExternal) && !UsedEditors.contains(Editor.ExternalEditor))
    {
      UsedEditors.append(Editor.ExternalEditor);
      QAction * Item = FEditorListMenu->addAction(Editor.Name());
      // EXTERNAL_EDITOR_HINT
      Item->setStatusTip(QStringLiteral("Edit selected file(s) using external editor '%1'").arg(Editor.Name()));
      Item->setEnabled(FEditAction->isEnabled());
      EditorPreferences Copy = Editor;
      connect(Item, &QAction::triggered, this, [this, Copy]() { ExecuteSelection(efExternalEditor, &Copy); });
    }
  }
  FEditorListMenu->addAction(FEditWithAction);
  FEditorListMenu->addSeparator();
  FEditorListMenu->addAction(FConfigureAction);
}

void EditorManager::ExecuteSelection(ExecuteFileBy By, const EditorPreferences * External)
{
  if (!FHooks.CurrentSelection)
  {
    return;
  }
  Selection S = FHooks.CurrentSelection();
  if (!S.Paths.isEmpty())
  {
    ExecuteFiles(S.Term, S.Paths, By, External);
  }
}

void EditorManager::ExecuteSelectionWith()
{
  // TCustomScpExplorerForm::ExecuteCurrentFileWith
  if (!FHooks.CurrentSelection)
  {
    return;
  }
  Selection S = FHooks.CurrentSelection();
  if (S.Paths.isEmpty())
  {
    return;
  }
  EditWithDialog Dialog(FWindow, S.Term != nullptr);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return;
  }
  EditorPreferences External;
  External.Editor = edExternal;
  External.ExternalEditor = ReformatFileNameCommand(Dialog.ExternalEditorEdit->text());
  External.ExternalEditorText = Dialog.TextCheck->isChecked();
  External.SDIExternalEditor = Dialog.SDICheck->isChecked();
  if (Dialog.RememberCheck->isChecked())
  {
    Configuration & Config = Configuration::Instance();
    bool Found = false;
    for (const EditorPreferences & Editor : Config.Editors)
    {
      Found = Found || ((Editor.Editor == edExternal) && (Editor.ExternalEditor == External.ExternalEditor));
    }
    if (!Found)
    {
      Config.Editors.append(External);
      Config.Save();
    }
  }
  ExecuteFiles(S.Term, S.Paths, efExternalEditor, &External);
}

//---------------------------------------------------------------------------
// Opening files
//---------------------------------------------------------------------------
void EditorManager::EditorAutoConfig()
{
  // TCustomScpExplorerForm::EditorAutoConfig (offered once)
  Configuration & Config = Configuration::Instance();
  IniStorage Storage(&Config.Ini());
  bool Offered = Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
    Storage.OpenSubKey(QStringLiteral("Interface"), false) &&
    Storage.ReadBool(QStringLiteral("OfferedEditorAutoConfig"), false);
  if (Offered)
  {
    return;
  }
  bool IsDefaultList = (Config.Editors == Configuration::DefaultEditors());
  QString Command;
  QString Description;
  if (IsDefaultList && DetectSystemExternalEditor(Command, Description))
  {
    QString Message = QStringLiteral("**Do you want to use '%1' instead of default internal editor?**\n\n"
      "NixSCP has detected that you have custom text editor '%1' associated with text files.").arg(Description);   // EDITOR_AUTO_CONFIG2
    if (MessageDialog(FWindow, Message, qtConfirmation, qaOK | qaCancel, QStringLiteral("ui_pref_editor#auto")) == qaOK)
    {
      EditorPreferences Editor;
      Editor.Editor = edExternal;
      Editor.ExternalEditor = Command;
      Config.Editors.insert(0, Editor);
    }
  }
  IniStorage Writer(&Config.Ini());
  if (Writer.OpenSubKey(QStringLiteral("Configuration"), true) && Writer.OpenSubKey(QStringLiteral("Interface"), true))
  {
    Writer.WriteBool(QStringLiteral("OfferedEditorAutoConfig"), true);
  }
  Config.Save();
}

bool EditorManager::ExecuteFileNormalize(ExecuteFileBy & By, const EditorPreferences *& External, const QString & FileName)
{
  // TCustomScpExplorerForm::ExecuteFileNormalize
  if (By == efDefaultEditor)
  {
    EditorAutoConfig();
    const EditorPreferences * Editor = Configuration::Instance().EditorPreferencesFor(FileName);
    if ((Editor == nullptr) || (Editor->Editor == edInternal))
    {
      By = efInternalEditor;
      External = nullptr;
    }
    else if (Editor->Editor == edOpen)
    {
      By = efShell;
      External = nullptr;
    }
    else if (Editor->ExternalEditor.trimmed().isEmpty())
    {
      By = efInternalEditor;
      External = nullptr;
    }
    else
    {
      By = efExternalEditor;
      External = Editor;
    }
  }
  else if ((By == efExternalEditor) && ((External == nullptr) || External->ExternalEditor.trimmed().isEmpty()))
  {
    By = efInternalEditor;
    External = nullptr;
  }
  return true;
}

void EditorManager::ExecuteFiles(Terminal * Term, const QStringList & Paths, ExecuteFileBy By, const EditorPreferences * External)
{
  if (Configuration::Instance().DisableOpenEdit())
  {
    return;
  }
  EditorPreferences ExternalCopy;
  if (External != nullptr)
  {
    ExternalCopy = *External;
  }
  ExecuteNext(QPointer<Terminal>(Term), Paths, By, ExternalCopy, External != nullptr);
}

void EditorManager::ExecuteNext(QPointer<Terminal> Term, QStringList Paths, ExecuteFileBy By, EditorPreferences External, bool HasExternal)
{
  // one file after another (each remote file is downloaded first)
  while (!Paths.isEmpty())
  {
    QString Path = Paths.takeFirst();
    if (Term.isNull())
    {
      QFileInfo Info(Path);
      if (!Info.isDir())
      {
        ExecuteLocalFile(Path, By, HasExternal ? &External : nullptr);
      }
      continue;
    }
    if (!TerminalAvailable(Term))
    {
      return;
    }
    QPointer<EditorManager> Self(this);
    ExecuteRemoteFile(Term, Path, By, HasExternal ? &External : nullptr, [Self, Term, Paths, By, External, HasExternal]()
    {
      if (!Self.isNull() && !Term.isNull())
      {
        Self->ExecuteNext(Term, Paths, By, External, HasExternal);
      }
    });
    return;
  }
}

void EditorManager::ExecuteLocalFile(const QString & Path, ExecuteFileBy By, const EditorPreferences * External)
{
  ExecuteFileNormalize(By, External, Path);
  switch (By)
  {
    case efInternalEditor:
    {
      auto * Editor = new EditorWindow(Path, QString(), FWindow);
      if (!Editor->IsLoaded())
      {
        delete Editor;
        return;
      }
      FLocalEditors.append(Editor);
      connect(Editor, &EditorWindow::Closed, this, [this, Editor]() { FLocalEditors.removeAll(Editor); });
      Editor->show();
      break;
    }
    case efExternalEditor:
      if (!StartExternal(External->ExternalEditor, Path, nullptr))
      {
        ShowErrorMessage(FWindow, QStringLiteral("Error executing editor '%1'.").arg(External->ExternalEditor));   // EDITOR_ERROR
      }
      break;
    default:
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(Path)))
      {
        ShowErrorMessage(FWindow, QStringLiteral("Error opening file '%1'.").arg(Path));   // EXECUTE_FILE_ERROR
      }
      break;
  }
}

bool EditorManager::CanAddFile(Terminal * Term, const QString & RemoteDirectory, const QString & OriginalFileName,
  QString & ExistingLocalRootDirectory, QString & ExistingLocalDirectory, std::unique_ptr<QLockFile> & ExistingLock)
{
  // TEditorManager::CanAddFile + the handling in TCustomScpExplorerForm::ExecuteFile
  QString SessionName = Term->Data().SessionName();
  for (EditedFile * File : FFiles)
  {
    // include even "closed" (= being uploaded) files, it is nonsense to
    // download a file being uploaded
    if ((File->RemoteDirectory == RemoteDirectory) && (File->OriginalFileName == OriginalFileName) &&
        (File->SessionName == SessionName))
    {
      if (!File->External)
      {
        if (!File->Closed && (File->Editor != nullptr))
        {
          EditorWindow * Editor = File->Editor;
          if (Editor->isMinimized())
          {
            Editor->showNormal();
          }
          Editor->raise();
          Editor->activateWindow();
          return false;
        }
      }
      else if (!File->HasProcess() && !File->Closed)
      {
        // MDI editor: download the file again to where it already is
        if (QFileInfo(File->FileName).absoluteDir().exists())
        {
          ExistingLocalRootDirectory = File->LocalRootDirectory;
          ExistingLocalDirectory = QFileInfo(File->FileName).absolutePath();
        }
        // the folder (and its lock) is taken over by the new record
        ExistingLock = std::move(File->Lock);
        CloseFile(File, false);   // do not delete the file
        return true;
      }
      // ALREADY_EDITED_EXTERNALLY_OR_UPLOADED
      ShowErrorMessage(FWindow, QStringLiteral("The file '%1' is already opened in external editor (application) or is being uploaded.")
        .arg(OriginalFileName));
      return false;
    }
  }
  if (FFiles.size() >= EditorConfigInteger(QStringLiteral("MaxEditors"), 500))
  {
    ShowErrorMessage(FWindow, QStringLiteral("There are too many opened files already. Please close some first."));   // TOO_MANY_EDITORS
    return false;
  }
  return true;
}

EditorManager::EditedFile * EditorManager::FindFile(quint64 Id) const
{
  for (EditedFile * File : FFiles)
  {
    if (File->Id == Id)
    {
      return File;
    }
  }
  return nullptr;
}

bool EditorManager::TemporaryDirectoryForRemoteFiles(Terminal * Term, const QString & RemoteDirectory, QString & Directory,
  QString & RootDirectory, std::unique_ptr<QLockFile> & Lock)
{
  // TCustomScpExplorerForm::TemporaryDirectoryForRemoteFiles
  const ConfigurationData & C = Configuration::Instance();
  QString Base = TemporaryBaseDirectory();
  QString Result;
  if (!C.TemporaryDirectoryDeterministic)
  {
    RootDirectory = CreateUniqueTemporaryDirectory(Base, &Lock);
    if (RootDirectory.isEmpty())
    {
      Directory = Base;
      return false;
    }
    Result = RootDirectory;
  }
  else
  {
    RootDirectory.clear();
    Result = Base;
  }
  QStringList Parts;
  if (C.TemporaryDirectoryAppendSession)
  {
    Parts.append(ValidLocalName(Term->Data().SessionName()));
  }
  if (C.TemporaryDirectoryAppendPath)
  {
    for (const QString & Part : RemoteDirectory.split(QLatin1Char('/'), Qt::SkipEmptyParts))
    {
      Parts.append(ValidLocalName(Part));
    }
  }
  if (!QFileInfo(Result).isDir() && !QDir().mkpath(Result))
  {
    Directory = Result;
    return false;
  }
  // folders below the base are private as the unique folder is
  for (const QString & Part : Parts)
  {
    Result += QLatin1Char('/') + Part;
    if (!QFileInfo(Result).isDir())
    {
      if (!QDir().mkdir(Result))
      {
        Directory = Result;
        return false;
      }
      QFile::setPermissions(Result, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
  }
  Directory = Result;
  return true;
}

void EditorManager::ExecuteRemoteFile(Terminal * Term, const QString & RemotePath, ExecuteFileBy By, const EditorPreferences * External,
  const std::function<void()> & Next)
{
  // TCustomScpExplorerForm::ExecuteFile (remote side)
  ExecuteFileNormalize(By, External, RemotePath);
  QString OriginalFileName = UnixPath::ExtractFileName(RemotePath);
  QString RemoteDirectory = UnixPath::ExtractFileDir(RemotePath);
  QString LocalRootDirectory;
  QString LocalDirectory;
  std::unique_ptr<QLockFile> Lock;
  if (!CanAddFile(Term, RemoteDirectory, OriginalFileName, LocalRootDirectory, LocalDirectory, Lock))
  {
    Next();
    return;
  }
  if (!TerminalIdle(Term))
  {
    if (Lock)
    {
      // the folder of a reused MDI file: nothing else holds it now
      Lock->unlock();
      QDir(LocalRootDirectory).removeRecursively();
    }
    ShowStatus(QStringLiteral("Cannot open '%1' now, another operation is in progress.").arg(OriginalFileName), 5000);
    return;
  }
  if (LocalDirectory.isEmpty() &&
      !TemporaryDirectoryForRemoteFiles(Term, RemoteDirectory, LocalDirectory, LocalRootDirectory, Lock))
  {
    // CREATE_TEMP_DIR_ERROR
    ShowErrorMessage(FWindow, QStringLiteral("Cannot create temporary directory '%1'. You may change root directory to store "
      "temporary files in Preferences.").arg(LocalDirectory));
    if (!LocalRootDirectory.isEmpty())
    {
      QDir(LocalRootDirectory).removeRecursively();
    }
    Next();
    return;
  }

  auto * File = new EditedFile;
  File->Id = FNextId++;
  File->LocalRootDirectory = LocalRootDirectory;
  File->Lock = std::move(Lock);
  File->RemoteDirectory = RemoteDirectory;
  File->OriginalFileName = OriginalFileName;
  File->SessionName = Term->Data().SessionName();
  File->Data = Term->Data();
  File->Term = Term;
  // RemoteExecuteForceText
  File->ForceText = (By == efExternalEditor) && (External != nullptr) && External->ExternalEditorText;
  QString LocalName = ValidLocalName(OriginalFileName);
  File->FileName = QDir(LocalDirectory).filePath(LocalName);

  // TemporaryFileCopyParam: no masks, no renaming, no newer-only, no confirmations
  CopyParam Param = Configuration::Instance().DefaultCopyParam().TemporaryFileCopyParam(File->ForceText);
  Param.FileMask = (LocalName == OriginalFileName) ? DefaultCopyFileMask : DelimitFileNameMask(LocalName);
  Param.ConfirmOverwriting = false;
  bool CheckNotModified = Configuration::Instance().EditorCheckNotModified;
  EditorPreferences ExternalCopy;
  bool HasExternal = (External != nullptr);
  if (HasExternal)
  {
    ExternalCopy = *External;
  }
  auto Source = std::make_shared<RemoteFile>();
  QPointer<Terminal> Guard(Term);
  QPointer<EditorManager> Self(this);

  auto Download = [this, Guard, File, Param, RemotePath, LocalDirectory, By, ExternalCopy, HasExternal, Next, Source, CheckNotModified]()
  {
    RunOperation(Guard, [Guard, RemotePath, LocalDirectory, Param](Terminal::Callback Done)
      {
        Guard->CopyToLocal({RemotePath}, LocalDirectory, Param, false, Done);
      },
      [this, File, By, ExternalCopy, HasExternal, Next, Source, CheckNotModified](const OperationResult & Result)
      {
        if (!Result.Ok || !QFileInfo::exists(File->FileName))
        {
          if (!Result.Ok && !Result.Aborted && !Result.Fatal)
          {
            ShowErrorMessage(FWindow, Result.Message, Result.MoreMessages);
          }
          if (File->Lock)
          {
            File->Lock->unlock();
          }
          if (!File->LocalRootDirectory.isEmpty())
          {
            QDir(File->LocalRootDirectory).removeRecursively();
          }
          delete File;
          return;
        }
        if (CheckNotModified && Source->Modification.isValid())
        {
          File->SourceTimestamp = Source->Modification;
        }
        CustomExecuteFile(File, By, HasExternal ? &ExternalCopy : nullptr);
        Next();
      }, true);
  };

  // TryReadFile (ExecuteFile): directories are not opened; the source
  // timestamp is kept to check that the remote file did not change before saving
  RunOperation(Term, [Guard, RemotePath, Source](Terminal::Callback Done)
    {
      Guard->RunOnWorker([RemotePath, Source](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          RemoteFile Found;
          if (Fs.FileExists(RemotePath, &Found))
          {
            *Source = Found;
          }
        }, Done);
    },
    [Self, Guard, File, Download, Next, Source](const OperationResult &)
    {
      if (Self.isNull() || Guard.isNull() || Source->IsDirectory)
      {
        if (File->Lock)
        {
          File->Lock->unlock();
        }
        if (!File->LocalRootDirectory.isEmpty())
        {
          QDir(File->LocalRootDirectory).removeRecursively();
        }
        delete File;
        if (!Self.isNull() && !Guard.isNull())
        {
          Next();
        }
        return;
      }
      Download();
    }, false);
}

void EditorManager::ExecuteNewRemoteFile(Terminal * Term, const QString & RemotePath)
{
  // TCustomScpExplorerForm::EditNew (!ExistingFile): TemporaryDirectoryForRemoteFiles,
  // FileCreate, ExecuteFileNormalize, CustomExecuteFile(NewFile)
  if (Configuration::Instance().DisableOpenEdit() || !TerminalAvailable(Term))
  {
    return;
  }
  QString OriginalFileName = UnixPath::ExtractFileName(RemotePath);
  QString RemoteDirectory = UnixPath::ExtractFileDir(RemotePath);
  QString LocalRootDirectory;
  QString LocalDirectory;
  std::unique_ptr<QLockFile> Lock;
  if (!CanAddFile(Term, RemoteDirectory, OriginalFileName, LocalRootDirectory, LocalDirectory, Lock))
  {
    return;
  }
  if (LocalDirectory.isEmpty() &&
      !TemporaryDirectoryForRemoteFiles(Term, RemoteDirectory, LocalDirectory, LocalRootDirectory, Lock))
  {
    // CREATE_TEMP_DIR_ERROR
    ShowErrorMessage(FWindow, QStringLiteral("Cannot create temporary directory '%1'. You may change root directory to store "
      "temporary files in Preferences.").arg(LocalDirectory));
    if (!LocalRootDirectory.isEmpty())
    {
      QDir(LocalRootDirectory).removeRecursively();
    }
    return;
  }
  ExecuteFileBy By = efDefaultEditor;
  const EditorPreferences * External = nullptr;
  ExecuteFileNormalize(By, External, OriginalFileName);

  auto * File = new EditedFile;
  File->Id = FNextId++;
  File->LocalRootDirectory = LocalRootDirectory;
  File->Lock = std::move(Lock);
  File->RemoteDirectory = RemoteDirectory;
  File->OriginalFileName = OriginalFileName;
  File->SessionName = Term->Data().SessionName();
  File->Data = Term->Data();
  File->Term = Term;
  File->ForceText = (By == efExternalEditor) && (External != nullptr) && External->ExternalEditorText;
  File->FileName = QDir(LocalDirectory).filePath(ValidLocalName(OriginalFileName));
  File->NewFile = true;
  QFile Empty(File->FileName);
  if (!QFileInfo::exists(File->FileName) && !Empty.open(QIODevice::WriteOnly))
  {
    // CREATE_FILE_ERROR
    ShowErrorMessage(FWindow, QStringLiteral("Cannot create file '%1'.").arg(File->FileName), Empty.errorString());
    if (File->Lock)
    {
      File->Lock->unlock();
    }
    if (!File->LocalRootDirectory.isEmpty())
    {
      QDir(File->LocalRootDirectory).removeRecursively();
    }
    delete File;
    return;
  }
  Empty.close();
  EditorPreferences ExternalCopy;
  if (External != nullptr)
  {
    ExternalCopy = *External;
  }
  CustomExecuteFile(File, By, (External != nullptr) ? &ExternalCopy : nullptr);
}

static int SessionInternalEditorEncoding(const SessionData & Data)
{
  // InternalEditorEncoding (raw site setting, -1 = default)
  static const QString Prefix = QStringLiteral("InternalEditorEncoding=");
  for (const QString & Option : Data.SaveToOptions(nullptr))
  {
    if (Option.startsWith(Prefix, Qt::CaseInsensitive))
    {
      bool Ok = false;
      int Value = Option.mid(Prefix.size()).toInt(&Ok);
      return Ok ? Value : -1;
    }
  }
  return -1;
}

void EditorManager::CustomExecuteFile(EditedFile * File, ExecuteFileBy By, const EditorPreferences * External)
{
  // TCustomScpExplorerForm::CustomExecuteFile (remote file)
  quint64 Id = File->Id;
  if (By == efInternalEditor)
  {
    // caption: remote path - session name
    QString Caption = File->RemotePath() + QStringLiteral(" - ") + File->SessionName;
    auto * Editor = new EditorWindow(File->FileName, Caption, FWindow, SessionInternalEditorEncoding(File->Data));
    if (!Editor->IsLoaded())
    {
      Editor->disconnect(this);
      delete Editor;
      if (File->Lock)
      {
        File->Lock->unlock();
      }
      if (!File->LocalRootDirectory.isEmpty())
      {
        QDir(File->LocalRootDirectory).removeRecursively();
      }
      delete File;
      return;
    }
    if (File->Data.Color != 0)
    {
      Editor->SetBackgroundColor(QColor::fromRgb(File->Data.ColorRgb()));
    }
    if (File->NewFile)
    {
      Editor->SetNewFile(true);
    }
    File->Editor = Editor;
    AddFile(File);
    connect(Editor, &EditorWindow::Saved, this, [this, Id]()
    {
      // TEditorManager::FileChanged
      if (EditedFile * F = FindFile(Id))
      {
        CheckFileChange(F, true);
      }
    });
    connect(Editor, &EditorWindow::Closed, this, [this, Id]()
    {
      if (EditedFile * F = FindFile(Id))
      {
        InternalEditorClosed(F, false);
      }
    });
    Editor->SetReloadHandler([this, Id](std::function<void(bool)> Done)
    {
      EditedFile * F = FindFile(Id);
      if (F == nullptr)
      {
        Done(false);
        return;
      }
      FileReload(F, Done);
    });
    Editor->show();
    return;
  }

  QString Command;
  bool Track;
  if (By == efExternalEditor)
  {
    Command = External->ExternalEditor;
    Track = External->SDIExternalEditor;
    File->Command = External->ExternalEditor;
  }
  else
  {
#ifdef Q_OS_WIN
    // Opens the file with its associated program (and exits at once)
    Command = QStringLiteral("explorer.exe !.!");
#else
    Command = QStringLiteral("xdg-open !.!");
#endif
    Track = (EditorConfigInteger(QStringLiteral("SDIShellEditor"), 0) != 0);
  }
  QStringList Args = EditorCommandArguments(Command, File->FileName);
  // Programs that pass the file to another (running) application and exit
  // cannot be tracked (OPEN_FILE_NO_PROCESS2)
  static const QStringList Launchers = {
    QStringLiteral("xdg-open"), QStringLiteral("gio"), QStringLiteral("kde-open"), QStringLiteral("kde-open5"),
    QStringLiteral("kioclient"), QStringLiteral("kioclient5"), QStringLiteral("exo-open"), QStringLiteral("gnome-open"),
    QStringLiteral("explorer.exe"), QStringLiteral("explorer"), QStringLiteral("cmd.exe"), QStringLiteral("cmd") };
  if (!Args.isEmpty() && Launchers.contains(QFileInfo(Args.first()).fileName()) && (By == efExternalEditor))
  {
    Track = false;
  }
  File->External = true;
  AddFile(File);
  qint64 Pid = 0;
  if (!StartExternal(Command, File->FileName, &Pid))
  {
    if (By == efExternalEditor)
    {
      // EDITOR_ERROR
      ShowErrorMessage(FWindow, QStringLiteral("Error executing editor '%1'.").arg(Args.isEmpty() ? Command : Args.first()));
    }
    else
    {
      // EXECUTE_FILE_ERROR
      ShowErrorMessage(FWindow, QStringLiteral("Error executing file '%1'.").arg(File->FileName));
    }
    CloseFile(File, true);
    return;
  }
  if (Track)
  {
    File->Pid = Pid;
    TrackProcess(File);
  }
  WatchFile(File);
}

bool EditorManager::StartExternal(const QString & Command, const QString & FileName, qint64 * Pid)
{
  // ExecuteShell without a shell
  QStringList Args = EditorCommandArguments(Command, FileName);
  if (Args.isEmpty())
  {
    return false;
  }
  QProcess Process;
  Process.setProgram(Args.takeFirst());
  Process.setArguments(Args);
  Process.setStandardInputFile(QProcess::nullDevice());
  Process.setStandardOutputFile(QProcess::nullDevice());
  Process.setStandardErrorFile(QProcess::nullDevice());
  qint64 ProcessId = 0;
  bool Result = Process.startDetached(&ProcessId);
  if (Pid != nullptr)
  {
    *Pid = ProcessId;
  }
  return Result;
}

void EditorManager::TrackProcess(EditedFile * File)
{
  quint64 Id = File->Id;
#ifdef Q_OS_WIN
  HANDLE Process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(File->Pid));
  if (Process != nullptr)
  {
    // the process handle is signalled when it exits
    File->PidHandle = Process;
    File->PidNotifier = new QWinEventNotifier(Process, this);
    connect(File->PidNotifier, &QWinEventNotifier::activated, this, [this, Id]()
    {
      if (EditedFile * F = FindFile(Id))
      {
        ProcessFinished(F);
      }
    });
  }
#else
  int Fd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(File->Pid), 0));
  if (Fd >= 0)
  {
    // the pidfd becomes readable when the process exits
    File->PidFd = Fd;
    File->PidNotifier = new QSocketNotifier(Fd, QSocketNotifier::Read, this);
    connect(File->PidNotifier, &QSocketNotifier::activated, this, [this, Id]()
    {
      if (EditedFile * F = FindFile(Id))
      {
        ProcessFinished(F);
      }
    });
  }
#endif
  else if (!IsProcessAlive(File->Pid))
  {
    QTimer::singleShot(0, this, [this, Id]()
    {
      if (EditedFile * F = FindFile(Id))
      {
        ProcessFinished(F);
      }
    });
  }
  else
  {
    // kernels without pidfd: poll
    File->PidPoll = new QTimer(this);
    File->PidPoll->setInterval(500);
    connect(File->PidPoll, &QTimer::timeout, this, [this, Id]()
    {
      EditedFile * F = FindFile(Id);
      if ((F != nullptr) && !IsProcessAlive(F->Pid))
      {
        ProcessFinished(F);
      }
    });
    File->PidPoll->start();
  }
}

//---------------------------------------------------------------------------
// TEditorManager
//---------------------------------------------------------------------------
bool EditorManager::FileStamp(const QString & FileName, qint64 & Time, qint64 & Size)
{
  LocalFs::Stat St;
  if (!LocalFs::StatFollow(FileName, St))
  {
    return false;
  }
  Time = St.ModifiedSec * 1000000000LL + St.ModifiedNsec;
  Size = St.Size;
  return true;
}

void EditorManager::AddFile(EditedFile * File)
{
  // TEditorManager::AddFile
  FileStamp(File->FileName, File->StampTime, File->StampSize);
  File->Opened.start();
  FFiles.append(File);
}

void EditorManager::WatchFile(EditedFile * File)
{
  if (QFileInfo::exists(File->FileName) && !FWatcher->files().contains(File->FileName))
  {
    FWatcher->addPath(File->FileName);
  }
  // editors that save by renaming a new file over the old one
  QString Dir = QFileInfo(File->FileName).absolutePath();
  if (!FWatcher->directories().contains(Dir))
  {
    FWatcher->addPath(Dir);
  }
  if (!FCheckTimer->isActive())
  {
    FCheckTimer->start();
  }
}

void EditorManager::Check()
{
  // TEditorManager::Check (external editors; internal ones report saves)
  qint64 Now = QDateTime::currentMSecsSinceEpoch();
  bool Again = false;
  const QList<EditedFile *> Files = FFiles;
  for (EditedFile * File : Files)
  {
    if (!FFiles.contains(File) || !File->External || File->Closed)
    {
      continue;
    }
    qint64 Time = 0;
    qint64 Size = 0;
    if (!FileStamp(File->FileName, Time, Size))
    {
      continue;
    }
    if (!FWatcher->files().contains(File->FileName))
    {
      FWatcher->addPath(File->FileName);
    }
    if ((Time != File->StampTime) || (Size != File->StampSize))
    {
      // let the editor finish writing the file
      if ((Now - (Time / 1000000)) < KeepUpToDateChangeDelay)
      {
        Again = true;
      }
      else
      {
        CheckFileChange(File, false);
      }
    }
  }
  if (Again)
  {
    QTimer::singleShot(KeepUpToDateChangeDelay, this, &EditorManager::Check);
  }
}

void EditorManager::FileChanged(EditedFile * File)
{
  CheckFileChange(File, true);
}

void EditorManager::CheckFileChange(EditedFile * File, bool Force)
{
  // TEditorManager::CheckFileChange
  qint64 Time = 0;
  qint64 Size = 0;
  bool Changed = !File->Reloading && FileStamp(File->FileName, Time, Size) &&
    ((Time != File->StampTime) || (Size != File->StampSize));
  if (!Force && !Changed)
  {
    return;
  }
  if (File->Uploading)
  {
    // changed while being uploaded: upload again afterwards
    File->Reupload = true;
    return;
  }
  if (Changed)
  {
    File->StampTime = Time;
    File->StampSize = Size;
  }
  UploadFile(File);
}

bool EditorManager::ReattachSession(EditedFile * File)
{
  // TCustomScpExplorerForm::ExecutedFileChanged, inavailable session
  QString FileNameOnly = QFileInfo(File->FileName).fileName();
  Terminal * Candidate = nullptr;
  if (FHooks.Terminals)
  {
    for (Terminal * Term : FHooks.Terminals())
    {
      if ((Term != nullptr) && Term->Active() && SameSite(Term->Data(), File->Data))
      {
        Candidate = Term;
        break;
      }
    }
  }
  if (Candidate != nullptr)
  {
    // EDIT_SESSION_REATTACH
    QString Message = QStringLiteral("**Do you want to attach edited file '%1' to session '%2'?**\n\n"
      "Original session used to download file '%3' to editor was closed already.")
      .arg(FileNameOnly, Candidate->Data().SessionName(), FileNameOnly);
    if (MessageDialog(FWindow, Message, qtConfirmation, qaOK | qaCancel) == qaOK)
    {
      File->Term = Candidate;
      File->SessionName = Candidate->Data().SessionName();
      return true;
    }
    return false;
  }
  // EDIT_SESSION_CLOSED2
  ShowErrorMessage(FWindow, QStringLiteral("**Cannot upload edited file '%1'**\n\nThe session '%2' has been already closed.\n\n"
    "Open a new session on the same site and try saving file again.").arg(FileNameOnly, File->SessionName));
  return false;
}

void EditorManager::UploadFile(EditedFile * File)
{
  // TCustomScpExplorerForm::ExecutedFileChanged
  if (File->Uploading)
  {
    File->Reupload = true;
    return;
  }
  quint64 Id = File->Id;
  if (!TerminalAvailable(File->Term))
  {
    if (!ReattachSession(File))
    {
      emit FileUploaded(File->FileName, false);
      if (File->Closed)
      {
        CloseFile(File, true);
      }
      return;
    }
    File = FindFile(Id);
    if (File == nullptr)
    {
      return;
    }
  }
  if (!TerminalIdle(File->Term))
  {
    // the session is busy, retry later
    if (!File->RetryPending)
    {
      File->RetryPending = true;
      QTimer::singleShot(UploadRetryDelay, this, [this, Id]()
      {
        if (EditedFile * F = FindFile(Id))
        {
          F->RetryPending = false;
          UploadFile(F);
        }
      });
    }
    return;
  }
  File->Uploading = true;
  QPointer<Terminal> Term(File->Term);
  QString FileNameOnly = QFileInfo(File->FileName).fileName();
  QString LocalFileName = File->FileName;
  QString RemotePath = File->RemotePath();
  QString RemoteDirectory = File->RemoteDirectory;
  bool CheckNotModified = Configuration::Instance().EditorCheckNotModified && File->SourceTimestamp.isValid();

  auto Upload = [this, Id, Term, LocalFileName, RemotePath, RemoteDirectory, FileNameOnly, CheckNotModified]()
  {
    EditedFile * F = FindFile(Id);
    if ((F == nullptr) || Term.isNull())
    {
      if (F != nullptr)
      {
        UploadComplete(F, false);
      }
      return;
    }
    // so no masking is needed: uploaded under the original name
    CopyParam Param = Configuration::Instance().DefaultCopyParam().TemporaryFileCopyParam(F->ForceText);
    Param.FileMask = (FileNameOnly == F->OriginalFileName) ? DefaultCopyFileMask : DelimitFileNameMask(F->OriginalFileName);
    Param.ConfirmOverwriting = false;
    ShowStatus(QStringLiteral("Uploading changes of '%1'...").arg(FileNameOnly));
    auto Source = std::make_shared<RemoteFile>();
    // ExecutedFileChanged: with background transfers the upload is a queue item
    // (TEditorUploadQueueItem, cpNoConfirmation | cpTemporary), shown in the queue list
    TransferQueue * Queue = FHooks.Queue ? FHooks.Queue(Term) : nullptr;
    if (Queue != nullptr)
    {
      Param.QueueNoConfirmation = true;
      int ItemId = Queue->AddUpload({LocalFileName}, RemoteDirectory, Param, false);
      auto Connection = std::make_shared<QMetaObject::Connection>();
      *Connection = connect(Queue, &TransferQueue::ItemFinished, this,
        [this, Id, Term, ItemId, RemotePath, RemoteDirectory, FileNameOnly, Connection](int FinishedId, const OperationResult & Result, bool, const QString &, bool)
        {
          if (FinishedId != ItemId)
          {
            return;
          }
          disconnect(*Connection);
          ShowStatus(Result.Ok ? QStringLiteral("'%1' uploaded.").arg(FileNameOnly) : QString(), 3000);
          if (Result.Ok && FHooks.FileUploaded && !Term.isNull())
          {
            FHooks.FileUploaded(Term, RemoteDirectory);
          }
          EditedFile * F = FindFile(Id);
          if (F == nullptr)
          {
            return;
          }
          if (!Result.Ok || Term.isNull())
          {
            UploadComplete(F, Result.Ok);
            return;
          }
          // new source timestamp for EditorCheckNotModified
          auto Uploaded = std::make_shared<RemoteFile>();
          Term->RunOnWorker([RemotePath, Uploaded](FileSystem & Fs, FileOperationProgress &, UserInterface &)
            {
              *Uploaded = Fs.ReadFile(RemotePath);
            }, [this, Id, Uploaded](const OperationResult &)
            {
              if (EditedFile * F2 = FindFile(Id))
              {
                if (Uploaded->Modification.isValid())
                {
                  F2->SourceTimestamp = Uploaded->Modification;
                }
                UploadComplete(F2, true);
              }
            });
        });
      return;
    }
    RunOperation(Term, [Term, LocalFileName, RemoteDirectory, RemotePath, Param, Source, CheckNotModified](Terminal::Callback Done)
      {
        Term->CopyToRemote({LocalFileName}, RemoteDirectory, Param, false, [Term, RemotePath, Source, CheckNotModified, Done](const OperationResult & Result)
        {
          if (!Result.Ok || !CheckNotModified || Term.isNull())
          {
            Done(Result);
            return;
          }
          // EditedFileUploaded: the new source timestamp
          Term->RunOnWorker([RemotePath, Source](FileSystem & Fs, FileOperationProgress &, UserInterface &)
            {
              *Source = Fs.ReadFile(RemotePath);
            }, [Result, Done](const OperationResult &) { Done(Result); });
        });
      },
      [this, Id, Term, RemoteDirectory, FileNameOnly, Source](const OperationResult & Result)
      {
        EditedFile * F = FindFile(Id);
        if (!Result.Ok && !Result.Aborted && !Result.Fatal)
        {
          ShowErrorMessage(FWindow, Result.Message, Result.MoreMessages);
        }
        ShowStatus(Result.Ok ? QStringLiteral("'%1' uploaded.").arg(FileNameOnly) : QString(), 3000);
        if (Result.Ok && FHooks.FileUploaded && !Term.isNull())
        {
          FHooks.FileUploaded(Term, RemoteDirectory);
        }
        if (F != nullptr)
        {
          if (Result.Ok && Source->Modification.isValid())
          {
            F->SourceTimestamp = Source->Modification;
          }
          UploadComplete(F, Result.Ok);
        }
      }, false);
  };

  if (!CheckNotModified)
  {
    Upload();
    return;
  }
  // EditorCheckNotModified: was the remote file changed meanwhile?
  auto Current = std::make_shared<RemoteFile>();
  RunOperation(Term, [Term, RemotePath, Current](Terminal::Callback Done)
    {
      Term->RunOnWorker([RemotePath, Current](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          RemoteFile File;
          if (Fs.FileExists(RemotePath, &File))
          {
            *Current = File;
          }
        }, Done);
    },
    [this, Id, Current, Upload](const OperationResult &)
    {
      EditedFile * F = FindFile(Id);
      if (F == nullptr)
      {
        return;
      }
      if (Current->Modification.isValid() && (Current->Modification != F->SourceTimestamp))
      {
        // EDIT_CHANGED_EXTERNALLY
        if (MessageDialog(FWindow, QStringLiteral("**The remote file has been changed while you were editing it. "
              "Do you want to overwrite it anyway?**"), qtConfirmation, qaOK | qaCancel) != qaOK)
        {
          UploadComplete(F, false);
          return;
        }
      }
      Upload();
    }, false);
}

void EditorManager::UploadComplete(EditedFile * File, bool Ok)
{
  // TEditorManager::UploadComplete
  File->Uploading = false;
  emit FileUploaded(File->FileName, Ok);
  if (File->Closed)
  {
    CloseFile(File, true);
  }
  else if (File->Reupload)
  {
    File->Reupload = false;
    CheckFileChange(File, true);
  }
}

void EditorManager::FileReload(EditedFile * File, const std::function<void(bool)> & Done)
{
  // TCustomScpExplorerForm::ExecutedFileReload
  if (!TerminalAvailable(File->Term))
  {
    // EDIT_SESSION_CLOSED_RELOAD
    ShowErrorMessage(File->Editor.isNull() ? FWindow : static_cast<QWidget *>(File->Editor),
      QStringLiteral("Cannot reload file '%1', the session '%2' has been already closed.")
        .arg(QFileInfo(File->FileName).fileName(), File->SessionName));
    Done(false);
    return;
  }
  if (!TerminalIdle(File->Term) || File->Uploading)
  {
    ShowErrorMessage(File->Editor.isNull() ? FWindow : static_cast<QWidget *>(File->Editor),
      QStringLiteral("Cannot reload file '%1' now, another operation is in progress.").arg(QFileInfo(File->FileName).fileName()));
    Done(false);
    return;
  }
  quint64 Id = File->Id;
  File->Reloading = true;
  QPointer<Terminal> Term(File->Term);
  QString RemotePath = File->RemotePath();
  QString Directory = QFileInfo(File->FileName).absolutePath();
  QString FileNameOnly = QFileInfo(File->FileName).fileName();
  CopyParam Param = Configuration::Instance().DefaultCopyParam().TemporaryFileCopyParam(File->ForceText);
  Param.FileMask = (FileNameOnly == File->OriginalFileName) ? DefaultCopyFileMask : DelimitFileNameMask(FileNameOnly);
  Param.ConfirmOverwriting = false;
  auto Source = std::make_shared<RemoteFile>();
  bool CheckNotModified = Configuration::Instance().EditorCheckNotModified;
  RunOperation(Term, [Term, RemotePath, Directory, Param, Source, CheckNotModified](Terminal::Callback Finished)
    {
      auto Download = [Term, RemotePath, Directory, Param, Finished]()
      {
        Term->CopyToLocal({RemotePath}, Directory, Param, false, Finished);
      };
      if (!CheckNotModified)
      {
        Download();
        return;
      }
      Term->RunOnWorker([RemotePath, Source](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          *Source = Fs.ReadFile(RemotePath);
        }, [Term, Download, Finished](const OperationResult & Result)
        {
          if (!Result.Ok || Term.isNull())
          {
            Finished(Result);
            return;
          }
          Download();
        });
    },
    [this, Id, Source, Done](const OperationResult & Result)
    {
      EditedFile * F = FindFile(Id);
      if (F != nullptr)
      {
        F->Reloading = false;
        if (Result.Ok)
        {
          FileStamp(F->FileName, F->StampTime, F->StampSize);
          if (Source->Modification.isValid())
          {
            F->SourceTimestamp = Source->Modification;
          }
        }
      }
      if (!Result.Ok && !Result.Aborted && !Result.Fatal)
      {
        QWidget * Parent = ((F != nullptr) && !F->Editor.isNull()) ? static_cast<QWidget *>(F->Editor) : FWindow;
        ShowErrorMessage(Parent, Result.Message, Result.MoreMessages);
      }
      Done(Result.Ok);
    }, true);
}

void EditorManager::InternalEditorClosed(EditedFile * File, bool Forced)
{
  // TEditorManager::FileClosed
  File->Editor = nullptr;
  CheckFileChange(File, false);
  if (FindFile(File->Id) == File)
  {
    CloseFile(File, !Forced);
  }
}

void EditorManager::ProcessFinished(EditedFile * File)
{
  // TEditorManager::Check, process part; the pidfd stays readable, so stop
  // listening before anything can run a nested event loop (dialogs)
  if (File->PidNotifier != nullptr)
  {
    File->PidNotifier->setEnabled(false);
  }
  if (File->PidPoll != nullptr)
  {
    File->PidPoll->stop();
  }
  quint64 Id = File->Id;
  CheckFileChange(File, false);
  File = FindFile(Id);
  if ((File != nullptr) && !EarlyClose(File))
  {
    CloseFile(File, true);
  }
}

bool EditorManager::EarlyClose(EditedFile * File)
{
  // TEditorManager::EarlyClose + TCustomScpExplorerForm::ExecutedFileEarlyClosed
  int EarlyCloseSeconds = EditorConfigInteger(QStringLiteral("EarlyClose"), 2);
  if (!File->HasProcess() || (File->Opened.elapsed() > EarlyCloseSeconds * 1000) || File->Command.isEmpty())
  {
    return false;
  }
  Configuration & Config = Configuration::Instance();
  bool AnyFound = false;
  bool AnyMDI = false;
  bool AnyNonMDI = false;
  bool AnyDetect = false;
  for (const EditorPreferences & Editor : Config.Editors)
  {
    if ((Editor.Editor == edExternal) && (Editor.ExternalEditor == File->Command))
    {
      AnyFound = true;
      if (Editor.SDIExternalEditor)
      {
        AnyNonMDI = true;
        AnyDetect = AnyDetect || Editor.DetectMDIExternalEditor;
      }
      else
      {
        AnyMDI = true;
      }
    }
  }
  bool KeepOpen = false;
  bool EnableMDI = false;
  bool DisableDetect = false;
  if (AnyMDI)
  {
    KeepOpen = true;
    EnableMDI = AnyNonMDI;
  }
  else if (AnyFound && !AnyDetect)
  {
    // all instances have MDI autodetection disabled: close the file
  }
  else
  {
    MessageParams Params;
    if (AnyFound)
    {
      Params.Params |= qpNeverAskAgainCheck;
    }
    // EDITOR_EARLY_CLOSED2
    QString Message = QStringLiteral("Editor (application) launched to open file '%1' was closed too early. If it was not closed by you, "
      "it may have happened because your external editor opens multiple files in one window (process). Additional launched "
      "instances of the editor then just sends the new file to existing instance of editor and closes itself immediately. "
      "To support this kind of editors, NixSCP must adapt its behavior, not to remove temporary file when the process exits, "
      "but to keep it as long as NixSCP is running. The behavior can be turned on by unsetting editor preference option "
      "'External editor opens each file in separate window (process)'. If your editor is not of this kind, ignore this "
      "message and let the file be removed from temporary directory now.\n \nDo you want to remove the opened file now? "
      "(By pressing 'No' you will enable the special behavior and keep the file in temporary folder.)").arg(File->OriginalFileName);
    quint64 Id = File->Id;
    unsigned int Answer = MessageDialog(FWindow, Message, qtWarning, qaYes | qaNo, QStringLiteral("task_edit#mdi"), &Params);
    if (FindFile(Id) != File)
    {
      return true;
    }
    if (Answer == qaNeverAskAgain)
    {
      DisableDetect = true;
    }
    else if (Answer == qaNo)
    {
      EnableMDI = true;
      KeepOpen = true;
    }
  }
  if (AnyFound && (EnableMDI || DisableDetect))
  {
    bool Changed = false;
    for (EditorPreferences & Editor : Config.Editors)
    {
      if ((Editor.Editor == edExternal) && (Editor.ExternalEditor == File->Command))
      {
        if (EnableMDI && Editor.SDIExternalEditor)
        {
          Editor.SDIExternalEditor = false;
          Changed = true;
        }
        if (DisableDetect && Editor.DetectMDIExternalEditor)
        {
          Editor.DetectMDIExternalEditor = false;
          Changed = true;
        }
      }
    }
    if (Changed)
    {
      Config.Save();
    }
  }
  if (KeepOpen)
  {
    // forget the process, keep watching the file (as an MDI editor)
    CloseProcess(File);
  }
  return KeepOpen;
}

void EditorManager::CloseProcess(EditedFile * File)
{
  delete File->PidNotifier;
  File->PidNotifier = nullptr;
#ifdef Q_OS_WIN
  if (File->PidHandle != nullptr)
  {
    CloseHandle(File->PidHandle);
    File->PidHandle = nullptr;
  }
#else
  if (File->PidFd >= 0)
  {
    ::close(File->PidFd);
    File->PidFd = -1;
  }
#endif
  delete File->PidPoll;
  File->PidPoll = nullptr;
  File->Pid = 0;
}

void EditorManager::CloseFile(EditedFile * File, bool Delete)
{
  // TEditorManager::CloseFile
  CloseProcess(File);
  if (File->Uploading)
  {
    // finished when the upload completes
    File->Closed = true;
    return;
  }
  FFiles.removeAll(File);
  if (FWatcher->files().contains(File->FileName))
  {
    FWatcher->removePath(File->FileName);
  }
  QString Dir = QFileInfo(File->FileName).absolutePath();
  bool DirUsed = false;
  bool AnyExternal = false;
  for (EditedFile * Other : FFiles)
  {
    DirUsed = DirUsed || (QFileInfo(Other->FileName).absolutePath() == Dir);
    AnyExternal = AnyExternal || Other->External;
  }
  if (!DirUsed && FWatcher->directories().contains(Dir))
  {
    FWatcher->removePath(Dir);
  }
  if (!AnyExternal)
  {
    FCheckTimer->stop();
  }
  if (File->Lock)
  {
    File->Lock->unlock();
  }
  if (Delete && !File->LocalRootDirectory.isEmpty())
  {
    QDir(File->LocalRootDirectory).removeRecursively();
  }
  QString FileName = File->FileName;
  delete File;
  emit FileClosed(FileName);
  UpdateActions();
}

//---------------------------------------------------------------------------
// Exit
//---------------------------------------------------------------------------
bool EditorManager::CanClose()
{
  // TCustomScpExplorerForm::FormCloseQuery: CloseInternalEditors
  const QList<EditedFile *> Files = FFiles;
  for (int Index = Files.size() - 1; Index >= 0; Index--)
  {
    EditedFile * File = Files[Index];
    if (FFiles.contains(File) && !File->Closed && !File->Editor.isNull())
    {
      File->Editor->close();
    }
  }
  for (EditedFile * File : FFiles)
  {
    if (!File->Closed && !File->Editor.isNull())
    {
      return false;
    }
  }
  // CloseExternalFilesWithoutProcess (after uploading their last changes)
  Check();
  // uploads of the files just saved/closed are not in the queue: let them
  // finish (the sessions are closed with the window)
  auto AnyUploading = [this]()
  {
    for (EditedFile * File : FFiles)
    {
      if (File->Uploading || File->RetryPending)
      {
        return true;
      }
    }
    return false;
  };
  if (AnyUploading())
  {
    ShowStatus(QStringLiteral("Uploading edited files..."));
    QEventLoop Loop;
    QTimer Poll;
    Poll.setInterval(50);
    QElapsedTimer Waited;
    Waited.start();
    connect(&Poll, &QTimer::timeout, &Loop, [&]()
    {
      if (!AnyUploading() || (Waited.elapsed() > 60000))
      {
        Loop.quit();
      }
    });
    Poll.start();
    Loop.exec();
    ShowStatus(QString());
  }
  const QList<EditedFile *> Remaining = FFiles;
  for (EditedFile * File : Remaining)
  {
    if (FFiles.contains(File) && !File->Closed && File->External && !File->HasProcess() && !File->Uploading)
    {
      CloseFile(File, true);
    }
  }
  // local files in internal editors
  while (!FLocalEditors.isEmpty())
  {
    int PrevCount = FLocalEditors.size();
    FLocalEditors.first()->close();
    if (FLocalEditors.size() >= PrevCount)
    {
      return false;
    }
  }
  if (!FFiles.isEmpty())
  {
    // PENDING_EDITORS (files with running editor processes or being uploaded)
    if (MessageDialog(FWindow, QStringLiteral("There are some opened files. Please close them before exiting application.\n \n"
          "Beware: If you ignore this message, opened files may remain in temporary directory."),
          qtWarning, qaIgnore | qaCancel) != qaIgnore)
    {
      return false;
    }
  }
  return true;
}

bool EditorManager::Empty() const
{
  return FFiles.isEmpty();
}

int EditorManager::Count() const
{
  return static_cast<int>(FFiles.size());
}

QStringList EditorManager::EditedFiles() const
{
  QStringList Result;
  for (EditedFile * File : FFiles)
  {
    Result.append(File->FileName);
  }
  return Result;
}

//---------------------------------------------------------------------------
// Static helpers
//---------------------------------------------------------------------------
static QStringList SplitCommandLine(const QString & Command)
{
  // like a POSIX shell, without any expansion: whitespace separates,
  // '...' is literal, "..." allows \" \\ \$ \`, \ escapes outside quotes
  QStringList Result;
  QString Current;
  bool InArgument = false;
  QChar Quote;
  for (int Index = 0; Index < Command.size(); Index++)
  {
    QChar C = Command.at(Index);
    if (Quote == QLatin1Char('\''))
    {
      if (C == QLatin1Char('\''))
      {
        Quote = QChar();
      }
      else
      {
        Current += C;
      }
    }
    else if (Quote == QLatin1Char('"'))
    {
      if (C == QLatin1Char('"'))
      {
        Quote = QChar();
      }
      else if ((C == QLatin1Char('\\')) && (Index + 1 < Command.size()) &&
               QStringLiteral("\"\\$`").contains(Command.at(Index + 1)))
      {
        Current += Command.at(++Index);
      }
      else
      {
        Current += C;
      }
    }
    else if (C.isSpace())
    {
      if (InArgument)
      {
        Result.append(Current);
        Current.clear();
        InArgument = false;
      }
    }
    else
    {
      InArgument = true;
      if ((C == QLatin1Char('\'')) || (C == QLatin1Char('"')))
      {
        Quote = C;
      }
      else if ((C == QLatin1Char('\\')) && (Index + 1 < Command.size()))
      {
        Current += Command.at(++Index);
      }
      else
      {
        Current += C;
      }
    }
  }
  if (InArgument)
  {
    Result.append(Current);
  }
  return Result;
}

QStringList EditorManager::EditorCommandArguments(const QString & Command, const QString & FileName)
{
  // SplitCommand + ExpandFileNameCommand, per argument (no shell)
  QStringList Args = SplitCommandLine(Command);
  if (Args.isEmpty())
  {
    return Args;
  }
  bool Found = false;
  for (int Index = 1; Index < Args.size(); Index++)
  {
    QString & Arg = Args[Index];
    if (Arg.contains(QStringLiteral("!.!")))
    {
      Arg.replace(QStringLiteral("!.!"), FileName);
      Found = true;
    }
    else if (Arg.contains(QStringLiteral("%1")))
    {
      Arg.replace(QStringLiteral("%1"), FileName);
      Found = true;
    }
  }
  if (!Found)
  {
    Args.append(FileName);
  }
  Args[0] = SessionData::ExpandEnvironmentVariables(Args[0]);
  return Args;
}

QString EditorManager::LockFileName()
{
  return QStringLiteral(".nixscp.lock");
}

QString EditorManager::TemporaryBaseDirectory()
{
  // TWinConfiguration::ExpandedTemporaryDirectory; the system default is
  // the private $XDG_RUNTIME_DIR when available
  Configuration & Config = Configuration::Instance();
  if (!Config.DDTemporaryDirectory.trimmed().isEmpty())
  {
    return Config.ExpandedTemporaryDirectory();
  }
  QString Runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  QFileInfo Info(Runtime);
  if (!Runtime.isEmpty() && Info.isAbsolute() && Info.isDir() && Info.isWritable())
  {
    while ((Runtime.size() > 1) && Runtime.endsWith(QLatin1Char('/')))
    {
      Runtime.chop(1);
    }
    return Runtime;
  }
  return QDir::tempPath();
}

QStringList EditorManager::TemporaryBaseDirectories()
{
  QStringList Result;
  for (const QString & Dir : {TemporaryBaseDirectory(), qEnvironmentVariable("XDG_RUNTIME_DIR"), QDir::tempPath()})
  {
    if (!Dir.isEmpty() && QFileInfo(Dir).isDir())
    {
      QString Canonical = QFileInfo(Dir).canonicalFilePath();
      if (!Result.contains(Canonical))
      {
        Result.append(Canonical);
      }
    }
  }
  return Result;
}

QString EditorManager::CreateUniqueTemporaryDirectory(const QString & Base, std::unique_ptr<QLockFile> * Lock)
{
  // UniqTempDir: mkdtemp creates the folder with mode 0700
  if (!QFileInfo(Base).isDir() && !QDir().mkpath(Base))
  {
    return QString();
  }
  QTemporaryDir Dir(QDir(Base).filePath(TemporaryFolderPrefix + QStringLiteral("XXXXXX")));
  if (!Dir.isValid())
  {
    return QString();
  }
  Dir.setAutoRemove(false);
  QString Path = Dir.path();
  if (Lock != nullptr)
  {
    auto File = std::make_unique<QLockFile>(QDir(Path).filePath(LockFileName()));
    File->setStaleLockTime(0);
    if (File->tryLock(0))
    {
      *Lock = std::move(File);
    }
  }
  return Path;
}

QStringList EditorManager::FindTemporaryFolders()
{
  // TWinConfiguration::DoFindTemporaryFolders
  QStringList Result;
  for (const QString & Base : TemporaryBaseDirectories())
  {
    const QFileInfoList Entries = QDir(Base).entryInfoList({TemporaryFolderPrefix + QStringLiteral("*")},
      QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo & Entry : Entries)
    {
      if (!Platform::OwnedByCurrentUser(Entry.ownerId()))
      {
        continue;
      }
      QString Suffix = Entry.fileName().mid(TemporaryFolderPrefix.size());
      bool IsNumber = false;
      qint64 Pid = Suffix.toLongLong(&IsNumber);
      if (IsNumber && IsProcessAlive(Pid))
      {
        // nixscp-<pid> of an older NixSCP still running
        continue;
      }
      QString LockPath = QDir(Entry.filePath()).filePath(LockFileName());
      if (QFileInfo::exists(LockPath))
      {
        QLockFile Lock(LockPath);
        Lock.setStaleLockTime(0);
        if (!Lock.tryLock(0))
        {
          // in use by a running instance
          continue;
        }
        Lock.unlock();
      }
      else if (!IsNumber && (Entry.lastModified().secsTo(QDateTime::currentDateTime()) < 60))
      {
        // possibly just being created by another instance
        continue;
      }
      Result.append(Entry.filePath());
    }
  }
  return Result;
}

bool EditorManager::CleanupTemporaryFolders(const QStringList & Folders)
{
  bool Result = true;
  for (const QString & Folder : Folders)
  {
    if (!QDir(Folder).removeRecursively())
    {
      Result = false;
    }
  }
  return Result;
}

void EditorManager::TemporaryDirectoryCleanup(QWidget * Parent)
{
  // windows/Setup.cpp TemporaryDirectoryCleanup
  Configuration & Config = Configuration::Instance();
  if (!Config.TemporaryDirectoryCleanup)
  {
    return;
  }
  QStringList Folders = FindTemporaryFolders();
  if (Folders.isEmpty())
  {
    return;
  }
  bool Continue = true;
  if (Config.ConfirmTemporaryDirectoryCleanup)
  {
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    MessageButtonAlias Alias;
    Alias.Button = qaRetry;
    Alias.Alias = QStringLiteral("&Open");   // OPEN_BUTTON
    Params.Aliases.append(Alias);
    // CLEANTEMP_CONFIRM2
    QString Message = QStringLiteral("**Do you want to delete past temporary folders?**\n\nNixSCP has found %1 temporary folders, "
      "which it has probably created in past. These folders may contain files previously edited or downloaded.\n\n"
      "You may also open the folders to review their content and to delete them yourself.").arg(Folders.size());
    unsigned int Answer = MoreMessageDialog(Parent, Message, Folders.join(QLatin1Char('\n')), qtConfirmation,
      qaYes | qaNo | qaRetry, QStringLiteral("temp_folders"), &Params);
    if (Answer == qaNeverAskAgain)
    {
      Config.ConfirmTemporaryDirectoryCleanup = false;
      Config.Save();
      Answer = qaYes;
    }
    else if (Answer == qaRetry)
    {
      for (const QString & Folder : Folders)
      {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Folder));
      }
    }
    Continue = (Answer == qaYes);
  }
  if (Continue && !CleanupTemporaryFolders(Folders))
  {
    // CLEANUP_TEMP_ERROR
    ShowErrorMessage(Parent, QStringLiteral("Some of the temporary folders have not been deleted. If you have some files stored "
      "in them still opened, close them and try again."));
  }
}

bool EditorManager::DetectSystemExternalEditor(QString & Command, QString & Description)
{
  // DetectSystemExternalEditor: the default application for text/plain
  QProcess Query;
  Query.start(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("text/plain")});
  if (!Query.waitForFinished(3000) || (Query.exitStatus() != QProcess::NormalExit) || (Query.exitCode() != 0))
  {
    Query.kill();
    return false;
  }
  QString Desktop = QString::fromLocal8Bit(Query.readAllStandardOutput()).trimmed().section(QLatin1Char(';'), 0, 0);
  if (Desktop.isEmpty())
  {
    return false;
  }
  QString DesktopFile = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, Desktop);
  QFile File(DesktopFile);
  if (DesktopFile.isEmpty() || !File.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    return false;
  }
  QString Exec;
  QString Name;
  bool InEntry = false;
  bool TerminalApp = false;
  while (!File.atEnd())
  {
    QString Line = QString::fromUtf8(File.readLine()).trimmed();
    if (Line.startsWith(QLatin1Char('[')))
    {
      InEntry = (Line == QStringLiteral("[Desktop Entry]"));
    }
    else if (InEntry)
    {
      if (Line.startsWith(QStringLiteral("Exec=")))
      {
        Exec = Line.mid(5);
      }
      else if (Line.startsWith(QStringLiteral("Name=")))
      {
        Name = Line.mid(5);
      }
      else if (Line.startsWith(QStringLiteral("Terminal=")))
      {
        TerminalApp = (Line.mid(9).trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
      }
    }
  }
  if (Exec.isEmpty() || TerminalApp)
  {
    return false;
  }
  // field codes of the Desktop Entry Specification
  bool HasFile = false;
  QString Result;
  for (int Index = 0; Index < Exec.size(); Index++)
  {
    QChar C = Exec.at(Index);
    if ((C == QLatin1Char('%')) && (Index + 1 < Exec.size()))
    {
      QChar Code = Exec.at(++Index);
      if (Code == QLatin1Char('%'))
      {
        Result += C;
      }
      else if (QStringLiteral("fFuU").contains(Code) && !HasFile)
      {
        Result += QStringLiteral("!.!");
        HasFile = true;
      }
    }
    else
    {
      Result += C;
    }
  }
  Result = Result.simplified();
  if (!HasFile)
  {
    Result += QStringLiteral(" !.!");
  }
  QStringList Args = SplitCommandLine(Result);
  if (Args.isEmpty() || (QFileInfo(Args.first()).fileName().startsWith(QStringLiteral("nixscp"))))
  {
    return false;
  }
  Command = Result;
  Description = Name.isEmpty() ? QFileInfo(Args.first()).fileName() : Name;
  return true;
}
