// "Agent Access..." of the site editor (core/AgentAccess.h): whether programs
// such as AI agents may use the site through NixSCP, what they may do, and
// whether NixSCP asks first.
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class SessionData;

class AgentAccessDialog : public QDialog
{
  Q_OBJECT

public:
  AgentAccessDialog(SessionData & Data, QWidget * Parent = nullptr);
  void accept() override;

  // Short state for the site editor ("Off", "Allowed", "Asks first")
  static QString Summary(const SessionData & Data);
  // Commands > Agent Access Log... / Agent Access Help
  static void ShowLog(QWidget * Window);
  static void ShowHelp(QWidget * Window);
  // Commands > Agent Sessions...: the recorded transcripts
  static void ShowSessions(QWidget * Window);

private:
  SessionData & FData;
  QComboBox * FAccess;
  QLineEdit * FAgentName;
  QCheckBox * FCommands;
  QCheckBox * FTransfer;
  QCheckBox * FUpload;
  QCheckBox * FTunnels;
  QCheckBox * FRecord;
  QPlainTextEdit * FAllow;
  QPlainTextEdit * FDeny;
  QLabel * FWarning;
  int FOtherPermissions = 0;

  void UpdateControls();
};
