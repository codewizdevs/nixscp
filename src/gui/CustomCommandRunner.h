// Execution of custom commands, port of WinSCP source/forms/CustomScpExplorer.cpp
// (CustomCommand, RemoteCustomCommand, LocalCustomCommand,
// LocalCustomCommandPure, LocalCustomCommandWithLocalFiles, AdHocCustomCommand,
// CustomCommandState inputs), core/Terminal.cpp (CustomCommandOnFiles,
// CustomCommandOnFile, DoCustomCommandOnFile) + core/ScpFileSystem.cpp
// (CustomCommandOnFile: recursion, apply to directories), and
// windows/WinInterface.cpp (TWinInteractiveCustomCommand,
// TCustomCommandPromptsDialog).
//
// Remote commands run through the session (Terminal::AnyCommand / an
// AnyCommandFileSystem on the worker thread) in the current remote directory.
// Local commands run with /bin/sh -c in the local panel directory; for
// remote files, the files are downloaded to a temporary directory first and
// the files the command modified are uploaded back (unless "Use remote files").
#pragma once

#include "core/CustomCommands.h"

#include <QString>
#include <QStringList>
#include <functional>

class QWidget;
class Terminal;

// Where and on what a command is executed
struct CustomCommandContext
{
  Terminal * Term = nullptr;            // session of the tab (null or closed: local commands without session patterns only)
  QString SessionPassword;              // password typed at login, when not stored in Term->Data().Password (for !P, !S)
  QString CurrentRemoteDir;             // remote panel directory (!/; remote commands run there)
  QStringList SelectedRemoteFiles;      // remote panel selection: names relative to CurrentRemoteDir (as WinSCP) or absolute paths
  QString CurrentLocalDir;              // local panel directory (!\; local commands run there)
  QStringList SelectedLocalFiles;       // local panel selection: absolute paths (local-local tab: the current panel's selection)
  bool LocalSide = false;               // the local panel is the current one (local commands then work with local files)
  bool RemotePanel = true;              // false for a local-only (local-local) tab
};

class CustomCommandRunner
{
public:
  // TCustomScpExplorerForm::CustomCommand: asks for the !?prompt?! values,
  // executes the command and shows/copies the results (flags). Returns at
  // once; Done (optional) is called on the GUI thread when everything has
  // finished (also after errors or cancel) - reload the panels then, as
  // commands may have changed files. Errors are shown in message boxes.
  static void Execute(QWidget * Parent, const CustomCommandType & Command, const CustomCommandContext & Context,
    std::function<void()> Done = std::function<void()>());

  // TCustomScpExplorerForm::AdHocCustomCommand (Commands > Custom Commands >
  // Enter...): LastCommand is the last ad hoc command (initial values) and
  // receives the entered one (WinSCP FLastCustomCommand). Returns false when
  // cancelled (Done is not called then).
  static bool ExecuteAdHoc(QWidget * Parent, CustomCommandType & LastCommand, const CustomCommandContext & Context,
    std::function<void()> Done = std::function<void()>());

  // Inputs of CustomCommandState() for menus/toolbars:
  //   CustomCommandState(Command, CustomCommandRunner::Availability(Context), ccltAll) > 0
  static CustomCommandAvailability Availability(const CustomCommandContext & Context);

  // TCustomCommandPromptsDialog ("'%s' command parameters"), with history
  static bool PromptValues(QWidget * Parent, const QString & CommandName, const QStringList & Prompts,
    const QStringList & Defaults, QStringList & Values);

  // Results window of "Show results in terminal" (WinSCP shows the Console
  // window with the captured output)
  static void ShowResults(QWidget * Parent, const QString & Title, const QStringList & Lines);
};

// Convenience for the remote panel: Execute() with the remote selection
void ExecuteCustomCommand(QWidget * Parent, Terminal * Term, const CustomCommandType & Command,
  const QStringList & SelectedRemotePaths, const QString & CurrentRemoteDir, std::function<void()> Done);
