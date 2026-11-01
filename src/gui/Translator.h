// User interface translations: WinSCP's translations (winscp-master/translations,
// converted by tools/build_translations.py to resources/translations/<code>.json)
// keyed by WinSCP's English texts, which NixSCP's widgets carry.
//
// windows/GUIConfiguration.cpp (Locale, LocaleSafe, Locales, AddLocale,
// LoadNewResourceModule) and the translation part of the VCL resource
// modules. Unlike WinSCP, the language can change while running: the
// application-wide event filter translates widgets as they are polished /
// shown and retranslates all of them when the language changes (the English
// originals are remembered).
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QAction;
class QWidget;

class Translator : public QObject
{
  Q_OBJECT

public:
  // TLocaleInfo
  struct LanguageInfo
  {
    QString Code;            // WinSCP translation code: "SRL", "DE", "CHS"...
    QString Locale;          // BCP-47: "sr-Latn", "de", "zh-Hans"
    unsigned int Lcid = 0;   // Windows LCID (LocaleSafe in WinSCP.ini)
    QString EnglishName;     // "Serbian (Latin)"
    QString NativeName;      // "srpski (latinica)"
    int Completeness = 0;    // TRANSLATION_COMPLETENESS (percent)
    bool Rtl = false;        // BIDI_MODE
  };

  // English (United States), the language of NixSCP's own texts
  static constexpr unsigned int InternalLcid = 0x0409;
  // WinSCP's AdditionalLanguageMask (translations XX<letter>)
  static constexpr unsigned int AdditionalLanguageMask = 0xFFFFFF00;

  static Translator * Instance();

  // All translations WinSCP has (built in), sorted by code; English not included
  static const QList<LanguageInfo> & Languages();
  static const LanguageInfo * FindLanguage(unsigned int Lcid);
  static const LanguageInfo * FindLanguage(const QString & CodeOrLocale);
  static unsigned int CodeToLcid(const QString & Code);   // 0 when unknown
  static QString LcidToCode(unsigned int Lcid);           // empty for English / unknown
  // "English name - native name" (TGUIConfiguration::AddLocale)
  static QString DisplayName(unsigned int Lcid);

  // TWinConfiguration::DefaultLocale: the translation for the system locale
  // (QLocale::system()), InternalLcid when there is none
  static unsigned int DefaultLcid();
  // The language in use (InternalLcid = English)
  static unsigned int AppliedLcid();
  static QString AppliedCode();

  // Switches the language (0 = DefaultLcid(), InternalLcid = English) and
  // retranslates all existing widgets; true when the language is in use
  static bool SetLanguage(unsigned int Lcid);
  // Loads a translation by its code (tests); empty code = English
  static bool LoadLanguage(const QString & Code);

  // Translation of an English text (a WinSCP caption, hint or message);
  // Context: WinSCP form name ("Login", "Preferences"...) for form specific
  // translations. Matches with and without "&" mnemonics and with trailing
  // ":" / "..." normalized. English when there is no translation.
  static QString Tr(const QString & English, const char * Context = nullptr);
  static QString Tr(const QString & English, const QString & Context);
  // Translation of a composed message: exact texts, and WinSCP's format
  // strings ("Error listing directory '%s'.") matched as templates, rebuilt
  // with the localized template and the original arguments; paragraphs and
  // lines are translated separately when the whole message is not known
  static QString TrMessage(const QString & Message);

  // WinSCP form name used as translation context for a widget's window
  // ("Login" for LoginDialog...); empty when unknown
  static QString FormContext(const QWidget * Widget);

  // Translates a widget tree / an action now (normally done by the event filter)
  static void TranslateWidget(QWidget * Widget);
  static void TranslateAction(QAction * Action);

signals:
  // After SetLanguage / LoadLanguage: models refresh their texts
  void LanguageChanged();

protected:
  bool eventFilter(QObject * Object, QEvent * Event) override;

private:
  struct Template;

  Translator();
  ~Translator() override;

  bool Load(const QString & Code);
  void InstallFilter();
  void RetranslateAll();
  QString Lookup(const QString & Text, const QStringList & Contexts) const;
  QString Translate(const QString & Text, const QStringList & Contexts, bool Message) const;
  QString TranslateBase(const QString & Text, const QStringList & Contexts) const;
  QString TranslateMessage(const QString & Message, int Depth) const;
  bool MatchTemplate(const QString & Message, QString & Result) const;
  void BuildTemplates() const;
  QString Apply(QObject * Object, const char * Key, const QString & Current, const QStringList & Contexts, bool Message = false);
  QString ApplyItem(const QString & Current, const QStringList & Contexts);
  QStringList ContextsOf(const QWidget * Widget) const;
  void DoTranslateWidget(QWidget * Widget);
  void DoTranslateAction(QAction * Action, const QStringList & Contexts);

  QString FCode;
  unsigned int FLcid = InternalLcid;
  QHash<QString, QString> FStrings;
  QHash<QString, QHash<QString, QString>> FContexts;
  // localized -> English of texts produced for the current / previous languages
  QHash<QString, QString> FReverse;
  QHash<QString, QString> FPreviousReverse;
  mutable QList<Template *> FTemplates;
  mutable bool FTemplatesBuilt = false;
  bool FFilterInstalled = false;
  bool FRetranslatePending = false;
  bool FApplying = false;
  bool FForce = false;
  QObject * FQtTranslator = nullptr;
};
