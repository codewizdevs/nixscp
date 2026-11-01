// Port of WinSCP source/forms/About.{cpp,dfm} and source/forms/License.{cpp,dfm}
#include "gui/AboutDialog.h"
#include "gui/HelpSystem.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <libssh/libssh.h>
#include <openssl/crypto.h>

#include <algorithm>

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.0.0"
#endif

static QString AppName()
{
  return QStringLiteral("NixSCP");
}

// WINSCP_COPYRIGHT
static const QString WinSCPCopyright = QStringLiteral("Copyright © 2000–2026 Martin Prikryl");
// NixSCP itself: Linux rewrite by Dragan Plavšić (CodeWiz, Banja Luka)
static const QString NixSCPCopyright = QStringLiteral("Copyright © 2026 Dragan Plavšić, CodeWiz");
static const QString ContactEmail = QStringLiteral("hello@codewizdev.com");

static QString Link(bool Html, const QString & Url, const QString & Title = QString())
{
  // TAboutDialog::CreateLink
  if (Html)
  {
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(Url.toHtmlEscaped(), (Title.isEmpty() ? Url : Title).toHtmlEscaped());
  }
  return Url;
}

static void AddPara(bool Html, QString & Text, const QString & S)
{
  if (Html)
  {
    Text += QStringLiteral("<p>") + S + QStringLiteral("</p>\n");
  }
  else
  {
    Text += S + QStringLiteral("\n\n");
  }
}

QString AboutDialog::VersionText()
{
  QString Version = QApplication::applicationVersion();
  if (Version.isEmpty())
  {
    Version = QStringLiteral(PROJECT_VERSION);
  }
  // VersionStr (VERSION2 "Version %s") + ABOUT_BASED_ON_PRODUCT "%s, Based on %s version %s";
  // the WinSCP version the sources come from is not tracked, so the product only
  return QStringLiteral("Version %1, Based on WinSCP").arg(Version);
}

QString AboutDialog::ThirdPartyText(bool Html)
{
  QString Br = Html ? QStringLiteral("<br/>\n") : QStringLiteral("\n");
  auto Esc = [Html](const QString & S) { return Html ? S.toHtmlEscaped() : S; };
  QString Result;

  // ABOUT_THIRDPARTY_HEADER
  AddPara(Html, Result, Esc(QStringLiteral("License agreements of all following programs (libraries) are part of application license agreement.")));

  AddPara(Html, Result,
    Esc(QStringLiteral("Based on WinSCP")) + Br +
    Esc(WinSCPCopyright) + Br +
    Link(Html, HelpSystem::WinSCPHomepageUrl().toString()));

  AddPara(Html, Result,
    Esc(QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()))) + Br +
    Esc(QStringLiteral("Copyright © The Qt Company Ltd. and other contributors")) + Br +
    Link(Html, QStringLiteral("https://www.qt.io/")));

  // PUTTY_BASED_ON-like line for the SSH implementation
  AddPara(Html, Result,
    Esc(QStringLiteral("SSH and SCP code based on libssh %1").arg(QString::fromLatin1(ssh_version(0)))) + Br +
    Esc(QStringLiteral("Copyright © Aris Adamantiadis, Andreas Schneider and others")) + Br +
    Link(Html, QStringLiteral("https://www.libssh.org/")));

  // "OpenSSL 3.0.13 30 Jan 2024": OPENSSL_BASED_ON + OPENSSL_COPYRIGHT2
  QStringList Parts = QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
  QString OpenSSLVersion = Parts.value(1);
  QString OpenSSLYear = Parts.value(4);
  AddPara(Html, Result,
    Esc(QStringLiteral("This software includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit %1.").arg(OpenSSLVersion)) + Br +
    Esc(QStringLiteral("Copyright © 1998–%1 The OpenSSL Project").arg(OpenSSLYear)) + Br +
    Link(Html, QStringLiteral("https://www.openssl.org/")));

  return Result;
}

AboutDialog::AboutDialog(QWidget * Parent) :
  QDialog(Parent)
{
  setWindowTitle(QStringLiteral("About %1").arg(AppName()));   // ABOUT_CAPTION "About %s"

  QFrame * Panel = new QFrame(this);
  Panel->setAutoFillBackground(true);
  QPalette PanelPalette = Panel->palette();
  PanelPalette.setColor(QPalette::Window, PanelPalette.color(QPalette::Base));
  Panel->setPalette(PanelPalette);

  QLabel * IconLabel = new QLabel(Panel);
  QIcon Icon = QApplication::windowIcon();
  if (Icon.isNull())
  {
    Icon = QIcon::fromTheme(QStringLiteral("nixscp"), style()->standardIcon(QStyle::SP_ComputerIcon));
  }
  IconLabel->setPixmap(Icon.pixmap(48, 48));
  IconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

  QLabel * ApplicationLabel = new QLabel(AppName(), Panel);
  ApplicationLabel->setObjectName(QStringLiteral("ApplicationLabel"));
  QFont BoldFont = ApplicationLabel->font();
  BoldFont.setBold(true);
  BoldFont.setPointSizeF(BoldFont.pointSizeF() * 1.25);
  ApplicationLabel->setFont(BoldFont);
  QLabel * VersionLabel = new QLabel(VersionText(), Panel);
  VersionLabel->setObjectName(QStringLiteral("VersionLabel"));
  VersionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QLabel * CopyrightLabel = new QLabel(NixSCPCopyright, Panel);
  CopyrightLabel->setObjectName(QStringLiteral("WinSCPCopyrightLabel"));
  QLabel * HomepageLabel = new QLabel(Link(true, HelpSystem::HomepageUrl().toString()), Panel);
  HomepageLabel->setObjectName(QStringLiteral("HomepageLabel"));
  HomepageLabel->setOpenExternalLinks(true);
  QLabel * MessageLabel = new QLabel(QStringLiteral("To send comments and report bugs write to:"), Panel);
  MessageLabel->setObjectName(QStringLiteral("ProductSpecificMessageLabel"));
  QLabel * ForumLabel = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(HelpSystem::ContactUrl().toString(), ContactEmail), Panel);
  ForumLabel->setObjectName(QStringLiteral("ForumUrlLabel"));
  ForumLabel->setOpenExternalLinks(true);
  QLabel * PortionsLabel = new QLabel(QStringLiteral("Portions copyright:"), Panel);

  FThirdPartyBrowser = new QTextBrowser(Panel);
  FThirdPartyBrowser->setObjectName(QStringLiteral("ThirdPartyPanel"));
  FThirdPartyBrowser->setOpenExternalLinks(true);
  FThirdPartyBrowser->setHtml(ThirdPartyText(true));
  FThirdPartyBrowser->setMinimumHeight(150);

  QVBoxLayout * TextLayout = new QVBoxLayout();
  TextLayout->setSpacing(2);
  TextLayout->addWidget(ApplicationLabel);
  TextLayout->addWidget(VersionLabel);
  TextLayout->addSpacing(10);
  TextLayout->addWidget(CopyrightLabel);
  TextLayout->addWidget(HomepageLabel);
  TextLayout->addSpacing(10);
  TextLayout->addWidget(MessageLabel);
  TextLayout->addWidget(ForumLabel);
  TextLayout->addSpacing(10);
  TextLayout->addWidget(PortionsLabel);
  TextLayout->addWidget(FThirdPartyBrowser, 1);

  QHBoxLayout * PanelLayout = new QHBoxLayout(Panel);
  PanelLayout->setContentsMargins(12, 12, 12, 12);
  PanelLayout->addWidget(IconLabel, 0, Qt::AlignTop);
  PanelLayout->addLayout(TextLayout, 1);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  FLicenseButton = Buttons->addButton(QStringLiteral("&License..."), QDialogButtonBox::ActionRole);
  FLicenseButton->setAutoDefault(false);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_about"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(FLicenseButton, &QPushButton::clicked, this, [this]() { ShowLicense(this); });

  QVBoxLayout * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 8);
  Layout->addWidget(Panel, 1);
  QHBoxLayout * ButtonLayout = new QHBoxLayout();
  ButtonLayout->setContentsMargins(8, 0, 8, 0);
  ButtonLayout->addWidget(Buttons);
  Layout->addLayout(ButtonLayout);
  resize(420, 470);
}

QString AboutDialog::ClipboardText() const
{
  // TAboutDialog::FormKeyDown
  QString Text =
    AppName() + QLatin1Char('\n') +
    VersionText() + QLatin1Char('\n') +
    QLatin1Char('\n') +
    NixSCPCopyright + QLatin1Char('\n') +
    HelpSystem::HomepageUrl().toString() + QLatin1Char('\n') +
    QLatin1Char('\n') +
    QStringLiteral("To send comments and report bugs write to:") + QLatin1Char('\n') +
    ContactEmail + QLatin1Char('\n');
  Text +=
    QLatin1Char('\n') +
    QString(40, QLatin1Char('-')) + QLatin1Char('\n') +
    QStringLiteral("Portions copyright:") + QLatin1Char('\n') +
    ThirdPartyText(false).trimmed() + QLatin1Char('\n');
  return Text;
}

void AboutDialog::keyPressEvent(QKeyEvent * Event)
{
  if ((Event->key() == Qt::Key_C) && (Event->modifiers() & Qt::ControlModifier) &&
      !(FThirdPartyBrowser->hasFocus() && FThirdPartyBrowser->textCursor().hasSelection()))
  {
    QApplication::clipboard()->setText(ClipboardText());
    Event->accept();
    return;
  }
  QDialog::keyPressEvent(Event);
}

void AboutDialog::Execute(QWidget * Parent)
{
  AboutDialog Dialog(Parent);
  Dialog.exec();
}

QString AboutDialog::LicenseText()
{
  QFile File(QStringLiteral(":/license/license.txt"));
  if (File.open(QIODevice::ReadOnly))
  {
    return QString::fromUtf8(File.readAll());
  }
  return QStringLiteral("GNU General Public License, version 3\nhttps://www.gnu.org/licenses/gpl-3.0.html\n");
}

void AboutDialog::ShowLicense(QWidget * Parent)
{
  // TLicenseDialog: "License", read-only memo, Close
  QDialog Dialog(Parent);
  Dialog.setObjectName(QStringLiteral("LicenseDialog"));
  Dialog.setWindowTitle(QStringLiteral("License"));
  Dialog.setSizeGripEnabled(true);
  QPlainTextEdit * Memo = new QPlainTextEdit(&Dialog);
  Memo->setObjectName(QStringLiteral("LicenseMemo"));
  Memo->setReadOnly(true);
  Memo->setLineWrapMode(QPlainTextEdit::NoWrap);
  Memo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  Memo->setPlainText(LicenseText());
  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  QVBoxLayout * Layout = new QVBoxLayout(&Dialog);
  Layout->addWidget(Memo, 1);
  Layout->addWidget(Buttons);
  int Width = Memo->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('M'))) + 60;
  Dialog.resize(std::min(Width, 760), 520);
  Dialog.exec();
}
