// Port of the workspace part of WinSCP source/core/SessionData.cpp and
// source/forms/CustomScpExplorer.cpp (see Workspaces.h)
#include "core/Workspaces.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"

#include <QFileInfo>
#include <QStringList>
#include <algorithm>

static QString FLastWorkspace;

static const QString AnonymousUserName = QStringLiteral("anonymous");

QString Workspaces::NewWorkspaceName()
{
  return QStringLiteral("My Workspace");
}

bool Workspaces::IsInFolderOrWorkspace(const SessionData & Data, const QString & Folder)
{
  // StartsText(UnixIncludeTrailingBackslash(AFolder), Name)
  QString Prefix = Folder;
  if (!Prefix.endsWith(QLatin1Char('/')))
  {
    Prefix += QLatin1Char('/');
  }
  return Data.Name.startsWith(Prefix, Qt::CaseInsensitive);
}

QStringList Workspaces::WorkspaceNames(const StoredSessionList & Sessions)
{
  // CreateSortedStringList: case insensitive, duplicates ignored
  QStringList Result;
  for (const auto & Data : Sessions.Sessions())
  {
    if (Data->IsWorkspace)
    {
      QString Name = Data->FolderName();
      bool Found = false;
      for (const QString & S : Result)
      {
        if (S.compare(Name, Qt::CaseInsensitive) == 0)
        {
          Found = true;
          break;
        }
      }
      if (!Found)
      {
        Result.append(Name);
      }
    }
  }
  std::sort(Result.begin(), Result.end(),
    [](const QString & A, const QString & B) { return A.compare(B, Qt::CaseInsensitive) < 0; });
  return Result;
}

// Unlike WinSCP not only the first session of the name decides: a site can
// share its folder with a workspace ("Prod" and "Prod/web"), and it must not
// pass for a workspace entry then
static bool AnySessionOfKind(const StoredSessionList & Sessions, const QString & Name, bool Workspace)
{
  if (!Name.isEmpty())
  {
    for (const auto & Data : Sessions.Sessions())
    {
      if ((Data->IsWorkspace == Workspace) && Workspaces::IsInFolderOrWorkspace(*Data, Name))
      {
        return true;
      }
    }
  }
  return false;
}

bool Workspaces::IsWorkspace(const StoredSessionList & Sessions, const QString & Name)
{
  return AnySessionOfKind(Sessions, Name, true);
}

bool Workspaces::IsFolder(const StoredSessionList & Sessions, const QString & Name)
{
  return AnySessionOfKind(Sessions, Name, false);
}

bool Workspaces::HasAnyWorkspace(const StoredSessionList & Sessions)
{
  for (const auto & Data : Sessions.Sessions())
  {
    if (Data->IsWorkspace)
    {
      return true;
    }
  }
  return false;
}

std::shared_ptr<SessionData> Workspaces::ResolveWorkspaceData(const StoredSessionList & Sessions, std::shared_ptr<SessionData> Data)
{
  // Unlike WinSCP, stop on (invalid) circular links
  for (int Depth = 0; (Data != nullptr) && !Data->Link.isEmpty(); Depth++)
  {
    if (Depth >= 32)
    {
      return nullptr;
    }
    Data = Sessions.FindByName(Data->Link);
  }
  return Data;
}

QList<SessionData> Workspaces::FolderOrWorkspace(const StoredSessionList & Sessions, const QString & Name)
{
  // TStoredSessionList::DoGetFolderOrWorkspace
  QList<SessionData> Result;
  // sites sharing the name of a workspace are not its tabs
  bool OnlyWorkspace = IsWorkspace(Sessions, Name);
  for (const auto & RawData : Sessions.Sessions())
  {
    if (!IsInFolderOrWorkspace(*RawData, Name) || (OnlyWorkspace && !RawData->IsWorkspace))
    {
      continue;
    }
    // CheckIsInFolderOrWorkspaceAndResolve
    std::shared_ptr<SessionData> Data = ResolveWorkspaceData(Sessions, RawData);
    if ((Data == nullptr) || !Data->CanOpen() || !Data->Link.isEmpty())
    {
      continue;
    }

    SessionData Data2 = *Data;
    // BACKWARD COMPATIBILITY: pre-5.6.4 workspaces have no state saved,
    // do not overwrite the site "state" defaults with (empty) workspace state
    if (!RawData->Link.isEmpty() && (Data != RawData) && HasStateData(*RawData))
    {
      CopyStateData(Data2, *RawData);
    }

    if (!RawData->NameOverride.isEmpty())
    {
      Data2.Name = RawData->NameOverride;
    }
    else if (RawData->Link.isEmpty() && RawData->IsWorkspace)
    {
      // Newly opened ad-hoc session has no name, so restore the workspace that way too.
      // Otherwise we would persist the generated internal workspace name as a real name.
      Data2.Name.clear();
    }

    Result.append(Data2);
  }
  return Result;
}

QList<SessionData> Workspaces::LoadWorkspace(const StoredSessionList & Sessions, const QString & Name)
{
  return FolderOrWorkspace(Sessions, Name);
}

QStringList Workspaces::FolderOrWorkspaceList(const StoredSessionList & Sessions, const QString & Name)
{
  QStringList Result;
  for (const SessionData & Data : FolderOrWorkspace(Sessions, Name))
  {
    // TSessionData::GetDefaultSessionName of a local browser
    Result.append(Data.CanLogin() ? Data.SessionName() :
      QFileInfo(Data.LocalDirectory).fileName() + QStringLiteral(" \u2013 ") + QFileInfo(Data.OtherLocalDirectory).fileName());
  }
  return Result;
}

QString Workspaces::EntryName(const QString & Workspace, int Index)
{
  // TSessionData::ComposePath(Name, IntToHex(Index, 4))
  QString Path = Workspace;
  if (!Path.endsWith(QLatin1Char('/')))
  {
    Path += QLatin1Char('/');
  }
  return Path + QStringLiteral("%1").arg(Index, 4, 16, QLatin1Char('0')).toUpper();
}

std::shared_ptr<SessionData> Workspaces::FindSame(const StoredSessionList & Sessions, const SessionData & Data)
{
  // TStoredSessionList::FindSame (NixSCP has no hidden sessions)
  if (Data.Name.isEmpty() || Data.IsWorkspace)
  {
    return nullptr;
  }
  std::shared_ptr<SessionData> Result = Sessions.FindByName(Data.Name);
  if ((Result != nullptr) && Result->IsWorkspace)
  {
    // a session cannot be the same as a workspace entry
    return nullptr;
  }
  return Result;
}

bool Workspaces::HasStateData(const SessionData & Data)
{
  return
    !Data.RemoteDirectory.isEmpty() ||
    !Data.LocalDirectory.isEmpty() ||
    !Data.OtherLocalDirectory.isEmpty() ||
    (Data.Color != 0);
}

void Workspaces::CopyStateData(SessionData & Target, const SessionData & Source)
{
  // CopyDirectoriesStateData + Color
  Target.RemoteDirectory = Source.RemoteDirectory;
  Target.LocalDirectory = Source.LocalDirectory;
  Target.OtherLocalDirectory = Source.OtherLocalDirectory;
  Target.SynchronizeBrowsing = Source.SynchronizeBrowsing;
  Target.Color = Source.Color;
}

bool Workspaces::HasAnySessionPassword(const SessionData & Data)
{
  return Data.HasAnySessionPassword();
}

void Workspaces::ClearSessionPasswords(SessionData & Data)
{
  Data.Password.clear();
  Data.TunnelPassword.clear();
  // WinSCP keeps these, but "without passwords" means no secret at all
  Data.ProxyPassword.clear();
  Data.S3SessionToken.clear();
  Data.TunnelPassphrase.clear();
}

// SaveWorkspaceData, the stored site branch: Result is a new TSessionData
// with only the state data and Link set. WinSCP saves it relative to factory
// defaults and loads it on top of "Default Settings". Here the entry is loaded
// exactly that way from the keys WinSCP writes, so that StoredSessionList
// (which saves relative to "Default Settings") writes the same keys and
// nothing else (no raw values inherited from "Default Settings").
static std::shared_ptr<SessionData> CreateLinkEntry(StoredSessionList & Sessions, const SessionData & Data, const QString & Link)
{
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Workspace"), true);
  // the values that differ from factory defaults (TSessionData::DoSave)
  if (!Data.LocalDirectory.isEmpty())
  {
    Storage.WriteString(QStringLiteral("LocalDirectory"), Data.LocalDirectory);
  }
  if (!Data.OtherLocalDirectory.isEmpty())
  {
    Storage.WriteString(QStringLiteral("OtherLocalDirectory"), Data.OtherLocalDirectory);
  }
  if (!Data.RemoteDirectory.isEmpty())
  {
    Storage.WriteString(QStringLiteral("RemoteDirectory"), Data.RemoteDirectory);
  }
  if (Data.SynchronizeBrowsing)
  {
    Storage.WriteBool(QStringLiteral("SynchronizeBrowsing"), true);
  }
  if (Data.Color != 0)
  {
    Storage.WriteInteger(QStringLiteral("Color"), Data.Color);
  }
  Storage.WriteBool(QStringLiteral("IsWorkspace"), true);
  Storage.WriteString(QStringLiteral("Link"), Link);

  auto Entry = std::make_shared<SessionData>(Sessions.DefaultSettings());
  Entry->Load(Storage);
  // Passwords are saved unconditionally (not relative to the defaults); a
  // link never carries any (WinSCP may write the redundant session
  // credentials, but they are ignored when the workspace is opened)
  Workspaces::ClearSessionPasswords(*Entry);
  return Entry;
}

void Workspaces::SaveWorkspace(StoredSessionList & Sessions, const QString & Name, const QList<SessionData> & Tabs, bool SavePasswords)
{
  // TTerminalManager::SaveWorkspace -> TStoredSessionList::SaveWorkspaceData
  QList<std::shared_ptr<SessionData>> DataList;
  for (int Index = 0; Index < Tabs.size(); Index++)
  {
    const SessionData & Data = Tabs[Index];
    std::shared_ptr<SessionData> Result;
    std::shared_ptr<SessionData> SameData = FindSame(Sessions, Data);
    if (SameData != nullptr)
    {
      // stored name (keeps the case of the stored site)
      Result = CreateLinkEntry(Sessions, Data, SameData->Name);
    }
    else
    {
      Result = std::make_shared<SessionData>(Data);
      Result->Link.clear();
      Result->NameOverride = Data.Name;
      // DoSaveWorkspace
      if (!SavePasswords)
      {
        ClearSessionPasswords(*Result);
      }
    }
    Result->IsWorkspace = true;
    DataList.append(Result);
  }

  // TStoredSessionList::NewWorkspace
  DeleteWorkspace(Sessions, Name);
  // A site of a folder of the same name may hold the name of an entry ("Prod/0000")
  int EntryIndex = 0;
  for (const auto & Data : DataList)
  {
    while (Sessions.FindByName(EntryName(Name, EntryIndex)) != nullptr)
    {
      EntryIndex++;
    }
    Data->Name = EntryName(Name, EntryIndex);
    Sessions.Add(Data);
  }
}

int Workspaces::DeleteWorkspace(StoredSessionList & Sessions, const QString & Name)
{
  // Only the entries of the workspace: sites never go with it, whatever the
  // workspace is called (WinSCP's NewWorkspace removes all sessions of the name)
  QStringList Remove;
  for (const auto & Data : Sessions.Sessions())
  {
    if (Data->IsWorkspace && IsInFolderOrWorkspace(*Data, Name))
    {
      Remove.append(Data->Name);
    }
  }
  for (const QString & RemoveName : Remove)
  {
    Sessions.Remove(RemoveName);
  }
  return Remove.size();
}

int Workspaces::UpdateLinks(StoredSessionList & Sessions, const QString & OldName, const QString & NewName, bool Folder)
{
  int Result = 0;
  QString Prefix = OldName + QLatin1Char('/');
  for (const auto & Data : Sessions.Sessions())
  {
    if (!Data->IsWorkspace || Data->Link.isEmpty())
    {
      continue;
    }
    if (!Folder && (Data->Link.compare(OldName, Qt::CaseInsensitive) == 0))
    {
      Data->Link = NewName;
      Result++;
    }
    else if (Folder && Data->Link.startsWith(Prefix, Qt::CaseInsensitive))
    {
      Data->Link = NewName + QLatin1Char('/') + Data->Link.mid(Prefix.size());
      Result++;
    }
  }
  return Result;
}

Workspaces::PasswordOptions Workspaces::SavePasswordOptions(const StoredSessionList & Sessions, const QList<SessionData> & Tabs)
{
  // TCustomScpExplorerForm::SaveWorkspace
  bool AnyNonStoredSessionWithPassword = false;
  bool AnyNonStoredNonWorkspaceSessionWithPassword = false;
  bool AllNonStoredSessionsAnonymous = true;
  for (const SessionData & Data : Tabs)
  {
    if (FindSame(Sessions, Data) == nullptr)
    {
      if (HasAnySessionPassword(Data))
      {
        AnyNonStoredSessionWithPassword = true;
        if (!Data.IsWorkspace)
        {
          AnyNonStoredNonWorkspaceSessionWithPassword = true;
        }
      }
      if (Data.UserName.compare(AnonymousUserName, Qt::CaseInsensitive) != 0)
      {
        AllNonStoredSessionsAnonymous = false;
      }
    }
  }

  PasswordOptions Result;
  // NixSCP has no master password
  Result.NotRecommended = !AllNonStoredSessionsAnonymous;
  if (AnyNonStoredSessionWithPassword)
  {
    Result.CanSavePasswords = true;
    Result.SavePasswords = !AnyNonStoredNonWorkspaceSessionWithPassword || !Result.NotRecommended;
  }
  return Result;
}

QString Workspaces::LastWorkspace()
{
  return FLastWorkspace;
}

void Workspaces::SetLastWorkspace(const QString & Name)
{
  FLastWorkspace = Name;
}

QString Workspaces::WorkspaceName(const Configuration & Config)
{
  if (!FLastWorkspace.isEmpty())
  {
    return FLastWorkspace;
  }
  if (!Config.AutoWorkspace.isEmpty())
  {
    return Config.AutoWorkspace;
  }
  return NewWorkspaceName();
}

QString Workspaces::ValidateWorkspaceName(const StoredSessionList & Sessions, const QString & Name)
{
  // TSessionData::ValidateName: Name.LastDelimiter(L"/") > 0
  if (Name.lastIndexOf(QLatin1Char('/')) >= 0)
  {
    return QStringLiteral("Invalid name '%1'. Name cannot contain '%2'.").arg(Name, QStringLiteral("/"));
  }
  if (IsFolder(Sessions, Name))
  {
    return QStringLiteral("'%1' is site folder, not workspace.").arg(Name);
  }
  return QString();
}
