// Port of WinSCP source/forms/SiteAdvanced.{cpp,dfm} ("Advanced Site Settings")
// Pages: Environment (Directories, Recycle bin, SFTP, SCP/Shell, FTP, S3,
// WebDAV), Connection (Proxy, Tunnel, TLS/SSL), SSH (Key exchange,
// Authentication, Bugs), Note. Pages not applicable to Data.FSProtocol are
// not shown, as in WinSCP. Encryption page and PuTTY group are n/a.
// Also TSiteRawDialog of forms/Custom.cpp ("Raw Site Settings").
#pragma once

#include "core/SessionData.h"

#include <QDialog>

#include <functional>

class QPlainTextEdit;
class QMenu;

class SiteAdvancedDialog : public QDialog
{
  Q_OBJECT

public:
  SiteAdvancedDialog(SessionData & Data, QWidget * Parent = nullptr);
  ~SiteAdvancedDialog() override;
  void accept() override;

  // Selects page by its (untranslated) caption, e.g. "Proxy"
  void SelectPage(const QString & Caption);

  // Session color menu (CreateSessionColorMenu: Default, recent custom
  // colors, standard session colors, More Colors...); Color is a BGR TColor.
  // Custom colors are remembered in Configuration\Interface\SessionColors.
  static void PopulateSessionColorMenu(QMenu * Menu, int Color, const std::function<void(int)> & OnColorChange);

private:
  struct Private;
  Private * d;
  SessionData & FData;

  void LoadSession();
  void SaveSession(SessionData & Data);
  void UpdateControls();
  void UpdateNavigationTree();
  void CyclePage(bool Forward);
  void PrivateKeyViewButtonClick();
  void PrivateKeyUploadItemClick();
};

// TSiteRawDialog: all non-default site settings as "Key=Value" lines
// (WinSCP.ini key names); the basic Login dialog settings are kept
class SiteRawDialog : public QDialog
{
  Q_OBJECT

public:
  SiteRawDialog(SessionData & Data, QWidget * Parent = nullptr);
  void accept() override;

private:
  SessionData & FData;
  QPlainTextEdit * SettingsMemo;

  QStringList Settings() const;
  void AddButtonClick();
};
