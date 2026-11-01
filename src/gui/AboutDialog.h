// Port of WinSCP source/forms/About.{cpp,dfm} (TAboutDialog, DoAboutDialog)
// and source/forms/License.{cpp,dfm} (TLicenseDialog, DoLicenseDialog).
//
// Application name and version, the WinSCP copyright the port is based on,
// homepage and support forum links, the "Portions copyright" list with the
// run-time versions of Qt (qVersion), libssh (ssh_version) and OpenSSL
// (OpenSSL_version), a License... button showing the full GPL text, Help
// (ui_about) and Ctrl+C copying the whole text (TAboutDialog::FormKeyDown).
// Windows-only parts dropped: the Microsoft Store registration box, the
// translation credits (no WinSCP translations) and the PuTTY, FileZilla,
// neon, libs3, Expat, Toolbar2000, TBX, Filemanager, JCL and PngComponents
// credits (none of that code is used by NixSCP).
#pragma once

#include <QDialog>
#include <QString>

class QPushButton;
class QTextBrowser;

class AboutDialog : public QDialog
{
  Q_OBJECT

public:
  explicit AboutDialog(QWidget * Parent = nullptr);

  // DoAboutDialog
  static void Execute(QWidget * Parent);
  // DoProductLicense / DoLicenseDialog(lcWinScp): the GPL text
  static void ShowLicense(QWidget * Parent);
  // Text of the product license (resources/license/license.txt)
  static QString LicenseText();

  // "Version 0.1.0, Based on WinSCP" (ABOUT_BASED_ON_PRODUCT)
  static QString VersionText();
  // GenerateThirdParty(Html)
  static QString ThirdPartyText(bool Html);
  // FormKeyDown Ctrl+C text
  QString ClipboardText() const;

  QPushButton * LicenseButton() const { return FLicenseButton; }
  QTextBrowser * ThirdPartyBrowser() const { return FThirdPartyBrowser; }

protected:
  void keyPressEvent(QKeyEvent * Event) override;

private:
  QPushButton * FLicenseButton = nullptr;
  QTextBrowser * FThirdPartyBrowser = nullptr;
};
