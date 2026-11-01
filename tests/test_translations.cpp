// Translations: WinSCP's translations converted by tools/build_translations.py
// (resources/translations/<code>.json) and src/gui/Translator: captions with
// and without mnemonics, form contexts, message templates with arguments, the
// LCID mapping (LocaleSafe) and the live translation of widgets.
// NIXSCP_GUI_TESTS
#include "gui/Translator.h"
#include "gui/GuiUserInterface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { QString _A = (A); QString _B = (B); if (_A != _B) { std::printf("FAIL %s:%d: %s\n  got:      %s\n  expected: %s\n", \
    __FILE__, __LINE__, #A, qPrintable(_A), qPrintable(_B)); Failures++; } } while (0)

static QString U(const char * Text)
{
  return QString::fromUtf8(Text);
}

static void TestResources()
{
  CHECK(QFile::exists(QStringLiteral(":/translations/index.json")));
  CHECK(QFile::exists(QStringLiteral(":/translations/srl.json")));
  CHECK(QFile::exists(QStringLiteral(":/translations/de.json")));
  int Complete = 0;
  for (const Translator::LanguageInfo & Info : Translator::Languages())
  {
    CHECK(Info.Completeness >= 0);   // every language has its JSON built in
    Complete += (Info.Completeness >= 90) ? 1 : 0;
  }
  CHECK(Translator::Languages().size() == 55);
  CHECK(Complete >= 30);
}

static void TestLcids()
{
  // round trip for all languages
  for (const Translator::LanguageInfo & Info : Translator::Languages())
  {
    CHECK(Translator::CodeToLcid(Info.Code) == Info.Lcid);
    CHECK(Translator::LcidToCode(Info.Lcid) == Info.Code);
    CHECK(Translator::FindLanguage(Info.Lcid) == &Info);
    CHECK(Translator::FindLanguage(Info.Locale) == &Info);
  }
  CHECK(Translator::CodeToLcid(QStringLiteral("SRL")) == 0x081A);
  CHECK(Translator::CodeToLcid(QStringLiteral("srl")) == 2074);
  CHECK(Translator::CodeToLcid(QStringLiteral("DE")) == 0x0407);
  CHECK(Translator::CodeToLcid(QStringLiteral("CHS")) == 0x0804);
  CHECK(Translator::CodeToLcid(QStringLiteral("XXK")) == 0xFFFFFF4B);
  CHECK(Translator::CodeToLcid(QStringLiteral("EN")) == Translator::InternalLcid);
  CHECK(Translator::CodeToLcid(QStringLiteral("nonsense")) == 0);
  CHECK(Translator::LcidToCode(0x0416) == QStringLiteral("PT"));
  CHECK(Translator::LcidToCode(0x0816) == QStringLiteral("PTG"));
  CHECK(Translator::LcidToCode(Translator::InternalLcid).isEmpty());
  // as stored in WinSCP.ini (LocaleSafe is a signed integer)
  CHECK(Translator::FindLanguage(static_cast<unsigned int>(-187))->Code == QStringLiteral("XXE"));
  // sublanguage without own translation: German (Austria) -> German
  CHECK(Translator::FindLanguage(0x0C07u) != nullptr);
  CHECK(Translator::FindLanguage(0x0C07u)->Code == QStringLiteral("DE"));
  // never Traditional Chinese for Chinese (Singapore)
  CHECK(Translator::FindLanguage(0x1004u) == nullptr);
  CHECK(Translator::FindLanguage(QStringLiteral("sr_Latn"))->Code == QStringLiteral("SRL"));
  CHECK(Translator::DisplayName(0x081A) == QStringLiteral("Serbian (Latin) - srpski (latinica)"));
  CHECK(Translator::DisplayName(Translator::InternalLcid) == QStringLiteral("English - English"));
}

static void TestSerbian()
{
  CHECK(Translator::LoadLanguage(QStringLiteral("SRL")));
  CHECK(Translator::AppliedCode() == QStringLiteral("SRL"));
  CHECK(Translator::AppliedLcid() == 0x081A);
  // forms
  CHECK_EQ(Translator::Tr(QStringLiteral("Preferences")), U("Podešavanja"));
  CHECK_EQ(Translator::Tr(QStringLiteral("OK")), QStringLiteral("U redu"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Cancel")), U("Otkaži"));
  CHECK_EQ(Translator::Tr(QStringLiteral("&Help")), U("&Pomoć"));
  // mnemonic-free variants, Qt's standard button texts
  CHECK_EQ(Translator::Tr(QStringLiteral("Help")), U("Pomoć"));
  CHECK_EQ(Translator::Tr(QStringLiteral("&Yes")), QStringLiteral("&Da"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Yes")), QStringLiteral("Da"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Languages")), QStringLiteral("Jezici"));
  // trailing ":" / "..." normalization
  CHECK_EQ(Translator::Tr(QStringLiteral("&Hostname:")), U("&Ime računara"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Hostname")), U("Ime računara"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Preferences...")), U("Podešavanja..."));
  CHECK_EQ(Translator::Tr(QStringLiteral("Languages:")), QStringLiteral("Jezici:"));
  // form context: Login's "Save" opens a menu
  CHECK_EQ(Translator::Tr(QStringLiteral("&Save"), "Login"), U("&Sačuvaj..."));
  CHECK_EQ(Translator::Tr(QStringLiteral("&Save")), U("&Sačuvaj"));
  // file panel (Delphi resource strings)
  CHECK_EQ(Translator::Tr(QStringLiteral("Parent directory")), U("Matični direktorijum"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Date modified")), QStringLiteral("Datum izmene"));
  // untranslated
  CHECK_EQ(Translator::Tr(QStringLiteral("No such caption in WinSCP")), QStringLiteral("No such caption in WinSCP"));
  CHECK_EQ(Translator::Tr(QString()), QString());

  // .rc templates
  CHECK_EQ(Translator::TrMessage(QStringLiteral("Error listing directory '/home/user/some dir'.")),
    U("Greška pri izlistavanju direktorijuma '/home/user/some dir'."));
  CHECK_EQ(Translator::TrMessage(QStringLiteral("Command 'ls -la'\nfailed with return code 127 and the following error message.")),
    U("Komanda 'ls -la'\nnije uspela sa povratnim kodom 127 i sledećom porukom o grešci."));
  CHECK_EQ(Translator::TrMessage(QStringLiteral("0 B of 1,234 KB in 0 of 15")),
    QStringLiteral("0 B od 1,234 KB u 0 od 15"));
  CHECK_EQ(Translator::TrMessage(QStringLiteral("12 hidden")), QStringLiteral("12 skrivenih"));
  // paragraphs translated separately (message + details)
  CHECK_EQ(Translator::TrMessage(QStringLiteral("Error listing directory '/tmp'.\n\nPermission denied.")),
    Translator::TrMessage(QStringLiteral("Error listing directory '/tmp'.")) + QStringLiteral("\n\n") +
      Translator::TrMessage(QStringLiteral("Permission denied.")));
  CHECK(Translator::TrMessage(QStringLiteral("Permission denied.")) != QStringLiteral("Permission denied."));
  // unknown text with arguments stays
  CHECK_EQ(Translator::TrMessage(QStringLiteral("Something 'x' happened.")), QStringLiteral("Something 'x' happened."));
}

static void TestGerman()
{
  CHECK(Translator::LoadLanguage(QStringLiteral("DE")));
  CHECK_EQ(Translator::Tr(QStringLiteral("Preferences")), QStringLiteral("Einstellungen"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Cancel")), QStringLiteral("Abbrechen"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Cancel"), "Login"), QStringLiteral("&Abbrechen"));
  CHECK_EQ(Translator::Tr(QStringLiteral("OK")), QStringLiteral("OK"));
  CHECK_EQ(Translator::Tr(QStringLiteral("&Help")), QStringLiteral("&Hilfe"));
  CHECK_EQ(Translator::Tr(QStringLiteral("Size")), U("Größe"));
  // localized quotes in the template
  CHECK_EQ(Translator::TrMessage(QStringLiteral("Error listing directory '/srv'.")),
    U("Fehler beim Anzeigen des Verzeichnisses „/srv“."));
  CHECK_EQ(Translator::TrMessage(QStringLiteral("5 hidden")), QStringLiteral("5 versteckt"));
  CHECK(Translator::LoadLanguage(QString()));
  CHECK_EQ(Translator::Tr(QStringLiteral("Preferences")), QStringLiteral("Preferences"));
}

static void TestWidgets()
{
  QDialog Dialog;
  Dialog.setWindowTitle(QStringLiteral("Preferences"));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Group = new QGroupBox(QStringLiteral("Languages"));
  auto * GroupLayout = new QVBoxLayout(Group);
  auto * Label = new QLabel(QStringLiteral("&Hostname:"));
  GroupLayout->addWidget(Label);
  auto * Check = new QCheckBox(QStringLiteral("Not a WinSCP caption"));
  Check->setToolTip(QStringLiteral("Preferences"));
  GroupLayout->addWidget(Check);
  Layout->addWidget(Group);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help);
  Layout->addWidget(Buttons);
  // a tool button showing its action with an own icon text (Login's Advanced...)
  auto * IconAction = new QAction(QStringLiteral("A&dvanced..."), &Dialog);
  IconAction->setIconText(IconAction->text());
  auto * Tool = new QToolButton();
  Tool->setDefaultAction(IconAction);
  Tool->setToolButtonStyle(Qt::ToolButtonTextOnly);
  Layout->addWidget(Tool);
  QMenu Menu(&Dialog);
  QAction * Action = Menu.addAction(QStringLiteral("&Preferences..."));
  Action->setStatusTip(QStringLiteral("Cancel"));

  // English: untouched
  CHECK(Translator::SetLanguage(Translator::InternalLcid));
  Dialog.show();
  QApplication::processEvents();
  CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Preferences"));
  CHECK_EQ(Buttons->button(QDialogButtonBox::Cancel)->text(), QStringLiteral("Cancel"));

  // live switch to Serbian
  CHECK(Translator::SetLanguage(0x081A));
  QApplication::processEvents();
  CHECK_EQ(Dialog.windowTitle(), U("Podešavanja"));
  CHECK_EQ(Group->title(), QStringLiteral("Jezici"));
  CHECK_EQ(Label->text(), U("&Ime računara"));
  CHECK_EQ(Check->text(), QStringLiteral("Not a WinSCP caption"));
  CHECK_EQ(Check->toolTip(), U("Podešavanja"));
  CHECK_EQ(Buttons->button(QDialogButtonBox::Ok)->text(), QStringLiteral("U redu"));
  CHECK_EQ(Buttons->button(QDialogButtonBox::Cancel)->text(), U("Otkaži"));
  CHECK_EQ(Buttons->button(QDialogButtonBox::Help)->text(), U("Pomoć"));
  CHECK_EQ(Action->text(), U("&Podešavanja..."));
  CHECK_EQ(Action->statusTip(), U("Otkaži"));
  CHECK(IconAction->iconText() != QStringLiteral("A&dvanced..."));
  CHECK_EQ(IconAction->iconText(), Translator::Tr(QStringLiteral("A&dvanced...")));
  CHECK_EQ(Tool->text(), IconAction->iconText());

  // widgets created later are translated when shown; the application
  // changing a text is noticed
  auto * Later = new QPushButton(QStringLiteral("&Help"));
  Layout->addWidget(Later);
  Later->show();
  QApplication::processEvents();
  CHECK_EQ(Later->text(), U("&Pomoć"));
  Label->setText(QStringLiteral("Languages"));
  Translator::TranslateWidget(Label);
  CHECK_EQ(Label->text(), QStringLiteral("Jezici"));
  // changed by the application while shown: translated before painted
  Group->setTitle(QStringLiteral("Preferences"));
  Group->repaint();
  CHECK_EQ(Group->title(), U("Podešavanja"));
  Group->setTitle(QStringLiteral("Languages"));
  Group->repaint();
  CHECK_EQ(Group->title(), QStringLiteral("Jezici"));
  QAction * Added = Menu.addAction(QStringLiteral("Cancel"));
  QApplication::processEvents();
  CHECK_EQ(Added->text(), U("Otkaži"));

  // to German and back to English, from the remembered originals
  CHECK(Translator::SetLanguage(0x0407));
  QApplication::processEvents();
  CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Einstellungen"));
  CHECK_EQ(Group->title(), QStringLiteral("Sprachen"));
  CHECK_EQ(Later->text(), QStringLiteral("&Hilfe"));
  CHECK_EQ(Action->statusTip(), QStringLiteral("Abbrechen"));
  CHECK(Translator::SetLanguage(Translator::InternalLcid));
  QApplication::processEvents();
  CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Preferences"));
  CHECK_EQ(Group->title(), QStringLiteral("Languages"));
  CHECK_EQ(Label->text(), QStringLiteral("Languages"));
  CHECK_EQ(Action->text(), QStringLiteral("&Preferences..."));
  CHECK_EQ(Buttons->button(QDialogButtonBox::Cancel)->text(), QStringLiteral("Cancel"));
  CHECK_EQ(Later->text(), QStringLiteral("&Help"));
  CHECK_EQ(IconAction->iconText(), QStringLiteral("A&dvanced..."));
  CHECK(Translator::AppliedLcid() == Translator::InternalLcid);
}

static void TestMessageForm()
{
  // the central message path translates WinSCP's message templates
  CHECK(Translator::SetLanguage(0x081A));
  {
    MessageForm Form(nullptr, QStringLiteral("Error listing directory '/srv/data'."), QStringLiteral("Permission denied."),
      qtError, qaOK, QString(), nullptr);
    QString Text = Form.FormText();
    CHECK(Text.contains(U("Greška pri izlistavanju direktorijuma '/srv/data'.")));
    CHECK(Text.contains(Translator::TrMessage(QStringLiteral("Permission denied."))));
    CHECK(!Text.contains(QStringLiteral("Error listing directory")));
  }
  CHECK(Translator::SetLanguage(Translator::InternalLcid));
}

int main(int argc, char * argv[])
{
  QApplication App(argc, argv);
  TestResources();
  TestLcids();
  TestSerbian();
  TestGerman();
  TestWidgets();
  TestMessageForm();
  if (Failures == 0)
  {
    std::printf("OK (0 failures)\n");
  }
  else
  {
    std::printf("%d failure(s)\n", Failures);
  }
  return (Failures == 0) ? 0 : 1;
}
