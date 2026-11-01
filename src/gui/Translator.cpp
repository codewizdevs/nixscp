#include "gui/Translator.h"

#include <QAbstractButton>
#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QTreeWidget>

#include <QSet>

#include <algorithm>
#include <cxxabi.h>
#include <functional>
#include <cstdlib>
#include <typeinfo>

//---------------------------------------------------------------------------
// WinSCP translation codes (file extensions of the translations, derived from
// LOCALE_SABBREVLANGNAME) and the LCIDs WinSCP stores in LocaleSafe.
// Keep in sync with LANGUAGES in tools/build_translations.py.
namespace
{
struct LanguageEntry
{
  const char * Code;
  const char * Locale;
  unsigned int Lcid;
  const char * EnglishName;
  const char * NativeName;
};
}

static const LanguageEntry LanguageTable[] =
{
  { "AF", "af", 0x0436, "Afrikaans", "Afrikaans" },
  { "AR", "ar", 0x0401, "Arabic", "العربية" },
  { "BE", "be", 0x0423, "Belarusian", "беларуская" },
  { "BG", "bg", 0x0402, "Bulgarian", "български" },
  { "BN", "bn", 0x0445, "Bangla", "বাংলা" },
  { "CA", "ca", 0x0403, "Catalan", "català" },
  { "CH", "zh-Hant", 0x0404, "Chinese (Traditional)", "中文(繁體)" },
  { "CHS", "zh-Hans", 0x0804, "Chinese (Simplified)", "中文(简体)" },
  { "CS", "cs", 0x0405, "Czech", "čeština" },
  { "DA", "da", 0x0406, "Danish", "dansk" },
  { "DE", "de", 0x0407, "German", "Deutsch" },
  { "EL", "el", 0x0408, "Greek", "Ελληνικά" },
  { "ES", "es", 0x0C0A, "Spanish", "español" },
  { "ET", "et", 0x0425, "Estonian", "eesti" },
  { "FA", "fa", 0x0429, "Persian", "فارسی" },
  { "FI", "fi", 0x040B, "Finnish", "suomi" },
  { "FP", "fil", 0x0464, "Filipino", "Filipino" },
  { "FR", "fr", 0x040C, "French", "français" },
  { "GL", "gl", 0x0456, "Galician", "galego" },
  { "HE", "he", 0x040D, "Hebrew", "עברית" },
  { "HI", "hi", 0x0439, "Hindi", "हिन्दी" },
  { "HR", "hr", 0x041A, "Croatian", "hrvatski" },
  { "HU", "hu", 0x040E, "Hungarian", "magyar" },
  { "HY", "hy", 0x042B, "Armenian", "հայերեն" },
  { "IN", "id", 0x0421, "Indonesian", "Indonesia" },
  { "IS", "is", 0x040F, "Icelandic", "íslenska" },
  { "IT", "it", 0x0410, "Italian", "italiano" },
  { "JP", "ja", 0x0411, "Japanese", "日本語" },
  { "KA", "ka", 0x0437, "Georgian", "ქართული" },
  { "KO", "ko", 0x0412, "Korean", "한국어" },
  { "LT", "lt", 0x0427, "Lithuanian", "lietuvių" },
  { "LV", "lv", 0x0426, "Latvian", "latviešu" },
  { "MK", "mk", 0x042F, "Macedonian", "македонски" },
  { "MS", "ms", 0x043E, "Malay", "Melayu" },
  { "MYA", "my", 0x0455, "Burmese", "မြန်မာ" },
  { "NL", "nl", 0x0413, "Dutch", "Nederlands" },
  { "NO", "nb", 0x0414, "Norwegian Bokmål", "norsk bokmål" },
  { "PA", "pa", 0x0446, "Punjabi", "ਪੰਜਾਬੀ" },
  { "PL", "pl", 0x0415, "Polish", "polski" },
  { "PT", "pt-BR", 0x0416, "Portuguese (Brazil)", "português (Brasil)" },
  { "PTG", "pt-PT", 0x0816, "Portuguese (Portugal)", "português (Portugal)" },
  { "RO", "ro", 0x0418, "Romanian", "română" },
  { "RU", "ru", 0x0419, "Russian", "русский" },
  { "SK", "sk", 0x041B, "Slovak", "slovenčina" },
  { "SL", "sl", 0x0424, "Slovenian", "slovenščina" },
  { "SQ", "sq", 0x041C, "Albanian", "shqip" },
  { "SRL", "sr-Latn", 0x081A, "Serbian (Latin)", "srpski (latinica)" },
  { "SV", "sv", 0x041D, "Swedish", "svenska" },
  { "TA", "ta", 0x0449, "Tamil", "தமிழ்" },
  { "TH", "th", 0x041E, "Thai", "ไทย" },
  { "TR", "tr", 0x041F, "Turkish", "Türkçe" },
  { "UK", "uk", 0x0422, "Ukrainian", "українська" },
  { "VI", "vi", 0x042A, "Vietnamese", "Tiếng Việt" },
  // AdditionalLanguageMask + 'E' / 'K'
  { "XXE", "eo", 0xFFFFFF45, "Esperanto", "esperanto" },
  { "XXK", "kab", 0xFFFFFF4B, "Kabyle", "Taqbaylit" },
};

// WinSCP form (translation context) of NixSCP's windows and dialogs
static const struct { const char * ClassName; const char * Forms; } FormTable[] =
{
  { "CommanderWindow", "ScpCommander,NonVisual,CustomScpExplorer" },
  { "CommanderActions", "NonVisual,ScpCommander,CustomScpExplorer" },
  { "FilePanel", "ScpCommander,NonVisual,CustomScpExplorer" },
  { "QueuePanel", "NonVisual,CustomScpExplorer" },
  { "EditorWindow", "Editor" },
  { "LoginDialog", "Login" },
  { "ImportSessionsDialog", "ImportSessions" },
  { "PreferencesDialog", "Preferences" },
  { "EditorPreferencesDialog", "EditorPreferences" },
  { "SiteAdvancedDialog", "SiteAdvanced" },
  { "CopyDialog", "Copy" },
  { "CopyParamsDialog", "CopyParamCustom,CopyParams" },
  { "CopyParamPresetDialog", "CopyParamPreset,CopyParams" },
  { "CopyParamsFrame", "CopyParams" },
  { "CustomCommandDialog", "CustomCommand" },
  { "ConsoleDialog", "Console" },
  { "AboutDialog", "About" },
  { "AuthenticateDialog", "Authenticate" },
  { "LinkDialog", "Symlink" },
  { "GenerateUrlDialog", "GenerateUrl" },
  { "EditMaskDialog", "EditMask" },
  { "FindFilesDialog", "FileFind" },
  { "ServerInfoDialog", "FileSystemInfo" },
  { "RemoteTransferDialog", "RemoteTransfer" },
  { "RemoteMoveDialog", "RemoteTransfer" },
  { "OpenDirectoryDialog", "OpenDirectory" },
  { "PropertiesDialog", "Properties,Rights" },
  { "CreateDirectoryDialog", "CreateDirectory,Rights" },
  { "RightsWidget", "Rights" },
  { "ProgressDialog", "Progress" },
  { "LocationProfilesDialog", "LocationProfiles" },
  { "SynchronizeDialog", "Synchronize" },
  { "FullSynchronizeDialog", "FullSynchronize" },
  { "SynchronizeProgressDialog", "SynchronizeProgress" },
  { "SynchronizeChecklistDialog", "SynchronizeChecklist" },
  { "MessageForm", "" },
};

static const char * const TranslatedEn = "nixscpEn_";
static const char * const TranslatedTr = "nixscpTr_";

//---------------------------------------------------------------------------
// Delphi format strings (Format, FMTLOAD): %[index:][-][width][.prec]type, %%
namespace
{
struct FormatPart
{
  bool Spec = false;
  QString Literal;
  int Arg = 0;
  QChar Type;
};
}

static bool ParseFormat(const QString & Format, QList<FormatPart> & Parts)
{
  static const QRegularExpression SpecRegex(
    QStringLiteral("\\G%(?:(\\d+):)?-?(\\d+)?(?:\\.(\\d+))?([dDuUeEfFgGnNmMpPsSxX])"));
  Parts.clear();
  QString Literal;
  int NextArg = 0;
  int Index = 0;
  while (Index < Format.size())
  {
    QChar C = Format.at(Index);
    if (C != QLatin1Char('%'))
    {
      Literal.append(C);
      Index++;
      continue;
    }
    if ((Index + 1 < Format.size()) && (Format.at(Index + 1) == QLatin1Char('%')))
    {
      Literal.append(C);
      Index += 2;
      continue;
    }
    QRegularExpressionMatch Match = SpecRegex.match(Format, Index);
    if (!Match.hasMatch())
    {
      return false;
    }
    if (!Literal.isEmpty())
    {
      FormatPart Part;
      Part.Literal = Literal;
      Parts.append(Part);
      Literal.clear();
    }
    FormatPart Part;
    Part.Spec = true;
    Part.Arg = Match.capturedView(1).isEmpty() ? NextArg : Match.capturedView(1).toInt();
    Part.Type = Match.capturedView(4).at(0).toLower();
    NextArg = Part.Arg + 1;
    Parts.append(Part);
    Index = Match.capturedEnd();
  }
  if (!Literal.isEmpty())
  {
    FormatPart Part;
    Part.Literal = Literal;
    Parts.append(Part);
  }
  return true;
}

struct Translator::Template
{
  QList<FormatPart> English;
  QList<FormatPart> Localized;
  QString Longest;            // longest literal part (quick pre-check)
  int LiteralLength = 0;      // specificity
  QList<int> GroupArgs;       // capture group -> argument index
  mutable QRegularExpression Regex;
  mutable bool Compiled = false;

  void Compile() const
  {
    if (Compiled)
    {
      return;
    }
    Compiled = true;
    QString Pattern = QStringLiteral("^");
    QHash<int, int> ArgGroups;
    int Group = 0;
    for (const FormatPart & Part : English)
    {
      if (!Part.Spec)
      {
        Pattern += QRegularExpression::escape(Part.Literal);
        continue;
      }
      auto Existing = ArgGroups.constFind(Part.Arg);
      if (Existing != ArgGroups.constEnd())
      {
        Pattern += QStringLiteral("\\%1").arg(*Existing);
        continue;
      }
      Group++;
      ArgGroups.insert(Part.Arg, Group);
      switch (Part.Type.unicode())
      {
        case 'd':
        case 'u':
          // localized numbers (grouping separators)
          Pattern += QStringLiteral("(-?\\d[\\d.,'\\x{00A0}\\x{202F}]*)");
          break;
        case 'x':
          Pattern += QStringLiteral("([0-9A-Fa-f]+)");
          break;
        case 'e':
        case 'f':
        case 'g':
        case 'n':
        case 'm':
          Pattern += QStringLiteral("(-?[\\d.,'\\x{00A0}\\x{202F}eE+]+)");
          break;
        default:
          Pattern += QStringLiteral("(.*?)");
          break;
      }
    }
    Pattern += QStringLiteral("$");
    Regex.setPattern(Pattern);
    Regex.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);
  }
};

//---------------------------------------------------------------------------
Translator * Translator::Instance()
{
  static Translator * Result = nullptr;
  if (Result == nullptr)
  {
    Result = new Translator();
    if (QCoreApplication::instance() != nullptr)
    {
      Result->setParent(QCoreApplication::instance());
      QObject::connect(QCoreApplication::instance(), &QObject::destroyed, []() { Result = nullptr; });
    }
  }
  return Result;
}

Translator::Translator()
{
  setObjectName(QStringLiteral("NixSCPTranslator"));
}

Translator::~Translator()
{
  qDeleteAll(FTemplates);
}

const QList<Translator::LanguageInfo> & Translator::Languages()
{
  static QList<LanguageInfo> Result;
  if (Result.isEmpty())
  {
    // completeness, names and direction from the generated index
    QHash<QString, QJsonObject> Index;
    QFile File(QStringLiteral(":/translations/index.json"));
    if (File.open(QIODevice::ReadOnly))
    {
      const QJsonArray Array = QJsonDocument::fromJson(File.readAll()).array();
      for (const QJsonValue & Value : Array)
      {
        QJsonObject Object = Value.toObject();
        Index.insert(Object.value(QStringLiteral("lang")).toString(), Object);
      }
    }
    for (const LanguageEntry & Entry : LanguageTable)
    {
      LanguageInfo Info;
      Info.Code = QString::fromLatin1(Entry.Code);
      Info.Locale = QString::fromLatin1(Entry.Locale);
      Info.Lcid = Entry.Lcid;
      Info.EnglishName = QString::fromUtf8(Entry.EnglishName);
      Info.NativeName = QString::fromUtf8(Entry.NativeName);
      auto I = Index.constFind(Info.Code);
      if (I != Index.constEnd())
      {
        Info.Completeness = I->value(QStringLiteral("completeness")).toInt();
        Info.Rtl = I->value(QStringLiteral("rtl")).toBool();
      }
      else
      {
        Info.Completeness = -1;   // not built in
      }
      Result.append(Info);
    }
  }
  return Result;
}

const Translator::LanguageInfo * Translator::FindLanguage(unsigned int Lcid)
{
  for (const LanguageInfo & Info : Languages())
  {
    if (Info.Lcid == Lcid)
    {
      return &Info;
    }
  }
  // LoadNewResourceModule: language-only translation for another sublanguage
  // (e.g. German (Austria) 0x0C07 -> DE); never Traditional for Simplified Chinese
  if ((Lcid & AdditionalLanguageMask) != AdditionalLanguageMask)
  {
    unsigned int Primary = Lcid & 0x3FF;
    if (Primary != 0x04)   // LANG_CHINESE
    {
      for (const LanguageInfo & Info : Languages())
      {
        if (((Info.Lcid & AdditionalLanguageMask) != AdditionalLanguageMask) && ((Info.Lcid & 0x3FF) == Primary) &&
            ((Info.Lcid >> 10) == 1))   // SUBLANG_DEFAULT
        {
          return &Info;
        }
      }
    }
  }
  return nullptr;
}

const Translator::LanguageInfo * Translator::FindLanguage(const QString & CodeOrLocale)
{
  QString Name = CodeOrLocale;
  Name.replace(QLatin1Char('_'), QLatin1Char('-'));
  for (const LanguageInfo & Info : Languages())
  {
    if ((Info.Code.compare(Name, Qt::CaseInsensitive) == 0) || (Info.Locale.compare(Name, Qt::CaseInsensitive) == 0))
    {
      return &Info;
    }
  }
  return nullptr;
}

unsigned int Translator::CodeToLcid(const QString & Code)
{
  if (Code.compare(QStringLiteral("EN"), Qt::CaseInsensitive) == 0)
  {
    return InternalLcid;
  }
  const LanguageInfo * Info = FindLanguage(Code);
  return (Info != nullptr) ? Info->Lcid : 0;
}

QString Translator::LcidToCode(unsigned int Lcid)
{
  for (const LanguageInfo & Info : Languages())
  {
    if (Info.Lcid == Lcid)
    {
      return Info.Code;
    }
  }
  return QString();
}

QString Translator::DisplayName(unsigned int Lcid)
{
  // AddLocale: LOCALE_SENGLANGUAGE + TitleSeparator + LOCALE_SLANGUAGE
  if (Lcid == InternalLcid)
  {
    return QStringLiteral("English - English");
  }
  const LanguageInfo * Info = FindLanguage(Lcid);
  if (Info == nullptr)
  {
    return QString::number(Lcid);
  }
  return Info->EnglishName + QStringLiteral(" - ") + Info->NativeName;
}

static bool HasTranslation(const Translator::LanguageInfo & Info)
{
  return Info.Completeness > 0;
}

unsigned int Translator::DefaultLcid()
{
  // TWinConfiguration::GetDefaultLocale / TGUIConfiguration::SetLocaleInternal(NULL):
  // the user's UI language, when translated
  for (const QString & Name : QLocale::system().uiLanguages())
  {
    QLocale System(Name);
    if ((System.language() == QLocale::English) || (System.language() == QLocale::C))
    {
      return InternalLcid;
    }
    const LanguageInfo * Best = nullptr;
    for (const LanguageInfo & Info : Languages())
    {
      QLocale Candidate(Info.Locale);
      if ((Candidate.language() != System.language()) || !HasTranslation(Info))
      {
        continue;
      }
      // never Traditional Chinese for Simplified one (and Latin Serbian for Cyrillic)
      if ((System.script() != QLocale::AnyScript) && (Candidate.script() != System.script()) &&
          ((System.language() == QLocale::Chinese) || (System.language() == QLocale::Serbian)))
      {
        continue;
      }
      if ((Candidate.territory() == System.territory()) && Info.Locale.contains(QLatin1Char('-')))
      {
        return Info.Lcid;
      }
      if (Best == nullptr)
      {
        Best = &Info;
      }
    }
    if (Best != nullptr)
    {
      return Best->Lcid;
    }
  }
  return InternalLcid;
}

unsigned int Translator::AppliedLcid()
{
  return Instance()->FLcid;
}

QString Translator::AppliedCode()
{
  return Instance()->FCode;
}

//---------------------------------------------------------------------------
bool Translator::Load(const QString & Code)
{
  FStrings.clear();
  FContexts.clear();
  qDeleteAll(FTemplates);
  FTemplates.clear();
  FTemplatesBuilt = false;
  FCode.clear();
  FLcid = InternalLcid;
  if (Code.isEmpty())
  {
    return true;
  }
  QFile File(QStringLiteral(":/translations/%1.json").arg(Code.toLower()));
  if (!File.open(QIODevice::ReadOnly))
  {
    return false;
  }
  QJsonParseError Error;
  QJsonDocument Document = QJsonDocument::fromJson(File.readAll(), &Error);
  if (Document.isNull())
  {
    return false;
  }
  QJsonObject Root = Document.object();
  const QJsonObject Strings = Root.value(QStringLiteral("strings")).toObject();
  FStrings.reserve(Strings.size());
  for (auto I = Strings.constBegin(); I != Strings.constEnd(); ++I)
  {
    FStrings.insert(I.key(), I.value().toString());
  }
  const QJsonObject Contexts = Root.value(QStringLiteral("contexts")).toObject();
  for (auto C = Contexts.constBegin(); C != Contexts.constEnd(); ++C)
  {
    QHash<QString, QString> & Map = FContexts[C.key()];
    const QJsonObject Texts = C.value().toObject();
    for (auto I = Texts.constBegin(); I != Texts.constEnd(); ++I)
    {
      Map.insert(I.key(), I.value().toString());
    }
  }
  FCode = Root.value(QStringLiteral("lang")).toString(Code.toUpper());
  FLcid = CodeToLcid(FCode);
  return true;
}

bool Translator::LoadLanguage(const QString & Code)
{
  Translator * T = Instance();
  bool Result = T->Load(Code);
  emit T->LanguageChanged();
  return Result;
}

bool Translator::SetLanguage(unsigned int Lcid)
{
  Translator * T = Instance();
  if (Lcid == 0)
  {
    Lcid = DefaultLcid();
  }
  const LanguageInfo * Info = (Lcid == InternalLcid) ? nullptr : FindLanguage(Lcid);
  QString Code = ((Info != nullptr) && HasTranslation(*Info)) ? Info->Code : QString();
  if ((Code == T->FCode) && (T->FFilterInstalled || Code.isEmpty()))
  {
    return !Code.isEmpty() || (Lcid == InternalLcid);
  }
  bool Loaded = T->Load(Code);
  if (!Loaded)
  {
    T->Load(QString());
  }

  if (qobject_cast<QApplication *>(QCoreApplication::instance()) != nullptr)
  {
    // Qt's own texts (file dialogs, context menus of edit boxes)
    if (T->FQtTranslator != nullptr)
    {
      QCoreApplication::removeTranslator(static_cast<QTranslator *>(T->FQtTranslator));
      delete T->FQtTranslator;
      T->FQtTranslator = nullptr;
    }
    if (!T->FCode.isEmpty())
    {
      auto * QtTranslator = new QTranslator(T);
      QLocale Locale(Info->Locale);
      if (QtTranslator->load(Locale, QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
      {
        QCoreApplication::installTranslator(QtTranslator);
        T->FQtTranslator = QtTranslator;
      }
      else
      {
        delete QtTranslator;
      }
    }
    QGuiApplication::setLayoutDirection(((Info != nullptr) && Info->Rtl && !T->FCode.isEmpty()) ? Qt::RightToLeft : Qt::LeftToRight);
    T->InstallFilter();
    T->RetranslateAll();
  }
  emit T->LanguageChanged();
  return Code.isEmpty() ? (Lcid == InternalLcid) : (Loaded && !T->FCode.isEmpty());
}

void Translator::InstallFilter()
{
  if (!FFilterInstalled && (QCoreApplication::instance() != nullptr))
  {
    QCoreApplication::instance()->installEventFilter(this);
    FFilterInstalled = true;
  }
}

void Translator::RetranslateAll()
{
  FRetranslatePending = false;
  // texts produced for the previous language map back to their English originals
  FPreviousReverse.insert(FReverse);
  FReverse.clear();
  FForce = true;
  const QList<QWidget *> Widgets = QApplication::allWidgets();
  for (QWidget * Widget : Widgets)
  {
    DoTranslateWidget(Widget);
  }
  for (QWidget * Window : QApplication::topLevelWidgets())
  {
    const QList<QAction *> Actions = Window->findChildren<QAction *>();
    for (QAction * Action : Actions)
    {
      DoTranslateAction(Action, ContextsOf(Window));
    }
  }
  FForce = false;
}

//---------------------------------------------------------------------------
static QString StripMnemonic(const QString & Text)
{
  static const QRegularExpression Suffix(QStringLiteral("\\s*\\(&[^&\\s]\\)"));
  QString Result = Text;
  if (Result.contains(QLatin1String("(&")))
  {
    Result.remove(Suffix);
  }
  Result.replace(QLatin1String("&&"), QStringLiteral("\x01"));
  Result.remove(QLatin1Char('&'));
  Result.replace(QStringLiteral("\x01"), QStringLiteral("&"));
  return Result;
}

static bool HasMnemonic(const QString & Text)
{
  int Index = Text.indexOf(QLatin1Char('&'));
  while (Index >= 0)
  {
    if ((Index + 1 < Text.size()) && (Text.at(Index + 1) != QLatin1Char('&')) && !Text.at(Index + 1).isSpace())
    {
      return true;
    }
    Index = Text.indexOf(QLatin1Char('&'), Index + 2);
  }
  return false;
}

static bool HasLetters(const QString & Text)
{
  for (QChar C : Text)
  {
    if (C.isLetter())
    {
      return true;
    }
  }
  return false;
}

QString Translator::Lookup(const QString & Text, const QStringList & Contexts) const
{
  for (const QString & Context : Contexts)
  {
    auto C = FContexts.constFind(Context);
    if (C != FContexts.constEnd())
    {
      auto I = C->constFind(Text);
      if (I != C->constEnd())
      {
        return *I;
      }
    }
  }
  auto I = FStrings.constFind(Text);
  return (I != FStrings.constEnd()) ? *I : QString();
}

// Trailing ":" / "..." / "…" of a text and its translation
static const QString Suffixes[] = { QStringLiteral("..."), QStringLiteral("…"), QStringLiteral(":") };

static QString ChopSuffix(const QString & Text)
{
  QString Result = Text;
  for (const QString & Suffix : Suffixes)
  {
    if (Result.endsWith(Suffix))
    {
      Result.chop(Suffix.size());
      while (Result.endsWith(QLatin1Char(' ')) || Result.endsWith(QChar(0x00A0)))
      {
        Result.chop(1);
      }
      break;
    }
  }
  return Result;
}

QString Translator::TranslateBase(const QString & Text, const QStringList & Contexts) const
{
  QString Result = Lookup(Text, Contexts);
  if (!Result.isNull())
  {
    return Result;
  }
  // "..." <-> "…"
  if (Text.endsWith(QLatin1String("...")))
  {
    Result = Lookup(Text.chopped(3) + QChar(0x2026), Contexts);
  }
  else if (Text.endsWith(QChar(0x2026)))
  {
    Result = Lookup(Text.chopped(1) + QStringLiteral("..."), Contexts);
  }
  if (!Result.isNull())
  {
    return Result;
  }
  // with the suffix removed / added
  for (const QString & Suffix : Suffixes)
  {
    if (Text.endsWith(Suffix))
    {
      QString Base = Text.chopped(Suffix.size());
      while (Base.endsWith(QLatin1Char(' ')))
      {
        Base.chop(1);
      }
      if (Base.isEmpty())
      {
        return QString();
      }
      Result = Lookup(Base, Contexts);
      if (Result.isNull())
      {
        for (const QString & Other : Suffixes)
        {
          if (Other != Suffix)
          {
            Result = Lookup(Base + Other, Contexts);
            if (!Result.isNull())
            {
              break;
            }
          }
        }
      }
      if (!Result.isNull())
      {
        return ChopSuffix(Result) + Text.mid(Base.size());
      }
      return QString();
    }
  }
  for (const QString & Suffix : Suffixes)
  {
    Result = Lookup(Text + Suffix, Contexts);
    if (!Result.isNull())
    {
      return ChopSuffix(Result);
    }
  }
  return QString();
}

QString Translator::Translate(const QString & Text, const QStringList & Contexts, bool Message) const
{
  if (FStrings.isEmpty() || Text.isEmpty() || !HasLetters(Text))
  {
    return Text;
  }
  if (Message)
  {
    return TranslateMessage(Text, 0);
  }
  QString Result = TranslateBase(Text, Contexts);
  if (!Result.isNull())
  {
    return Result;
  }
  // surrounding white space
  QString Trimmed = Text.trimmed();
  if ((Trimmed.size() != Text.size()) && !Trimmed.isEmpty())
  {
    Result = TranslateBase(Trimmed, Contexts);
    if (!Result.isNull())
    {
      int Start = Text.indexOf(Trimmed);
      return Text.left(Start) + Result + Text.mid(Start + Trimmed.size());
    }
  }
  // without mnemonic: the translation is used without its mnemonic
  if (HasMnemonic(Text))
  {
    QString Bare = StripMnemonic(Text);
    Result = TranslateBase(Bare, Contexts);
    if (!Result.isNull())
    {
      return StripMnemonic(Result);
    }
  }
  else if (Text.contains(QLatin1Char('&')))
  {
    // "&&" (a literal ampersand)
    QString Plain = Text;
    Plain.replace(QLatin1String("&&"), QLatin1String("&"));
    Result = TranslateBase(Plain, Contexts);
    if (!Result.isNull())
    {
      return StripMnemonic(Result.replace(QLatin1String("&&"), QLatin1String("&")).replace(QLatin1String("&"), QLatin1String("&&")));
    }
  }
  return Text;
}

void Translator::BuildTemplates() const
{
  if (FTemplatesBuilt)
  {
    return;
  }
  FTemplatesBuilt = true;
  for (auto I = FStrings.constBegin(); I != FStrings.constEnd(); ++I)
  {
    const QString & English = I.key();
    if (!English.contains(QLatin1Char('%')))
    {
      continue;
    }
    auto * T = new Template();
    bool Valid = ParseFormat(English, T->English) && ParseFormat(I.value(), T->Localized);
    int Specs = 0;
    QSet<int> Args;
    for (const FormatPart & Part : T->English)
    {
      if (Part.Spec)
      {
        Specs++;
        Args.insert(Part.Arg);
      }
      else
      {
        T->LiteralLength += Part.Literal.size();
        if (Part.Literal.size() > T->Longest.size())
        {
          T->Longest = Part.Literal;
        }
      }
    }
    for (const FormatPart & Part : T->Localized)
    {
      if (Part.Spec && !Args.contains(Part.Arg))
      {
        Valid = false;
      }
    }
    // a template must have some text of its own ("%s", "%s: %s" are not worth it)
    int Letters = 0;
    for (const FormatPart & Part : T->English)
    {
      for (QChar C : Part.Literal)
      {
        Letters += C.isLetter() ? 1 : 0;
      }
    }
    if (!Valid || (Specs == 0) || (Letters < 3))
    {
      delete T;
      continue;
    }
    FTemplates.append(T);
  }
  // most specific first
  std::sort(FTemplates.begin(), FTemplates.end(), [](const Template * A, const Template * B)
  {
    return A->LiteralLength > B->LiteralLength;
  });
}

bool Translator::MatchTemplate(const QString & Message, QString & Result) const
{
  BuildTemplates();
  for (const Template * T : FTemplates)
  {
    if (!Message.contains(T->Longest))
    {
      continue;
    }
    T->Compile();
    QRegularExpressionMatch Match = T->Regex.match(Message);
    if (!Match.hasMatch())
    {
      continue;
    }
    // capture groups in the order of the first occurrence of each argument
    QHash<int, QString> Args;
    int Group = 0;
    for (const FormatPart & Part : T->English)
    {
      if (Part.Spec && !Args.contains(Part.Arg))
      {
        Group++;
        Args.insert(Part.Arg, Match.captured(Group));
      }
    }
    Result.clear();
    for (const FormatPart & Part : T->Localized)
    {
      Result += Part.Spec ? Args.value(Part.Arg) : Part.Literal;
    }
    return true;
  }
  return false;
}

QString Translator::TranslateMessage(const QString & Message, int Depth) const
{
  QString Text = Message;
  if (Text.contains(QLatin1Char('\r')))
  {
    Text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
  }
  if (!HasLetters(Text))
  {
    return Message;
  }
  QString Result = Lookup(Text, QStringList());
  if (!Result.isNull())
  {
    return Result;
  }
  if (MatchTemplate(Text, Result))
  {
    return Result;
  }
  if (Depth < 3)
  {
    // paragraphs, then lines
    for (const QString & Separator : { QStringLiteral("\n\n"), QStringLiteral("\n") })
    {
      if (Text.contains(Separator))
      {
        QStringList Parts = Text.split(Separator);
        for (QString & Part : Parts)
        {
          Part = TranslateMessage(Part, Depth + 1);
        }
        return Parts.join(Separator);
      }
    }
  }
  // a single line: white space, ":" / "..." and mnemonics as for captions
  QString Trimmed = Text.trimmed();
  if (Trimmed.size() != Text.size())
  {
    if (Trimmed.isEmpty())
    {
      return Message;
    }
    QString Inner = TranslateMessage(Trimmed, 3);
    int Start = Text.indexOf(Trimmed);
    return Text.left(Start) + Inner + Text.mid(Start + Trimmed.size());
  }
  return Translate(Text, QStringList(), false);
}

//---------------------------------------------------------------------------
QString Translator::Tr(const QString & English, const char * Context)
{
  Translator * T = Instance();
  if (T->FStrings.isEmpty())
  {
    return English;
  }
  QStringList Contexts;
  if ((Context != nullptr) && (*Context != '\0'))
  {
    Contexts = QString::fromLatin1(Context).split(QLatin1Char(','));
  }
  return T->Translate(English, Contexts, false);
}

QString Translator::Tr(const QString & English, const QString & Context)
{
  Translator * T = Instance();
  if (T->FStrings.isEmpty())
  {
    return English;
  }
  return T->Translate(English, Context.isEmpty() ? QStringList() : Context.split(QLatin1Char(',')), false);
}

QString Translator::TrMessage(const QString & Message)
{
  Translator * T = Instance();
  if (T->FStrings.isEmpty() || Message.isEmpty())
  {
    return Message;
  }
  return T->TranslateMessage(Message, 0);
}

//---------------------------------------------------------------------------
static QString ClassNameOf(const QObject * Object)
{
  static QHash<const std::type_info *, QString> Cache;
  const std::type_info * Type = &typeid(*Object);
  auto I = Cache.constFind(Type);
  if (I != Cache.constEnd())
  {
    return *I;
  }
  int Status = 0;
  char * Demangled = abi::__cxa_demangle(Type->name(), nullptr, nullptr, &Status);
  QString Name = (Status == 0) && (Demangled != nullptr) ? QString::fromLatin1(Demangled) : QString::fromLatin1(Type->name());
  std::free(Demangled);
  int Colons = Name.lastIndexOf(QLatin1String("::"));
  if (Colons >= 0)
  {
    Name = Name.mid(Colons + 2);
  }
  Cache.insert(Type, Name);
  return Name;
}

static const char * FormOfClass(const QString & ClassName)
{
  for (const auto & Entry : FormTable)
  {
    if (ClassName == QLatin1String(Entry.ClassName))
    {
      return Entry.Forms;
    }
  }
  return nullptr;
}

QStringList Translator::ContextsOf(const QWidget * Widget) const
{
  // the nearest NixSCP window/panel whose WinSCP form is known (menus: their parents)
  for (const QObject * Object = Widget; Object != nullptr; Object = Object->parent())
  {
    const char * Forms = FormOfClass(ClassNameOf(Object));
    if (Forms == nullptr)
    {
      // object names that follow WinSCP's form names ("LoginDialog", "Preferences")
      QString Name = Object->objectName();
      if (Name.endsWith(QLatin1String("Dialog")))
      {
        Name.chop(6);
      }
      if (!Name.isEmpty() && FContexts.contains(Name) && Object->isWidgetType() &&
          static_cast<const QWidget *>(Object)->isWindow())
      {
        return { Name };
      }
      continue;
    }
    if (*Forms == '\0')
    {
      return QStringList();
    }
    return QString::fromLatin1(Forms).split(QLatin1Char(','));
  }
  return QStringList();
}

QString Translator::FormContext(const QWidget * Widget)
{
  return Instance()->ContextsOf(Widget).value(0);
}

QString Translator::Apply(QObject * Object, const char * Key, const QString & Current, const QStringList & Contexts, bool Message)
{
  if (Current.isEmpty())
  {
    return Current;
  }
  QByteArray EnKey = QByteArray(TranslatedEn) + Key;
  QByteArray TrKey = QByteArray(TranslatedTr) + Key;
  QVariant Previous = Object->property(TrKey.constData());
  bool Unchanged = Previous.isValid() && (Previous.toString() == Current);
  if (Unchanged && !FForce)
  {
    // processed already (in this language)
    return Current;
  }
  QString English = Unchanged ? Object->property(EnKey.constData()).toString() : FPreviousReverse.value(Current, Current);
  QString Result = Translate(English, Contexts, Message);
  static const bool Debug = qEnvironmentVariableIsSet("NIXSCP_TRANSLATOR_DEBUG");
  if (Debug && (Result == English) && !FStrings.isEmpty() && HasLetters(English) && !FReverse.contains(English))
  {
    // testing aid: lists the texts without a translation
    qInfo("untranslated [%s] %s.%s: %s", qPrintable(Contexts.join(QLatin1Char(','))), Object->metaObject()->className(), Key,
      qPrintable(English.left(80)));
  }
  // remembered also when untranslated: the text is not looked up again until it changes
  Object->setProperty(EnKey.constData(), English);
  Object->setProperty(TrKey.constData(), Result);
  if (Result != English)
  {
    FReverse.insert(Result, English);
  }
  return Result;
}

// Has the application changed the text since it was translated?
static bool TextChanged(const QObject * Object, const char * Key, const QString & Current)
{
  if (Current.isEmpty())
  {
    return false;
  }
  QVariant Previous = Object->property((QByteArray(TranslatedTr) + Key).constData());
  return !Previous.isValid() || (Previous.toString() != Current);
}

QString Translator::ApplyItem(const QString & Current, const QStringList & Contexts)
{
  if (Current.isEmpty())
  {
    return Current;
  }
  QString English = FReverse.value(Current, FPreviousReverse.value(Current, Current));
  QString Result = Translate(English, Contexts, false);
  if (Result != English)
  {
    FReverse.insert(Result, English);
  }
  return Result;
}

void Translator::DoTranslateAction(QAction * Action, const QStringList & Contexts)
{
  if ((Action == nullptr) || Action->isSeparator() || Action->property("NixSCPNoTranslate").toBool())
  {
    return;
  }
  QStringList ActionContexts = Contexts;
  if (QObject * Parent = Action->parent(); (Parent != nullptr) && !Parent->isWidgetType())
  {
    // actions of CommanderActions (NonVisual.dfm)
    const char * Forms = FormOfClass(ClassNameOf(Parent));
    if ((Forms != nullptr) && (*Forms != '\0'))
    {
      ActionContexts = QString::fromLatin1(Forms).split(QLatin1Char(','));
    }
  }
  QString Text = Action->text();
  QString ToolTip = Action->toolTip();
  // QAction::toolTip() defaults to the text without the mnemonic
  bool OwnToolTip = !ToolTip.isEmpty() && (ToolTip != StripMnemonic(Text)) &&
    (ToolTip != StripMnemonic(Action->property((QByteArray(TranslatedTr) + "text").constData()).toString()));
  QString NewText = Apply(Action, "text", Text, ActionContexts);
  if (NewText != Text)
  {
    Action->setText(NewText);
  }
  if (OwnToolTip)
  {
    QString NewToolTip = Apply(Action, "toolTip", ToolTip, ActionContexts);
    if (NewToolTip != ToolTip)
    {
      Action->setToolTip(NewToolTip);
    }
  }
  // an own icon text (tool buttons showing the action), not the default one derived from the text
  QString IconText = Action->iconText();
  auto AutoIconText = [](const QString & S)
  {
    QString Result = StripMnemonic(S);
    if (Result.endsWith(QLatin1String("...")))
    {
      Result.chop(3);
    }
    else if (Result.endsWith(QChar(0x2026)))
    {
      Result.chop(1);
    }
    return Result;
  };
  if (!IconText.isEmpty() && (IconText != AutoIconText(Text)) && (IconText != AutoIconText(Action->text())))
  {
    QString NewIconText = Apply(Action, "iconText", IconText, ActionContexts);
    if (NewIconText != IconText)
    {
      Action->setIconText(NewIconText);
    }
  }
  QString StatusTip = Action->statusTip();
  QString NewStatusTip = Apply(Action, "statusTip", StatusTip, ActionContexts);
  if (NewStatusTip != StatusTip)
  {
    Action->setStatusTip(NewStatusTip);
  }
}

void Translator::DoTranslateWidget(QWidget * Widget)
{
  if ((Widget == nullptr) || Widget->property("NixSCPNoTranslate").toBool())
  {
    return;
  }
  // children of a NixSCPNoTranslate widget (e.g. the items of a list) are its business
  QStringList Contexts = ContextsOf(Widget);
  auto Set = [&](const char * Key, const QString & Current, const std::function<void(const QString &)> & Setter, bool Message = false)
  {
    QString Result = Apply(Widget, Key, Current, Contexts, Message);
    if (Result != Current)
    {
      Setter(Result);
    }
  };

  if (Widget->isWindow() || qobject_cast<QDockWidget *>(Widget))
  {
    Set("windowTitle", Widget->windowTitle(), [Widget](const QString & S) { Widget->setWindowTitle(S); });
  }
  Set("toolTip", Widget->toolTip(), [Widget](const QString & S) { Widget->setToolTip(S); });
  Set("whatsThis", Widget->whatsThis(), [Widget](const QString & S) { Widget->setWhatsThis(S); });
  Set("statusTip", Widget->statusTip(), [Widget](const QString & S) { Widget->setStatusTip(S); });

  if (auto * Box = qobject_cast<QMessageBox *>(Widget))
  {
    Set("text", Box->text(), [Box](const QString & S) { Box->setText(S); }, true);
    Set("informativeText", Box->informativeText(), [Box](const QString & S) { Box->setInformativeText(S); }, true);
  }
  else if (auto * Label = qobject_cast<QLabel *>(Widget))
  {
    // labels of message boxes are translated with the box; selectable labels show data
    bool InMessageBox = (qobject_cast<QMessageBox *>(Label->window()) != nullptr);
    if (!InMessageBox && !(Label->textInteractionFlags() & Qt::TextSelectableByMouse))
    {
      Set("text", Label->text(), [Label](const QString & S) { Label->setText(S); });
    }
  }
  else if (auto * Button = qobject_cast<QAbstractButton *>(Widget))
  {
    auto * ToolButton = qobject_cast<QToolButton *>(Widget);
    if ((ToolButton == nullptr) || (ToolButton->defaultAction() == nullptr))
    {
      Set("text", Button->text(), [Button](const QString & S) { Button->setText(S); });
    }
  }
  else if (auto * GroupBox = qobject_cast<QGroupBox *>(Widget))
  {
    Set("title", GroupBox->title(), [GroupBox](const QString & S) { GroupBox->setTitle(S); });
  }
  else if (auto * Combo = qobject_cast<QComboBox *>(Widget))
  {
    Set("placeholderText", Combo->placeholderText(), [Combo](const QString & S) { Combo->setPlaceholderText(S); });
    // only choice lists; editable combos hold user data
    if (!Combo->isEditable())
    {
      for (int Index = 0; Index < Combo->count(); Index++)
      {
        QString Current = Combo->itemText(Index);
        QString Result = ApplyItem(Current, Contexts);
        if (Result != Current)
        {
          Combo->setItemText(Index, Result);
        }
      }
    }
  }
  else if (auto * Edit = qobject_cast<QLineEdit *>(Widget))
  {
    Set("placeholderText", Edit->placeholderText(), [Edit](const QString & S) { Edit->setPlaceholderText(S); });
  }
  else if (auto * PlainEdit = qobject_cast<QPlainTextEdit *>(Widget))
  {
    Set("placeholderText", PlainEdit->placeholderText(), [PlainEdit](const QString & S) { PlainEdit->setPlaceholderText(S); });
  }
  else if (auto * TextEdit = qobject_cast<QTextEdit *>(Widget))
  {
    Set("placeholderText", TextEdit->placeholderText(), [TextEdit](const QString & S) { TextEdit->setPlaceholderText(S); });
  }
  else if (auto * Tabs = qobject_cast<QTabBar *>(Widget))
  {
    for (int Index = 0; Index < Tabs->count(); Index++)
    {
      QString Current = Tabs->tabText(Index);
      QString Result = ApplyItem(Current, Contexts);
      if (Result != Current)
      {
        Tabs->setTabText(Index, Result);
      }
      QString ToolTip = Tabs->tabToolTip(Index);
      QString NewToolTip = ApplyItem(ToolTip, Contexts);
      if (NewToolTip != ToolTip)
      {
        Tabs->setTabToolTip(Index, NewToolTip);
      }
    }
  }
  else if (auto * Menu = qobject_cast<QMenu *>(Widget))
  {
    Set("title", Menu->title(), [Menu](const QString & S) { Menu->setTitle(S); });
  }
  else if (auto * Tree = qobject_cast<QTreeWidget *>(Widget))
  {
    QTreeWidgetItem * Header = Tree->headerItem();
    for (int Column = 0; (Header != nullptr) && (Column < Header->columnCount()); Column++)
    {
      QString Current = Header->text(Column);
      QString Result = ApplyItem(Current, Contexts);
      if (Result != Current)
      {
        Header->setText(Column, Result);
      }
    }
    // item texts of static lists only (NixSCPTranslateItems = true / list of columns)
    QVariant Items = Tree->property("NixSCPTranslateItems");
    if (Items.isValid())
    {
      QList<int> Columns;
      if (Items.typeId() == QMetaType::Bool)
      {
        for (int Column = 0; Column < Tree->columnCount(); Column++)
        {
          Columns.append(Column);
        }
      }
      else
      {
        for (const QVariant & Column : Items.toList())
        {
          Columns.append(Column.toInt());
        }
      }
      for (QTreeWidgetItemIterator It(Tree); *It != nullptr; ++It)
      {
        for (int Column : Columns)
        {
          QString Current = (*It)->text(Column);
          QString Result = ApplyItem(Current, Contexts);
          if (Result != Current)
          {
            (*It)->setText(Column, Result);
          }
          QString ToolTip = (*It)->toolTip(Column);
          QString NewToolTip = ApplyItem(ToolTip, Contexts);
          if (NewToolTip != ToolTip)
          {
            (*It)->setToolTip(Column, NewToolTip);
          }
        }
      }
    }
  }
  else if (auto * List = qobject_cast<QListWidget *>(Widget))
  {
    if (List->property("NixSCPTranslateItems").toBool())
    {
      for (int Index = 0; Index < List->count(); Index++)
      {
        QListWidgetItem * Item = List->item(Index);
        QString Current = Item->text();
        QString Result = ApplyItem(Current, Contexts);
        if (Result != Current)
        {
          Item->setText(Result);
        }
      }
    }
  }
  else if (auto * Table = qobject_cast<QTableWidget *>(Widget))
  {
    for (int Column = 0; Column < Table->columnCount(); Column++)
    {
      if (QTableWidgetItem * Item = Table->horizontalHeaderItem(Column))
      {
        QString Current = Item->text();
        QString Result = ApplyItem(Current, Contexts);
        if (Result != Current)
        {
          Item->setText(Result);
        }
      }
    }
  }
  else if (auto * Header = qobject_cast<QHeaderView *>(Widget))
  {
    // headers of plain item models (QStandardItemModel); NixSCP's own models translate themselves
    if (auto * Model = qobject_cast<QStandardItemModel *>(Header->model()))
    {
      for (int Column = 0; Column < Model->columnCount(); Column++)
      {
        if (QStandardItem * Item = Model->horizontalHeaderItem(Column))
        {
          QString Current = Item->text();
          QString Result = ApplyItem(Current, Contexts);
          if (Result != Current)
          {
            Item->setText(Result);
          }
        }
      }
    }
  }

  if (qobject_cast<QMenu *>(Widget) || qobject_cast<QMenuBar *>(Widget) || qobject_cast<QToolBar *>(Widget) ||
      !Widget->actions().isEmpty())
  {
    for (QAction * Action : Widget->actions())
    {
      DoTranslateAction(Action, Contexts);
    }
  }
}

void Translator::TranslateWidget(QWidget * Widget)
{
  Translator * T = Instance();
  if (T->FStrings.isEmpty() && T->FPreviousReverse.isEmpty())
  {
    return;
  }
  T->DoTranslateWidget(Widget);
  const QList<QWidget *> Children = Widget->findChildren<QWidget *>();
  for (QWidget * Child : Children)
  {
    T->DoTranslateWidget(Child);
  }
}

void Translator::TranslateAction(QAction * Action)
{
  Translator * T = Instance();
  if (T->FStrings.isEmpty() && T->FPreviousReverse.isEmpty())
  {
    return;
  }
  QWidget * Parent = qobject_cast<QWidget *>(Action->parent());
  T->DoTranslateAction(Action, (Parent != nullptr) ? T->ContextsOf(Parent) : QStringList());
}

bool Translator::eventFilter(QObject * Object, QEvent * Event)
{
  switch (Event->type())
  {
    case QEvent::Polish:
    case QEvent::Show:
      if (!FApplying && Object->isWidgetType() && (!FStrings.isEmpty() || !FPreviousReverse.isEmpty()))
      {
        DoTranslateWidget(static_cast<QWidget *>(Object));
      }
      break;
    case QEvent::Paint:
      // texts the application changed after the widget was shown
      if (!FStrings.isEmpty() || !FPreviousReverse.isEmpty())
      {
        bool Changed = false;
        if (Object->property("NixSCPNoTranslate").toBool())
        {
        }
        else if (auto * Label = qobject_cast<QLabel *>(Object))
        {
          Changed = !(Label->textInteractionFlags() & Qt::TextSelectableByMouse) &&
            (qobject_cast<QMessageBox *>(Label->window()) == nullptr) && TextChanged(Label, "text", Label->text());
        }
        else if (auto * Button = qobject_cast<QAbstractButton *>(Object))
        {
          auto * ToolButton = qobject_cast<QToolButton *>(Object);
          Changed = ((ToolButton == nullptr) || (ToolButton->defaultAction() == nullptr)) && TextChanged(Button, "text", Button->text());
        }
        else if (auto * GroupBox = qobject_cast<QGroupBox *>(Object))
        {
          Changed = TextChanged(GroupBox, "title", GroupBox->title());
        }
        if (Changed)
        {
          DoTranslateWidget(static_cast<QWidget *>(Object));
        }
      }
      break;
    case QEvent::ActionAdded:
    case QEvent::ActionChanged:
      if (!FApplying && Object->isWidgetType() && (!FStrings.isEmpty() || !FPreviousReverse.isEmpty()))
      {
        FApplying = true;
        DoTranslateAction(static_cast<QActionEvent *>(Event)->action(), ContextsOf(static_cast<QWidget *>(Object)));
        FApplying = false;
      }
      break;
    case QEvent::LanguageChange:
      // Qt resets its own texts (standard buttons) to its translations: ours again afterwards
      if (!FRetranslatePending && Object->isWidgetType())
      {
        FRetranslatePending = true;
        QTimer::singleShot(0, this, [this]() { RetranslateAll(); });
      }
      break;
    default:
      break;
  }
  return false;
}
