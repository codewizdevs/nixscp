// Port of WinSCP source/forms/GenerateUrl.{cpp,dfm} (TGenerateUrlDialog,
// DoGenerateUrlDialog) and of TSessionData::GenerateSessionUrl,
// GetRawSettingsForUrl and GenerateOpenCommandArgs (core/SessionData.cpp).
//
// "Generate session URL/code": URL tab (Username, Password, SSH host key,
// Initial directory, WinSCP-specific, Save extension, Advanced settings) and
// Script tab with the formats Script file (nixscp-cli script), Shell script
// (POSIX sh calling nixscp-cli; Linux replacement of the Batch file) and
// Command-line (nixscp-cli arguments). "Generate file URL" for selected
// files. Copy to Clipboard, Help (ui_generateurl).
// Windows policy: the PowerShell script format and the ".NET assembly code"
// tab (C#, VB.NET, PowerShell) are dropped (no NixSCP .NET assembly).
// Remembered in Configuration\Interface as WinSCP does: GenerateUrlComponents,
// GenerateUrlCodeTarget (0 URL, 1 Script; WinSCP's 2 = assembly reads as URL),
// GenerateUrlScriptFormat (0 Script file, 1 Batch file -> Shell script,
// 2 Command-line; WinSCP's 3 = PowerShell reads as Script file).
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class SessionData;

class GenerateUrlDialog : public QDialog
{
  Q_OBJECT

public:
  // TSessionUrlFlags
  enum
  {
    sufSpecific = 0x01,
    sufUserName = 0x02,
    sufPassword = 0x04,
    sufHostKey = 0x08,
    sufRawSettings = 0x10,
    sufHttpForWebDAV = 0x20,
    sufSession = sufUserName | sufPassword | sufHostKey,
    sufComplete = sufSession | sufRawSettings,
    sufOpen = sufUserName | sufPassword
  };
  // TScriptFormat (Linux: Batch file -> Shell script, no PowerShell)
  enum ScriptFormat { sfScriptFile = 0, sfShellScript = 1, sfCommandLine = 2 };
  // Check box tags (GenerateUrlComponents)
  enum
  {
    gucUserName = 1, gucPassword = 2, gucHostKey = 4, gucRemoteDirectory = 8,
    gucWinSCPSpecific = 16, gucSaveExtension = 32, gucRawSettings = 64
  };

  // DoGenerateUrlDialog: Paths empty = session URL/code, otherwise
  // "Generate file URL" for these remote paths (absolute or relative to
  // the session's RemoteDirectory)
  static void Execute(QWidget * Parent, const SessionData & Data, const QStringList & Paths = QStringList());

  GenerateUrlDialog(QWidget * Parent, const SessionData & Data, const QStringList & Paths = QStringList());
  ~GenerateUrlDialog() override;

  // ---- Code generation (pure functions, tested separately) ----
  // EncodeUrlString / EncodeUrlPath (DoEncodeUrl: letters, digits, _-.* and DoNotEncode kept)
  static QString EncodeUrlString(const QString & S);
  static QString EncodeUrlPath(const QString & S);
  // TSessionData::GetProtocolUrl ("sftp://", "ftpes://", "davs://", "s3plain://"...)
  static QString ProtocolUrl(const SessionData & Data, bool HttpForWebDAV);
  // TSessionData::GetRawSettingsForUrl ("Key=Value" of the settings not in the URL)
  static QStringList RawSettingsForUrl(const SessionData & Data);
  // TSessionData::GenerateSessionUrl
  static QString GenerateSessionUrl(const SessionData & Data, unsigned int Flags);
  // TSessionData::GenerateOpenCommandArgs (script "open" arguments)
  static QString GenerateOpenCommandArgs(const SessionData & Data);
  // nixscp-cli executable for the generated code
  static QString CliExecutable();

  // Current result text and the description label (for tests)
  QString ResultText() const;
  QString Description() const;
  QString ResultCaption() const;
  QTabWidget * PageControl() const;
  QCheckBox * Check(int Tag) const;
  QComboBox * ScriptFormatCombo() const;

  // TGenerateUrlDialog::GenerateUrl / GenerateScript
  QString GenerateUrl() const;
  QString GenerateScript(ScriptFormat Format, QString & Description) const;

  // Execute: restores/stores GenerateUrlComponents, GenerateUrlCodeTarget and GenerateUrlScriptFormat
  void LoadOptions();
  void SaveOptions();

private:
  struct Private;
  Private * d;

  void UpdateControls();
};
