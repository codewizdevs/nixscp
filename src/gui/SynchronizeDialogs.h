// Synchronization user interface. Ports of WinSCP
//   source/forms/FullSynchronize.{cpp,dfm}       Synchronize dialog
//   source/forms/SynchronizeChecklist.{cpp,dfm}  Synchronization checklist
//   source/forms/SynchronizeProgress.{cpp,dfm}   Comparing/synchronizing progress
//   source/forms/Synchronize.{cpp,dfm}           Keep remote directory up to date
// and of the orchestration in source/forms/CustomScpExplorer.cpp
// (DoFullSynchronizeDirectories, FullSynchronize, DoSynchronizeDirectories)
// and source/forms/ScpCommander.cpp (CompareDirectories, FullSynchronizeDirectories).
//
// Settings are kept as in WinSCP.ini, [Configuration\Interface]:
//   SynchronizeParams, SynchronizeOptions, SynchronizeModeAuto, SynchronizeMode,
//   MaxWatchDirectories, KeepUpToDateChangeDelay; [Configuration\Interface\SynchronizeChecklist]
//   WindowParams, ListParams; directory histories [Configuration\History\LocalDirectory]
//   and [...\RemoteDirectory]; plus Configuration::SynchronizeSummary,
//   Configuration::DeleteToRecycleBin and Configuration::CompareByTime/CompareBySize
//   ([Configuration\Interface\Commander]).
//
// Linux adaptations: "Minimize to System Tray" uses QSystemTrayIcon (the item
// is not offered without a system tray); "Explore" of the checklist opens the
// local directory in the desktop file manager (FileManager1.ShowItems with
// xdg-open fallback).
#pragma once

#include "core/Synchronize.h"

#include <QStringList>
#include <functional>

class QWidget;
class TransferQueue;

// Synchronization settings of TGUIConfiguration (read from / written to
// Configuration::Instance().Ini(); Save() also saves the configuration file)
struct SynchronizeConfiguration
{
  int Params = spDefault;                       // SynchronizeParams
  int Options = soRecurse | soSynchronizeAsk;   // SynchronizeOptions
  int ModeAuto = -1;                            // SynchronizeModeAuto (-1 = by active panel)
  int Mode = smRemote;                          // SynchronizeMode
  int KeepUpToDateChangeDelay = 500;            // KeepUpToDateChangeDelay (ms)
  int MaxWatchDirectories = 500;                // MaxWatchDirectories
  // SynchronizeChecklist\WindowParams ("CustomPos;Left;Top;Right;Bottom;State;PixelsPerInch",
  // Left = Top = -1: size only) and SynchronizeChecklist\ListParams
  // ("SortColumn;SortAscending|Width,Visible;...;@PixelsPerInch|Order")
  QString ChecklistWindowParams = QStringLiteral("0;-1;-1;1090;700;0;96");
  QString ChecklistListParams = QStringLiteral("1;1|150,1;100,1;80,1;130,1;25,1;100,1;80,1;130,1;@96|0;1;2;3;4;5;6;7");

  static SynchronizeConfiguration Load();
  void Save() const;
};

// Integration points of the synchronization windows provided by the main
// window. Unset hooks hide the respective commands. (The transfer settings
// presets drop-downs use CopyParamListPopup of gui/CopyParamsDialog.h.)
struct SynchronizeUiHooks
{
  // Synchronization checklist > OK drop-down > Start in Background: the
  // background queue of the session of Term
  std::function<TransferQueue *(Terminal * Term)> Queue;
  // Checklist > Right Directory > Explore: show RemoteDirectory (with
  // FileName focused, when not empty) in the remote panel
  std::function<void(Terminal * Term, const QString & RemoteDirectory, const QString & FileName)> ExploreRemote;
  // Synchronize > OK drop-down > Start in New Window
  // (TCustomScpExplorerForm::FullSynchronizeInNewWindow)
  std::function<void(Terminal * Term, SynchronizeMode Mode, int Params, const QString & LocalDirectory,
    const QString & RemoteDirectory, const CopyParam & Param)> FullSynchronizeInNewWindow;
  // Keep remote directory up to date > Start drop-down > Start in New Window
  std::function<void(Terminal * Term, const SynchronizeParamType & Params, const CopyParam & Param)> KeepUpToDateInNewWindow;
};

SynchronizeUiHooks & SynchronizeHooks();

// Commands > Synchronize... (Ctrl+S): Synchronize dialog, comparison with
// progress window, Synchronization checklist (with "Preview changes"),
// synchronization with progress window and the summary. Returns once the
// Synchronize dialog is closed; the rest runs asynchronously (window-modal
// progress windows). Finished is called on the GUI thread when everything is
// over (also when cancelled), e.g. to refresh both panels.
//  LocalPanelActive: default direction when none was saved
//    (WinSCP: local panel active -> "Remote", otherwise "Local").
//  Param: current transfer settings (Configuration::DefaultCopyParam()).
//  SelectedNames: names of the files selected in both panels; non-empty
//    enables "Selected files only".
// The Terminal must be idle (not Busy) and must not be used until Finished.
void RunFullSynchronize(QWidget * Parent, Terminal * Term, const QString & LocalDirectory,
  const QString & RemoteDirectory, bool LocalPanelActive, const CopyParam & Param,
  const QStringList & SelectedNames, std::function<void()> Finished);

// Commands > Keep Remote Directory up to Date... (Ctrl+U): modal dialog that
// watches LocalDirectory and uploads changes until the user closes it.
// Finished is called after the dialog is closed (e.g. to refresh panels).
void RunKeepRemoteUpToDate(QWidget * Parent, Terminal * Term, const QString & LocalDirectory,
  const QString & RemoteDirectory, const CopyParam & Param, const QStringList & SelectedNames,
  std::function<void()> Finished);

// Commands > Compare Directories: compares the files shown in the panels
// (non-recursive; RemoteFiles and LocalNames, without hidden and filtered out
// files) with the criteria of Preferences > Commander
// (Configuration::CompareByTime/CompareBySize). Select is called with the
// names to select in each panel (all other files should be deselected, as
// WinSCP does); when nothing differs, "No differences found." is shown and
// Select is called with empty lists.
void RunCompareDirectories(QWidget * Parent, const QString & LocalDirectory, const RemoteFileList & RemoteFiles,
  const QSet<QString> & LocalNames,
  std::function<void(const QStringList & LocalNames, const QStringList & RemoteNames)> Select);
