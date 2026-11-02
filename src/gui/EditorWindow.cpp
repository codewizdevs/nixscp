// Port of WinSCP source/forms/Editor.{cpp,dfm}
// (the Windows common Find/Replace dialogs are reimplemented; the Color
// button offers the default and a custom colour instead of the TBX palette)
#include "gui/EditorWindow.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "gui/GuiUserInterface.h"
#include "gui/PreferencesDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <langinfo.h>
#endif

// Open editors, for Save All and the application exit (TEditorForm instances)
static QList<EditorWindow *> EditorInstances;

// TColor (BGR; 0 = default, negative = system colour) to QColor (invalid = default)
static QColor ColorFromTColor(int Color)
{
  if (Color <= 0)
  {
    return QColor();
  }
  return QColor(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF);
}

//---------------------------------------------------------------------------
// Editor control with optional line numbers
//---------------------------------------------------------------------------

namespace
{

class EditorMemo;

class LineNumberArea : public QWidget
{
public:
  explicit LineNumberArea(EditorMemo * AEditor);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent * Event) override;

private:
  EditorMemo * FEditor;
};

class EditorMemo : public QPlainTextEdit
{
public:
  explicit EditorMemo(QWidget * Parent) : QPlainTextEdit(Parent)
  {
    FLineNumberArea = new LineNumberArea(this);
    FLineNumberArea->hide();
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() { UpdateLineNumberAreaWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect & Rect, int Dy)
    {
      if (!FShowLineNumbers)
      {
        return;
      }
      if (Dy != 0)
      {
        FLineNumberArea->scroll(0, Dy);
      }
      else
      {
        FLineNumberArea->update(0, Rect.y(), FLineNumberArea->width(), Rect.height());
      }
    });
  }

  void SetShowLineNumbers(bool Show)
  {
    FShowLineNumbers = Show;
    FLineNumberArea->setVisible(Show);
    UpdateLineNumberAreaWidth();
  }
  bool ShowLineNumbers() const { return FShowLineNumbers; }

  int LineNumberAreaWidth() const
  {
    if (!FShowLineNumbers)
    {
      return 0;
    }
    int Digits = QString::number(std::max(1, blockCount())).size();
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, Digits);
  }

  void PaintLineNumbers(QPaintEvent * Event)
  {
    QPainter Painter(FLineNumberArea);
    Painter.fillRect(Event->rect(), palette().color(QPalette::Window));
    QTextBlock Block = firstVisibleBlock();
    int Number = Block.blockNumber();
    int Top = qRound(blockBoundingGeometry(Block).translated(contentOffset()).top());
    int Bottom = Top + qRound(blockBoundingRect(Block).height());
    Painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
    while (Block.isValid() && (Top <= Event->rect().bottom()))
    {
      if (Block.isVisible() && (Bottom >= Event->rect().top()))
      {
        Painter.drawText(0, Top, FLineNumberArea->width() - 4, fontMetrics().height(), Qt::AlignRight,
          QString::number(Number + 1));
      }
      Block = Block.next();
      Top = Bottom;
      Bottom = Top + qRound(blockBoundingRect(Block).height());
      Number++;
    }
  }

protected:
  void resizeEvent(QResizeEvent * Event) override
  {
    QPlainTextEdit::resizeEvent(Event);
    QRect Rect = contentsRect();
    FLineNumberArea->setGeometry(QRect(Rect.left(), Rect.top(), LineNumberAreaWidth(), Rect.height()));
  }

private:
  LineNumberArea * FLineNumberArea;
  bool FShowLineNumbers = false;

  void UpdateLineNumberAreaWidth()
  {
    setViewportMargins(LineNumberAreaWidth(), 0, 0, 0);
    QRect Rect = contentsRect();
    FLineNumberArea->setGeometry(QRect(Rect.left(), Rect.top(), LineNumberAreaWidth(), Rect.height()));
  }
};

LineNumberArea::LineNumberArea(EditorMemo * AEditor) : QWidget(AEditor), FEditor(AEditor)
{
}

QSize LineNumberArea::sizeHint() const
{
  return QSize(FEditor->LineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent * Event)
{
  FEditor->PaintLineNumbers(Event);
}

//---------------------------------------------------------------------------
// Find / Replace dialog (TFindDialog / TReplaceDialog)
//---------------------------------------------------------------------------

class FindDialog : public QDialog
{
public:
  FindDialog(QWidget * Parent, bool Replace) : QDialog(Parent), FReplace(Replace)
  {
    setWindowTitle(Replace ? QStringLiteral("Replace") : QStringLiteral("Find"));
    QGridLayout * Layout = new QGridLayout(this);
    FindEdit = new QLineEdit(this);
    QLabel * FindLabel = new QLabel(QStringLiteral("Fi&nd what:"), this);
    FindLabel->setBuddy(FindEdit);
    Layout->addWidget(FindLabel, 0, 0);
    Layout->addWidget(FindEdit, 0, 1);
    int Row = 1;
    if (Replace)
    {
      ReplaceEdit = new QLineEdit(this);
      QLabel * ReplaceLabel = new QLabel(QStringLiteral("Re&place with:"), this);
      ReplaceLabel->setBuddy(ReplaceEdit);
      Layout->addWidget(ReplaceLabel, Row, 0);
      Layout->addWidget(ReplaceEdit, Row, 1);
      Row++;
    }
    QVBoxLayout * Checks = new QVBoxLayout();
    WholeWordCheck = new QCheckBox(QStringLiteral("Match &whole word only"), this);
    MatchCaseCheck = new QCheckBox(QStringLiteral("Match &case"), this);
    Checks->addWidget(WholeWordCheck);
    Checks->addWidget(MatchCaseCheck);
    Checks->addStretch(1);
    QHBoxLayout * Options = new QHBoxLayout();
    Options->addLayout(Checks);
    if (!Replace)
    {
      QGroupBox * DirectionGroup = new QGroupBox(QStringLiteral("Direction"), this);
      QHBoxLayout * DirectionLayout = new QHBoxLayout(DirectionGroup);
      UpRadio = new QRadioButton(QStringLiteral("&Up"), DirectionGroup);
      DownRadio = new QRadioButton(QStringLiteral("&Down"), DirectionGroup);
      DirectionLayout->addWidget(UpRadio);
      DirectionLayout->addWidget(DownRadio);
      DownRadio->setChecked(true);
      Options->addWidget(DirectionGroup);
    }
    Layout->addLayout(Options, Row, 0, 1, 2);

    QVBoxLayout * Buttons = new QVBoxLayout();
    FindNextButton = new QPushButton(QStringLiteral("&Find Next"), this);
    FindNextButton->setDefault(true);
    Buttons->addWidget(FindNextButton);
    if (Replace)
    {
      ReplaceButton = new QPushButton(QStringLiteral("&Replace"), this);
      ReplaceAllButton = new QPushButton(QStringLiteral("Replace &All"), this);
      Buttons->addWidget(ReplaceButton);
      Buttons->addWidget(ReplaceAllButton);
    }
    QPushButton * CancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    Buttons->addWidget(CancelButton);
    // frShowHelp: HELP_EDITOR_FIND / HELP_EDITOR_REPLACE
    QPushButton * HelpButton = new QPushButton(QStringLiteral("&Help"), this);
    HelpButton->setAutoDefault(false);
    HelpSystem::InstallFormHelp(this, Replace ? QStringLiteral("ui_editor_replace") : QStringLiteral("ui_editor_find"), HelpButton);
    Buttons->addWidget(HelpButton);
    Buttons->addStretch(1);
    Layout->addLayout(Buttons, 0, 2, Row + 1, 1);
    connect(CancelButton, &QPushButton::clicked, this, &QDialog::hide);
    connect(FindEdit, &QLineEdit::textChanged, this, [this]() { UpdateControls(); });
    UpdateControls();
    layout()->setSizeConstraint(QLayout::SetFixedSize);
  }

  void UpdateControls()
  {
    bool HasText = !FindEdit->text().isEmpty();
    FindNextButton->setEnabled(HasText);
    if (FReplace)
    {
      ReplaceButton->setEnabled(HasText);
      ReplaceAllButton->setEnabled(HasText);
    }
  }

  bool FReplace;
  QLineEdit * FindEdit = nullptr;
  QLineEdit * ReplaceEdit = nullptr;
  QCheckBox * WholeWordCheck = nullptr;
  QCheckBox * MatchCaseCheck = nullptr;
  QRadioButton * UpRadio = nullptr;
  QRadioButton * DownRadio = nullptr;
  QPushButton * FindNextButton = nullptr;
  QPushButton * ReplaceButton = nullptr;
  QPushButton * ReplaceAllButton = nullptr;
};

} // namespace

//---------------------------------------------------------------------------
// EditorWindow
//---------------------------------------------------------------------------

struct EditorWindow::Private : public QObject
{
  enum FindMode { fmFindNext, fmReplace, fmReplaceAll };

  EditorWindow * Q = nullptr;
  QString FileName;
  QString DisplayName;
  bool Loaded = false;
  bool ReadOnly = false;
  bool Reloading = false;
  bool Closing = false;          // FCloseAnnounced (closeEvent accepted or ForceClose)
  int EncodingOverride = -1;     // FInternalEditorEncodingOverride
  QColor CustomBackground;       // FBackgroundColor (session colour / Color button)
  ReloadHandler OnFileReload;

  // Internal editor preferences (TEditorConfiguration)
  int TabSize = 8;
  int DefaultCodePage = CodePageUTF8;
  bool WarnOnEncodingFallback = true;
  bool WarnOrLargeFileSize = true;
  qint64 LargeFileSize = 10 * 1024 * 1024;
  QColor PrefFontColor;
  QColor PrefBackgroundColor;
  Encoding FileEncoding = encUTF8;
  bool NewFile = false;
  bool EncodingSet = false;      // FEncoding != NULL (explicitly chosen / detected)
  bool WithBom = false;
  LineEnding Eol = eolLF;
  bool FinalNewLine = false;

  EditorMemo * Memo = nullptr;
  QLabel * LineStatus = nullptr;
  QLabel * ColumnStatus = nullptr;
  QLabel * CharacterStatus = nullptr;
  QLabel * EncodingStatus = nullptr;
  QLabel * EolStatus = nullptr;
  QLabel * StateStatus = nullptr;

  QAction * SaveAction = nullptr;
  QAction * SaveAllAction = nullptr;
  QAction * ReloadAction = nullptr;
  QAction * PreferencesAction = nullptr;
  QAction * ColorAction = nullptr;
  QAction * DefaultColorAction = nullptr;
  QAction * CustomColorAction = nullptr;
  QAction * HelpAction = nullptr;
  QMenu * ColorMenu = nullptr;
  QAction * CutAction = nullptr;
  QAction * CopyAction = nullptr;
  QAction * PasteAction = nullptr;
  QAction * DeleteAction = nullptr;
  QAction * SelectAllAction = nullptr;
  QAction * UndoAction = nullptr;
  QAction * RedoAction = nullptr;
  QAction * FindAction = nullptr;
  QAction * ReplaceAction = nullptr;
  QAction * FindNextAction = nullptr;
  QAction * GoToLineAction = nullptr;
  QAction * LineNumbersAction = nullptr;
  QAction * UTF8EncodingAction = nullptr;
  QAction * DefaultEncodingAction = nullptr;
  QAction * Latin1EncodingAction = nullptr;
  QMenu * EncodingMenu = nullptr;

  FindDialog * FindDlg = nullptr;
  FindDialog * ReplaceDlg = nullptr;
  FindDialog * LastFindDialog = nullptr;

  // TEditorConfiguration find options (Configuration\Interface\Editor)
  QString FindText;
  QString ReplaceText;
  bool FindMatchCase = false;
  bool FindWholeWord = false;
  bool FindDown = true;

  QAction * CreateAction(const QString & Caption, const QString & Hint, const QString & ThemeIcon,
    QStyle::StandardPixmap Fallback, const QKeySequence & ShortCut)
  {
    QAction * Action = new QAction(Caption, Q);
    if (!ThemeIcon.isEmpty())
    {
      QIcon Icon = QIcon::fromTheme(ThemeIcon);
      if (Icon.isNull() && (Fallback != QStyle::SP_CustomBase))
      {
        Icon = Q->style()->standardIcon(Fallback);
      }
      Action->setIcon(Icon);
    }
    Action->setToolTip(Hint);
    Action->setStatusTip(Hint);
    if (!ShortCut.isEmpty())
    {
      Action->setShortcut(ShortCut);
    }
    Action->setShortcutContext(Qt::WindowShortcut);
    Q->addAction(Action);
    return Action;
  }

  // --- configuration

  bool OpenEditorConfig(IniStorage & Storage, bool CanCreate, bool NixSCP)
  {
    return Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), CanCreate) &&
      (!NixSCP || Storage.OpenSubKey(QStringLiteral("NixSCP"), CanCreate)) &&
      Storage.OpenSubKey(QStringLiteral("Editor"), CanCreate);
  }

  void LoadConfiguration()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenEditorConfig(Storage, false, false))
    {
      FindText = Storage.ReadString(QStringLiteral("FindText"), FindText);
      ReplaceText = Storage.ReadString(QStringLiteral("ReplaceText"), ReplaceText);
      FindMatchCase = Storage.ReadBool(QStringLiteral("FindMatchCase"), FindMatchCase);
      FindWholeWord = Storage.ReadBool(QStringLiteral("FindWholeWord"), FindWholeWord);
      FindDown = Storage.ReadBool(QStringLiteral("FindDown"), FindDown);
    }
    IniStorage NixSCPStorage(&Configuration::Instance().Ini());
    if (OpenEditorConfig(NixSCPStorage, false, true))
    {
      QByteArray Geometry = QByteArray::fromHex(NixSCPStorage.ReadStringRaw(QStringLiteral("WindowGeometry"), QString()).toLatin1());
      if (!Geometry.isEmpty())
      {
        Q->restoreGeometry(Geometry);
      }
      Memo->SetShowLineNumbers(NixSCPStorage.ReadBool(QStringLiteral("LineNumbers"), false));
    }
  }

  // TEditorForm::ApplyConfiguration: Preferences > Editors > Internal editor
  void ApplyEditorPreferences()
  {
    const ConfigurationData & C = Configuration::Instance();
    TabSize = (C.EditorTabSize > 0) ? C.EditorTabSize : 8;
    WarnOrLargeFileSize = C.EditorWarnOrLargeFileSize;
    LargeFileSize = static_cast<qint64>(std::max(0, C.EditorLargeFileSize)) * 1024;
    // keys Configuration does not model are read as stored by WinSCP
    int FontStyle = 0;
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenEditorConfig(Storage, false, false))
    {
      PrefFontColor = ColorFromTColor(Storage.ReadInteger(QStringLiteral("FontColor"), 0));
      PrefBackgroundColor = ColorFromTColor(Storage.ReadInteger(QStringLiteral("BackgroundColor"), 0));
      DefaultCodePage = Storage.ReadInteger(QStringLiteral("Encoding"), CodePageUTF8);
      WarnOnEncodingFallback = Storage.ReadBool(QStringLiteral("WarnOnEncodingFallback"), true);
      FontStyle = Storage.ReadInteger(QStringLiteral("FontStyle"), 0);
    }
    else
    {
      PrefFontColor = QColor();
      PrefBackgroundColor = QColor();
      DefaultCodePage = CodePageUTF8;
      WarnOnEncodingFallback = true;
    }

    QFont Font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!C.EditorFontName.trimmed().isEmpty())
    {
      Font.setFamily(C.EditorFontName.trimmed());
    }
    if (C.EditorFontSize > 0)
    {
      Font.setPointSize(C.EditorFontSize);
    }
    // TFontStyles: fsBold, fsItalic, fsUnderline, fsStrikeOut
    Font.setBold((FontStyle & 0x01) != 0);
    Font.setItalic((FontStyle & 0x02) != 0);
    Font.setUnderline((FontStyle & 0x04) != 0);
    Font.setStrikeOut((FontStyle & 0x08) != 0);
    Memo->setFont(Font);
    Memo->setTabStopDistance(QFontMetricsF(Font).horizontalAdvance(QLatin1Char(' ')) * TabSize);
    Memo->setLineWrapMode(C.EditorWordWrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    UpdateColors();
  }

  // TEditorForm::UpdateBackgroundColor + GetWindowTextColor
  void UpdateColors()
  {
    QColor Background = CustomBackground.isValid() ? CustomBackground : PrefBackgroundColor;
    QPalette Palette = Memo->palette();
    QPalette Default = Q->palette();
    Palette.setColor(QPalette::Base, Background.isValid() ? Background : Default.color(QPalette::Base));
    QColor Text = PrefFontColor;
    if (!Text.isValid())
    {
      // default text colour, readable on the background
      Text = Background.isValid() ? ((Background.lightness() < 128) ? QColor(Qt::white) : QColor(Qt::black)) :
        Default.color(QPalette::Text);
    }
    Palette.setColor(QPalette::Text, Text);
    Memo->setPalette(Palette);
  }

  void SaveFindConfiguration()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenEditorConfig(Storage, true, false))
    {
      Storage.WriteString(QStringLiteral("FindText"), FindText);
      Storage.WriteString(QStringLiteral("ReplaceText"), ReplaceText);
      Storage.WriteBool(QStringLiteral("FindMatchCase"), FindMatchCase);
      Storage.WriteBool(QStringLiteral("FindWholeWord"), FindWholeWord);
      Storage.WriteBool(QStringLiteral("FindDown"), FindDown);
    }
  }

  void SaveWindowConfiguration()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenEditorConfig(Storage, true, true))
    {
      Storage.WriteStringRaw(QStringLiteral("WindowGeometry"), QString::fromLatin1(Q->saveGeometry().toHex()));
      Storage.WriteBool(QStringLiteral("LineNumbers"), Memo->ShowLineNumbers());
    }
  }

  // --- loading / saving

  static bool StartsWith(const QByteArray & Data, const char * Signature, int Len)
  {
    return (Data.size() >= Len) && (memcmp(Data.constData(), Signature, Len) == 0);
  }

  static QStringConverter::Encoding ConverterEncoding(Encoding AEncoding)
  {
    switch (AEncoding)
    {
      case encUTF8: return QStringConverter::Utf8;
      case encSystem: return QStringConverter::System;
      case encLatin1: return QStringConverter::Latin1;
      case encUTF16LE: return QStringConverter::Utf16LE;
      case encUTF16BE: return QStringConverter::Utf16BE;
    }
    return QStringConverter::Utf8;
  }

  // Decodes Data; false on invalid byte sequences
  static bool Decode(const QByteArray & Data, Encoding AEncoding, QString & Text)
  {
    QStringDecoder Decoder(ConverterEncoding(AEncoding), QStringDecoder::Flag::Stateless);
    Text = Decoder.decode(Data);
    return !Decoder.hasError();
  }

  // TEditorForm::LoadFromFile
  bool LoadFromFile(bool PrimaryEncoding, QString & Error)
  {
    QFile File(FileName);
    if (!File.open(QIODevice::ReadOnly))
    {
      // EDITOR_LOAD_ERROR
      Error = QStringLiteral("Error loading file '%1'.").arg(FileName) + QStringLiteral("\n\n") + File.errorString();
      return false;
    }
    QByteArray Data = File.readAll();
    File.close();

    bool CanTrySecondary = false;
    int BomLength = 0;
    if (!EncodingSet || WithBom)
    {
      WithBom = false;
      if (StartsWith(Data, "\xEF\xBB\xBF", 3))
      {
        FileEncoding = encUTF8;
        WithBom = true;
        BomLength = 3;
      }
      else if (StartsWith(Data, "\xFF\xFE", 2))
      {
        FileEncoding = encUTF16LE;
        WithBom = true;
        BomLength = 2;
      }
      else if (StartsWith(Data, "\xFE\xFF", 2))
      {
        FileEncoding = encUTF16BE;
        WithBom = true;
        BomLength = 2;
      }
      else if (!EncodingSet)
      {
        // Default encoding (Preferences, or the site's InternalEditorEncoding):
        // UTF-8 -> secondary system default (Latin-1 on UTF-8 systems);
        // system default -> secondary UTF-8 (Latin-1 on UTF-8 systems)
        int CodePage = (EncodingOverride >= 0) ? EncodingOverride : DefaultCodePage;
        if (CodePage == CodePageSystem)
        {
          FileEncoding = PrimaryEncoding ? encSystem : (SystemIsUtf8() ? encLatin1 : encUTF8);
        }
        else
        {
          FileEncoding = PrimaryEncoding ? encUTF8 : SecondaryEncoding();
        }
        CanTrySecondary = PrimaryEncoding;
      }
    }
    else if (StartsWith(Data, "\xEF\xBB\xBF", 3) && (FileEncoding == encUTF8))
    {
      WithBom = true;
      BomLength = 3;
    }

    QString Text;
    if (!Decode(Data.mid(BomLength), FileEncoding, Text))
    {
      // EDITOR_ENCODING_ERROR
      QString Message = QStringLiteral("Error loading file '%1' using '%2' encoding.").arg(FileName, EncodingName(FileEncoding));
      if (CanTrySecondary)
      {
        if (!LoadFromFile(false, Error))
        {
          return false;
        }
        EncodingSet = true;
        if (WarnOnEncodingFallback)
        {
          // EDITOR_ENCODING_REVERTED
          QMessageBox::information(Q->isVisible() ? Q : Q->parentWidget(), QStringLiteral("Information"),
            Message + QLatin1Char(' ') + QStringLiteral("Falling back to '%1' encoding.").arg(EncodingName(FileEncoding)));
        }
        return true;
      }
      if ((FileEncoding != encLatin1) && (FileEncoding != encSystem))
      {
        Error = Message;
        return false;
      }
    }
    EncodingSet = true;

    // Line endings: the prevailing one is kept on save
    qsizetype CRLF = Text.count(QStringLiteral("\r\n"));
    qsizetype CR = Text.count(QLatin1Char('\r')) - CRLF;
    qsizetype LF = Text.count(QLatin1Char('\n')) - CRLF;
    if ((CRLF > 0) && (CRLF >= LF) && (CRLF >= CR))
    {
      Eol = eolCRLF;
    }
    else if ((CR > 0) && (CR > LF))
    {
      Eol = eolCR;
    }
    else
    {
      Eol = eolLF;
    }
    Text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    Text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    FinalNewLine = Text.endsWith(QLatin1Char('\n'));
    if (FinalNewLine)
    {
      // as TStrings: the final line break does not make an extra empty line
      Text.chop(1);
    }

    Memo->setPlainText(Text);
    Memo->document()->setModified(false);
    return true;
  }

  static bool SystemIsUtf8()
  {
    QString System = SystemCodeset();
    return (System.compare(QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0) ||
           (System.compare(QStringLiteral("utf8"), Qt::CaseInsensitive) == 0);
  }

  static Encoding SecondaryEncoding()
  {
    return SystemIsUtf8() ? encLatin1 : encSystem;
  }

  static QString SystemCodeset()
  {
#ifdef Q_OS_WIN
    // The ANSI code page of Windows ("CP1252"...), as WinSCP's CP_ACP
    return QStringLiteral("CP%1").arg(GetACP());
#else
    const char * Codeset = nl_langinfo(CODESET);
    return (Codeset != nullptr) && (*Codeset != '\0') ? QString::fromLatin1(Codeset) : QStringLiteral("ANSI_X3.4-1968");
#endif
  }

  bool LoadFile()
  {
    QFileInfo Info(FileName);
    // WarnOrLargeFileSize / LargeFileSize (KB)
    if (Info.exists() && WarnOrLargeFileSize && (Info.size() > LargeFileSize) && !Loaded)
    {
      // INTERNAL_EDITOR_LARGE_FILE2
      if (QMessageBox::warning(Q->parentWidget(), QStringLiteral("Warning"),
            QStringLiteral("Try to open large file?\n\nThe file you are trying to open in an internal editor is too large (%1). "
              "NixSCP internal editor is not designed for editing large files. Consider using an external editor capable "
              "of editing large files.\n\nYou may try to open the file anyway, but NixSCP may fail.")
              .arg(QLocale().formattedDataSize(Info.size())),
            QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
      {
        return false;
      }
    }
    QString Error;
    if (!LoadFromFile(true, Error))
    {
      QMessageBox::critical(Q->isVisible() ? Q : Q->parentWidget(), QStringLiteral("Error"), Error);
      return false;
    }
    if (!ReadOnly && Info.exists() && !Info.isWritable())
    {
      ReadOnly = true;
    }
    Memo->setReadOnly(ReadOnly);
    UpdateControls();
    return true;
  }

  // TEditorForm::SaveToFile
  bool SaveToFile(bool Interactive = true)
  {
    QString Text = Memo->toPlainText();
    if (FinalNewLine)
    {
      Text += QLatin1Char('\n');
    }
    if (Eol == eolCRLF)
    {
      Text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    }
    else if (Eol == eolCR)
    {
      Text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    }
    QStringEncoder Encoder(ConverterEncoding(FileEncoding), QStringEncoder::Flag::Stateless);
    QByteArray Data = Encoder.encode(Text);
    if (Encoder.hasError() && Interactive)
    {
      if (QMessageBox::warning(Q, QStringLiteral("Warning"),
            QStringLiteral("Some characters cannot be represented in '%1' encoding and will be replaced. Continue?")
              .arg(EncodingName(FileEncoding)),
            QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
      {
        return false;
      }
    }
    if (WithBom)
    {
      switch (FileEncoding)
      {
        case encUTF8: Data.prepend("\xEF\xBB\xBF"); break;
        case encUTF16LE: Data.prepend("\xFF\xFE"); break;
        case encUTF16BE: Data.prepend("\xFE\xFF"); break;
        default: break;
      }
    }
    // Written in place (not replaced), so watchers of the file keep working
    QFile File(FileName);
    if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate) || (File.write(Data) != Data.size()) || !File.flush())
    {
      if (!Interactive)
      {
        return false;
      }
      QMessageBox::critical(Q, QStringLiteral("Error"),
        QStringLiteral("Error writing file '%1'.").arg(FileName) + QStringLiteral("\n\n") + File.errorString());
      return false;
    }
    File.close();
    return true;
  }

  // TEditorForm::SaveFile
  bool SaveFile()
  {
    if (!Memo->document()->isModified() || ReadOnly)
    {
      return false;
    }
    StateStatus->setText(QStringLiteral("Saving..."));   // EDITOR_SAVING
    StateStatus->repaint();
    bool Result = SaveToFile();
    if (Result)
    {
      Memo->document()->setModified(false);
      NewFile = false;
      emit Q->Saved(FileName);
    }
    UpdateControls();
    return Result;
  }

  // TEditorForm::ChangeEncoding
  void ChangeEncoding(Encoding AEncoding)
  {
    if ((FileEncoding == AEncoding) && !WithBom)
    {
      UpdateControls();
      return;
    }
    if (Memo->document()->isModified() &&
        (QMessageBox::question(Q, QStringLiteral("Confirm"),
          // EDITOR_MODIFIED_ENCODING
          QStringLiteral("File was modified. Changes will be lost, if file is reloaded using different encoding. Continue?"),
          QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Cancel))
    {
      UpdateControls();
      return;
    }
    Encoding PrevEncoding = FileEncoding;
    bool PrevBom = WithBom;
    FileEncoding = AEncoding;
    EncodingSet = true;
    WithBom = false;
    QString Error;
    if (!LoadFromFile(true, Error))
    {
      QMessageBox::critical(Q, QStringLiteral("Error"), Error);
      FileEncoding = PrevEncoding;
      WithBom = PrevBom;
      QString Ignored;
      LoadFromFile(true, Ignored);
    }
    UpdateControls();
  }

  // TEditorForm::Reload
  void Reload()
  {
    if (Memo->document()->isModified() &&
        (QMessageBox::question(Q, QStringLiteral("Confirm"),
          // EDITOR_MODIFIED_RELOAD
          QStringLiteral("File was modified. Changes will be lost, if file is reloaded. Continue?"),
          QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Cancel))
    {
      return;
    }
    if (OnFileReload)
    {
      // remote file: download it again first (ExecutedFileReload)
      Reloading = true;
      UpdateControls();
      QPointer<EditorWindow> Guard(Q);
      OnFileReload([this, Guard](bool Ok)
      {
        if (Guard.isNull())
        {
          return;
        }
        if (Ok)
        {
          LoadAgain();
        }
        Reloading = false;
        UpdateControls();
      });
      return;
    }
    LoadAgain();
  }

  // TEditorForm::LoadFile, keeping the position
  void LoadAgain()
  {
    bool WasReloading = Reloading;
    Reloading = true;
    QTextCursor Cursor = Memo->textCursor();
    int Position = Cursor.position();
    int Scroll = Memo->verticalScrollBar()->value();
    // keep the encoding chosen (the BOM is detected again)
    QString Error;
    if (!LoadFromFile(true, Error))
    {
      QMessageBox::critical(Q, QStringLiteral("Error"), Error);
    }
    else
    {
      Cursor = Memo->textCursor();
      Cursor.setPosition(std::min(Position, static_cast<int>(Memo->document()->characterCount() - 1)));
      Memo->setTextCursor(Cursor);
      Memo->verticalScrollBar()->setValue(Scroll);
    }
    Reloading = WasReloading;
    UpdateControls();
  }

  // TEditorForm::BackupSave: unsaved changes of a forced close go to <file>.bak[N]
  void BackupSave()
  {
    if (!Loaded || ReadOnly || !Memo->document()->isModified())
    {
      return;
    }
    int Uniq = 0;
    while (true)
    {
      QString BackupName = FileName + QStringLiteral(".bak") + ((Uniq == 0) ? QString() : QString::number(Uniq));
      if (!QFileInfo::exists(BackupName))
      {
        QString Saved = FileName;
        FileName = BackupName;
        SaveToFile(false);
        FileName = Saved;
        break;
      }
      Uniq++;
    }
  }

  // --- find & replace

  void StartFind(bool Find)
  {
    // TEditorForm::StartFind
    FindDialog * Dialog = Find ? FindDlg : ReplaceDlg;
    if (Dialog == nullptr)
    {
      Dialog = new FindDialog(Q, !Find);
      if (Find)
      {
        FindDlg = Dialog;
      }
      else
      {
        ReplaceDlg = Dialog;
      }
      connect(Dialog->FindNextButton, &QPushButton::clicked, this, [this, Dialog]() { DialogFind(Dialog, fmFindNext); });
      if (!Find)
      {
        connect(Dialog->ReplaceButton, &QPushButton::clicked, this, [this, Dialog]() { DialogFind(Dialog, fmReplace); });
        connect(Dialog->ReplaceAllButton, &QPushButton::clicked, this, [this, Dialog]() { DialogFind(Dialog, fmReplaceAll); });
      }
    }
    // hide the other one, as Windows does with a single dialog
    FindDialog * Other = Find ? ReplaceDlg : FindDlg;
    if (Other != nullptr)
    {
      Other->hide();
    }
    QString Text = Memo->textCursor().selectedText();
    if (Text.isEmpty() || Text.contains(QChar::ParagraphSeparator))
    {
      // word at the cursor
      QTextCursor Cursor = Memo->textCursor();
      Cursor.select(QTextCursor::WordUnderCursor);
      Text = Cursor.selectedText();
    }
    if (Text.isEmpty())
    {
      Text = FindText;
    }
    Dialog->FindEdit->setText(Text);
    if (Dialog->ReplaceEdit != nullptr)
    {
      Dialog->ReplaceEdit->setText(ReplaceText);
    }
    Dialog->MatchCaseCheck->setChecked(FindMatchCase);
    Dialog->WholeWordCheck->setChecked(FindWholeWord);
    if (Dialog->DownRadio != nullptr)
    {
      Dialog->DownRadio->setChecked(FindDown);
      Dialog->UpRadio->setChecked(!FindDown);
    }
    Dialog->FindEdit->selectAll();
    Dialog->show();
    Dialog->raise();
    Dialog->activateWindow();
    Dialog->FindEdit->setFocus();
  }

  void DialogFind(FindDialog * Dialog, FindMode Mode)
  {
    LastFindDialog = Dialog;
    FindText = Dialog->FindEdit->text();
    if (Dialog->ReplaceEdit != nullptr)
    {
      ReplaceText = Dialog->ReplaceEdit->text();
    }
    FindMatchCase = Dialog->MatchCaseCheck->isChecked();
    FindWholeWord = Dialog->WholeWordCheck->isChecked();
    // Replace dialog always searches down
    FindDown = (Dialog->DownRadio == nullptr) || Dialog->DownRadio->isChecked();
    SaveFindConfiguration();
    Find(Mode);
  }

  // TEditorForm::Find
  void Find(FindMode Mode)
  {
    if (FindText.isEmpty())
    {
      StartFind(true);
      return;
    }
    QTextDocument::FindFlags Flags;
    if (FindMatchCase) Flags |= QTextDocument::FindCaseSensitively;
    if (FindWholeWord) Flags |= QTextDocument::FindWholeWords;
    bool Down = FindDown || (Mode != fmFindNext) || (LastFindDialog == ReplaceDlg);
    if (!Down) Flags |= QTextDocument::FindBackward;

    int Replacements = 0;
    bool Found;
    QTextCursor EditBlock = Memo->textCursor();
    bool InEditBlock = (Mode == fmReplaceAll);
    if (InEditBlock)
    {
      EditBlock.beginEditBlock();
    }
    do
    {
      QTextCursor Cursor = Memo->textCursor();
      if ((Mode != fmFindNext) && !Memo->isReadOnly() && Cursor.hasSelection() &&
          (Cursor.selectedText().compare(FindText, FindMatchCase ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0))
      {
        Cursor.insertText(ReplaceText);
        Cursor.setPosition(Cursor.position() - static_cast<int>(ReplaceText.size()));
        Cursor.setPosition(Cursor.position() + static_cast<int>(ReplaceText.size()), QTextCursor::KeepAnchor);
        Memo->setTextCursor(Cursor);
        Replacements++;
        // continue after the replacement
        Cursor.setPosition(Cursor.position());
      }
      else
      {
        int Start = Cursor.selectionStart();
        Cursor.setPosition(Down ? (Cursor.hasSelection() ? Start + 1 : Start) : Start);
      }
      QTextCursor Match = Memo->document()->find(FindText, Cursor, Flags);
      Found = !Match.isNull();
      if (Found)
      {
        Memo->setTextCursor(Match);
      }
    }
    while (Found && (Mode == fmReplaceAll));
    if (InEditBlock)
    {
      EditBlock.endEditBlock();
    }
    Memo->ensureCursorVisible();

    if (!Found)
    {
      QWidget * MessageParent = ((LastFindDialog != nullptr) && LastFindDialog->isVisible()) ?
        static_cast<QWidget *>(LastFindDialog) : static_cast<QWidget *>(Q);
      if (Replacements == 0)
      {
        // EDITOR_FIND_END
        QMessageBox::information(MessageParent, QStringLiteral("Information"),
          QStringLiteral("Could not find the string '%1'.").arg(FindText));
      }
      else if (Mode == fmReplaceAll)
      {
        // EDITOR_REPLACE_END
        QMessageBox::information(MessageParent, QStringLiteral("Information"),
          QStringLiteral("Total replacements performed: %1").arg(Replacements));
      }
    }
    UpdateControls();
  }

  // TEditorForm::GoToLine
  void GoToLine()
  {
    bool Ok = false;
    QString Str = QInputDialog::getText(Q, QStringLiteral("Go to line"), QStringLiteral("Line number:"),
      QLineEdit::Normal, QString(), &Ok);
    if (!Ok)
    {
      return;
    }
    bool Valid = false;
    int Line = Str.trimmed().toInt(&Valid);
    if (!Valid || (Line <= 0) || (Line > Memo->document()->blockCount()))
    {
      QMessageBox::critical(Q, QStringLiteral("Error"), QStringLiteral("Invalid line number."));   // EDITOR_INVALID_LINE
      return;
    }
    QTextCursor Cursor(Memo->document()->findBlockByNumber(Line - 1));
    Memo->setTextCursor(Cursor);
    Memo->centerCursor();
  }

  // --- status

  void UpdateControls()
  {
    QTextCursor Cursor = Memo->textCursor();
    QTextBlock Block = Cursor.block();
    // EDITOR_LINE_STATUS
    LineStatus->setText(QStringLiteral("Line: %1/%2").arg(Block.blockNumber() + 1).arg(Memo->document()->blockCount()));
    QString Line = Block.text();
    int X = Cursor.positionInBlock();
    int Column = 0;
    for (int Index = 0; Index <= X; Index++)
    {
      if ((Index >= 1) && (Index - 1 < Line.size()) && (Line.at(Index - 1) == QLatin1Char('\t')))
      {
        Column = (((Column / TabSize) + 1) * TabSize) + 1;
      }
      else
      {
        Column++;
      }
    }
    ColumnStatus->setText(QStringLiteral("Column: %1").arg(Column));   // EDITOR_COLUMN_STATUS
    QString Character;
    if (X < Line.size())
    {
      char32_t Code = Line.at(X).unicode();
      if (Line.at(X).isHighSurrogate() && (X + 1 < Line.size()))
      {
        Code = QChar::surrogateToUcs4(Line.at(X), Line.at(X + 1));
      }
      int Value = static_cast<int>(Code);
      if ((FileEncoding == encLatin1) || (FileEncoding == encSystem))
      {
        QStringEncoder Encoder(ConverterEncoding(FileEncoding), QStringEncoder::Flag::Stateless);
        QByteArray Bytes = Encoder.encode(QString::fromUcs4(&Code, 1));
        Value = (!Encoder.hasError() && (Bytes.size() == 1)) ? static_cast<unsigned char>(Bytes.at(0)) : -1;
      }
      if (Value >= 0)
      {
        // EDITOR_CHARACTER_STATUS2
        Character = QStringLiteral("Character: %1 (0x%2)").arg(Value).arg(Value, 2, 16, QLatin1Char('0'));
      }
    }
    CharacterStatus->setText(Character);
    QString EncodingStr = EncodingName(FileEncoding);
    if (WithBom)
    {
      EncodingStr += QStringLiteral(" (BOM)");
    }
    EncodingStatus->setText(QStringLiteral("Encoding: %1").arg(EncodingStr));   // EDITOR_ENCODING_STATUS
    EolStatus->setText((Eol == eolCRLF) ? QStringLiteral("CRLF") : ((Eol == eolCR) ? QStringLiteral("CR") : QStringLiteral("LF")));
    QString State;
    if (ReadOnly)
    {
      State = QStringLiteral("Read-only");    // EDITOR_READONLY
    }
    else if (NewFile)
    {
      State = QStringLiteral("New");          // EDITOR_NEW
    }
    else if (Memo->document()->isModified())
    {
      State = QStringLiteral("Modified");     // EDITOR_MODIFIED
    }
    StateStatus->setText(State);

    bool Modified = Memo->document()->isModified();
    SaveAction->setEnabled(Modified && !ReadOnly);
    bool AnyModified = false;
    for (EditorWindow * Editor : EditorInstances)
    {
      AnyModified = AnyModified || (Editor->IsModified() && !Editor->d->ReadOnly);
    }
    for (EditorWindow * Editor : EditorInstances)
    {
      if (Editor->d->SaveAllAction != nullptr)
      {
        Editor->d->SaveAllAction->setEnabled(AnyModified);
      }
    }
    ReplaceAction->setEnabled(!ReadOnly);
    FindNextAction->setEnabled((LastFindDialog != nullptr) || !FindText.isEmpty());
    ReloadAction->setEnabled(!Reloading && !NewFile);
    EncodingMenu->setEnabled(!NewFile);
    bool HasSelection = Cursor.hasSelection();
    CutAction->setEnabled(HasSelection && !ReadOnly);
    CopyAction->setEnabled(HasSelection);
    DeleteAction->setEnabled(HasSelection && !ReadOnly);
    PasteAction->setEnabled(!ReadOnly && Memo->canPaste());
    UndoAction->setEnabled(Memo->document()->isUndoAvailable() && !ReadOnly);
    RedoAction->setEnabled(Memo->document()->isRedoAvailable() && !ReadOnly);
    UTF8EncodingAction->setChecked(FileEncoding == encUTF8);
    DefaultEncodingAction->setChecked(FileEncoding == encSystem);
    Latin1EncodingAction->setChecked(FileEncoding == encLatin1);
    LineNumbersAction->setChecked(Memo->ShowLineNumbers());

    QString Caption = DisplayName.isEmpty() ? FileName : DisplayName;
    Q->setWindowTitle(QStringLiteral("%1[*] - Editor - NixSCP").arg(Caption));   // EDITOR_CAPTION
    Q->setWindowModified(Modified);
  }

  void ShowContextMenu(const QPoint & Pos)
  {
    UpdateControls();
    QMenu Menu(Q);
    Menu.addAction(UndoAction);
    Menu.addAction(RedoAction);
    Menu.addSeparator();
    Menu.addAction(CutAction);
    Menu.addAction(CopyAction);
    Menu.addAction(PasteAction);
    Menu.addAction(DeleteAction);
    Menu.addSeparator();
    Menu.addAction(SelectAllAction);
    Menu.addSeparator();
    Menu.addAction(FindAction);
    Menu.addAction(ReplaceAction);
    Menu.addAction(FindNextAction);
    Menu.addAction(GoToLineAction);
    Menu.addSeparator();
    Menu.addMenu(EncodingMenu);
    Menu.addAction(LineNumbersAction);
    Menu.addSeparator();
    Menu.addAction(SaveAction);
    Menu.addAction(ReloadAction);
    Menu.exec(Memo->viewport()->mapToGlobal(Pos));
  }
};

QString EditorWindow::EncodingName(Encoding AEncoding)
{
  switch (AEncoding)
  {
    case encUTF8: return QStringLiteral("UTF-8");   // UTF8_NAME
    case encSystem: return Private::SystemCodeset();
    case encLatin1: return QStringLiteral("ISO-8859-1");
    case encUTF16LE: return QStringLiteral("UTF-16LE");
    case encUTF16BE: return QStringLiteral("UTF-16BE");
  }
  return QString();
}

EditorWindow::EditorWindow(const QString & LocalFileName, const QString & DisplayName, QWidget * Parent,
    int EncodingOverride) :
  QMainWindow(Parent, Qt::Window),
  d(new Private)
{
  d->Q = this;
  d->FileName = LocalFileName;
  d->DisplayName = DisplayName;
  d->EncodingOverride = EncodingOverride;
  setAttribute(Qt::WA_DeleteOnClose);
  resize(700, 500);
  EditorInstances.append(this);

  d->Memo = new EditorMemo(this);
  d->Memo->setContextMenuPolicy(Qt::CustomContextMenu);
  d->Memo->installEventFilter(this);
  setCentralWidget(d->Memo);

  // EditorActions
  d->SaveAction = d->CreateAction(QStringLiteral("&Save"), QStringLiteral("Save file"), QStringLiteral("document-save"),
    QStyle::SP_DialogSaveButton, QKeySequence(Qt::CTRL | Qt::Key_S));
  d->SaveAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_S), QKeySequence(Qt::Key_F2)});
  d->SaveAllAction = d->CreateAction(QStringLiteral("Save &All"), QStringLiteral("Save files in all editors"),
    QStringLiteral("document-save-all"), QStyle::SP_DialogSaveButton, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  d->ReloadAction = d->CreateAction(QStringLiteral("&Reload"), QStringLiteral("Reload file"), QStringLiteral("view-refresh"),
    QStyle::SP_BrowserReload, QKeySequence(Qt::CTRL | Qt::Key_R));
  d->ReloadAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5)});
  d->CopyAction = d->CreateAction(QStringLiteral("&Copy"), QStringLiteral("Copy the selection and put it on the Clipboard"),
    QStringLiteral("edit-copy"), QStyle::SP_CustomBase, QKeySequence());
  d->CutAction = d->CreateAction(QStringLiteral("Cu&t"), QStringLiteral("Cut the selection and put it on the Clipboard"),
    QStringLiteral("edit-cut"), QStyle::SP_CustomBase, QKeySequence());
  d->PasteAction = d->CreateAction(QStringLiteral("&Paste"), QStringLiteral("Insert Clipboard contents"),
    QStringLiteral("edit-paste"), QStyle::SP_CustomBase, QKeySequence());
  d->DeleteAction = d->CreateAction(QStringLiteral("&Delete"), QStringLiteral("Erase the selection"),
    QStringLiteral("edit-delete"), QStyle::SP_CustomBase, QKeySequence());
  d->SelectAllAction = d->CreateAction(QStringLiteral("Select &All"), QStringLiteral("Select the entire document"),
    QStringLiteral("edit-select-all"), QStyle::SP_CustomBase, QKeySequence());
  d->UndoAction = d->CreateAction(QStringLiteral("&Undo"), QStringLiteral("Revert the last action"),
    QStringLiteral("edit-undo"), QStyle::SP_ArrowBack, QKeySequence());
  d->RedoAction = d->CreateAction(QStringLiteral("Re&do"), QStringLiteral("Revert the effects of most recent Undo"),
    QStringLiteral("edit-redo"), QStyle::SP_ArrowForward, QKeySequence());
  // the editor control handles the standard editing keys itself; show them in menus only
  d->CopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
  d->CutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_X));
  d->PasteAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_V));
  d->SelectAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
  d->UndoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
  d->RedoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y));
  for (QAction * Action : { d->CopyAction, d->CutAction, d->PasteAction, d->SelectAllAction, d->UndoAction, d->RedoAction, d->DeleteAction })
  {
    Action->setShortcutContext(Qt::WidgetShortcut);   // not triggered (the memo is the focus)
  }
  d->FindAction = d->CreateAction(QStringLiteral("&Find..."), QStringLiteral("Find the specified text"),
    QStringLiteral("edit-find"), QStyle::SP_FileDialogContentsView, QKeySequence(Qt::CTRL | Qt::Key_F));
  d->ReplaceAction = d->CreateAction(QStringLiteral("&Replace..."), QStringLiteral("Replace the specified text with different text"),
    QStringLiteral("edit-find-replace"), QStyle::SP_FileDialogDetailedView, QKeySequence(Qt::CTRL | Qt::Key_H));
  d->FindNextAction = d->CreateAction(QStringLiteral("Find &Next"), QStringLiteral("Find the next occurrence of the specified text"),
    QStringLiteral("go-down-search"), QStyle::SP_ArrowDown, QKeySequence(Qt::Key_F3));
  d->GoToLineAction = d->CreateAction(QStringLiteral("&Go to Line Number..."), QStringLiteral("Go to specific line number"),
    QStringLiteral("go-jump"), QStyle::SP_ArrowRight, QKeySequence(Qt::CTRL | Qt::Key_G));
  // SecondaryShortCuts of Editor.dfm
  d->FindAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_F), QKeySequence(Qt::Key_F7)});
  d->ReplaceAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_H), QKeySequence(Qt::CTRL | Qt::Key_F7)});
  d->FindNextAction->setShortcuts({QKeySequence(Qt::Key_F3), QKeySequence(Qt::SHIFT | Qt::Key_F7)});
  d->GoToLineAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_G), QKeySequence(Qt::ALT | Qt::Key_F8)});
  d->PreferencesAction = d->CreateAction(QStringLiteral("&Preferences..."), QStringLiteral("Show/change editor preferences"),
    QStringLiteral("preferences-system"), QStyle::SP_FileDialogInfoView, QKeySequence());
  d->ColorAction = d->CreateAction(QStringLiteral("C&olor"), QStringLiteral("Change color of editor"),
    QStringLiteral("color-management"), QStyle::SP_CustomBase, QKeySequence());
  d->HelpAction = d->CreateAction(QStringLiteral("&Help"), QStringLiteral("Editor help"),
    QStringLiteral("help-contents"), QStyle::SP_DialogHelpButton, QKeySequence());
  d->ColorMenu = new QMenu(QStringLiteral("C&olor"), this);
  d->DefaultColorAction = d->ColorMenu->addAction(QStringLiteral("&Default"));   // COLOR_DEFAULT-like
  d->DefaultColorAction->setCheckable(true);
  d->CustomColorAction = d->ColorMenu->addAction(QStringLiteral("&More Colors..."));
  d->ColorAction->setMenu(d->ColorMenu);
  d->LineNumbersAction = d->CreateAction(QStringLiteral("&Line Numbers"), QStringLiteral("Show line numbers"),
    QString(), QStyle::SP_CustomBase, QKeySequence());
  d->LineNumbersAction->setCheckable(true);

  d->EncodingMenu = new QMenu(QStringLiteral("Encoding"), this);
  QActionGroup * EncodingGroup = new QActionGroup(this);
  d->DefaultEncodingAction = d->EncodingMenu->addAction(
    QStringLiteral("Default (%1)").arg(EncodingName(encSystem)));
  d->DefaultEncodingAction->setStatusTip(QStringLiteral("Default system encoding (%1)").arg(EncodingName(encSystem)));
  d->UTF8EncodingAction = d->EncodingMenu->addAction(QStringLiteral("UTF-8"));
  d->UTF8EncodingAction->setStatusTip(QStringLiteral("UTF-8 encoding"));
  d->Latin1EncodingAction = d->EncodingMenu->addAction(QStringLiteral("ISO-8859-1 (Latin-1)"));
  d->Latin1EncodingAction->setStatusTip(QStringLiteral("ISO-8859-1 encoding"));
  for (QAction * Action : { d->DefaultEncodingAction, d->UTF8EncodingAction, d->Latin1EncodingAction })
  {
    Action->setCheckable(true);
    EncodingGroup->addAction(Action);
  }

  QToolBar * ToolBar = addToolBar(QStringLiteral("ToolBar"));
  ToolBar->setObjectName(QStringLiteral("EditorToolBar"));
  ToolBar->setMovable(false);
  ToolBar->addAction(d->SaveAction);
  ToolBar->addAction(d->SaveAllAction);
  ToolBar->addAction(d->ReloadAction);
  ToolBar->addSeparator();
  ToolBar->addAction(d->CopyAction);
  ToolBar->addAction(d->CutAction);
  ToolBar->addAction(d->PasteAction);
  ToolBar->addAction(d->DeleteAction);
  ToolBar->addAction(d->SelectAllAction);
  ToolBar->addSeparator();
  ToolBar->addAction(d->UndoAction);
  ToolBar->addAction(d->RedoAction);
  ToolBar->addSeparator();
  ToolBar->addAction(d->FindAction);
  ToolBar->addAction(d->ReplaceAction);
  ToolBar->addAction(d->FindNextAction);
  ToolBar->addAction(d->GoToLineAction);
  ToolBar->addSeparator();
  QToolButton * EncodingButton = new QToolButton(ToolBar);
  EncodingButton->setText(QStringLiteral("Encoding"));
  EncodingButton->setToolTip(QStringLiteral("Change file encoding"));
  EncodingButton->setMenu(d->EncodingMenu);
  EncodingButton->setPopupMode(QToolButton::InstantPopup);
  EncodingButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  ToolBar->addWidget(EncodingButton);
  QToolButton * ColorButton = new QToolButton(ToolBar);
  ColorButton->setDefaultAction(d->ColorAction);
  ColorButton->setPopupMode(QToolButton::InstantPopup);
  ToolBar->addWidget(ColorButton);
  ToolBar->addAction(d->PreferencesAction);
  ToolBar->addSeparator();
  ToolBar->addAction(d->HelpAction);

  // Status bar panels
  QStatusBar * Status = statusBar();
  d->LineStatus = new QLabel(Status);
  d->ColumnStatus = new QLabel(Status);
  d->CharacterStatus = new QLabel(Status);
  d->EncodingStatus = new QLabel(Status);
  d->EolStatus = new QLabel(Status);
  d->StateStatus = new QLabel(Status);
  for (QLabel * Label : { d->LineStatus, d->ColumnStatus, d->CharacterStatus, d->EncodingStatus })
  {
    Label->setMinimumWidth(150);
    Status->addWidget(Label);
  }
  d->EolStatus->setToolTip(QStringLiteral("Line endings (kept on save)"));
  Status->addWidget(d->EolStatus);
  Status->addWidget(d->StateStatus, 1);

  connect(d->SaveAction, &QAction::triggered, d, [this]() { d->SaveFile(); });
  connect(d->SaveAllAction, &QAction::triggered, d, []() { EditorWindow::SaveAll(); });
  connect(d->ReloadAction, &QAction::triggered, d, [this]() { d->Reload(); });
  connect(d->PreferencesAction, &QAction::triggered, d, [this]()
  {
    // PreferencesDialog(pmEditorInternal); Configuration::Changed re-applies the preferences
    PreferencesDialog::Execute(this, QStringLiteral("Internal editor"));
  });
  // FormHelp: F1 and Help > Help open the editor's page
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_editor"));
  connect(d->HelpAction, &QAction::triggered, d, []() { HelpSystem::ShowHelp(QStringLiteral("ui_editor")); });
  connect(d->DefaultColorAction, &QAction::triggered, d, [this]() { SetBackgroundColor(QColor()); });
  connect(d->CustomColorAction, &QAction::triggered, d, [this]()
  {
    QColor Color = QColorDialog::getColor(BackgroundColor().isValid() ? BackgroundColor() : d->Memo->palette().color(QPalette::Base),
      this, QStringLiteral("Color"));
    if (Color.isValid())
    {
      SetBackgroundColor(Color);
    }
  });
  connect(d->ColorMenu, &QMenu::aboutToShow, d, [this]() { d->DefaultColorAction->setChecked(!d->CustomBackground.isValid()); });
  connect(&Configuration::Instance(), &Configuration::Changed, this, &EditorWindow::ApplyConfiguration);
  connect(d->CopyAction, &QAction::triggered, d->Memo, &QPlainTextEdit::copy);
  connect(d->CutAction, &QAction::triggered, d->Memo, &QPlainTextEdit::cut);
  connect(d->PasteAction, &QAction::triggered, d->Memo, &QPlainTextEdit::paste);
  connect(d->DeleteAction, &QAction::triggered, d, [this]() { d->Memo->textCursor().removeSelectedText(); });
  connect(d->SelectAllAction, &QAction::triggered, d->Memo, &QPlainTextEdit::selectAll);
  connect(d->UndoAction, &QAction::triggered, d->Memo, &QPlainTextEdit::undo);
  connect(d->RedoAction, &QAction::triggered, d->Memo, &QPlainTextEdit::redo);
  connect(d->FindAction, &QAction::triggered, d, [this]() { d->StartFind(true); });
  connect(d->ReplaceAction, &QAction::triggered, d, [this]() { d->StartFind(false); });
  connect(d->FindNextAction, &QAction::triggered, d, [this]()
  {
    if (d->LastFindDialog == nullptr)
    {
      d->LastFindDialog = d->FindDlg;
    }
    d->Find(Private::fmFindNext);
  });
  connect(d->GoToLineAction, &QAction::triggered, d, [this]() { d->GoToLine(); });
  connect(d->LineNumbersAction, &QAction::toggled, d, [this](bool Checked) { d->Memo->SetShowLineNumbers(Checked); });
  connect(d->DefaultEncodingAction, &QAction::triggered, d, [this]() { d->ChangeEncoding(encSystem); });
  connect(d->UTF8EncodingAction, &QAction::triggered, d, [this]() { d->ChangeEncoding(encUTF8); });
  connect(d->Latin1EncodingAction, &QAction::triggered, d, [this]() { d->ChangeEncoding(encLatin1); });
  connect(d->Memo, &QPlainTextEdit::cursorPositionChanged, d, [this]() { d->UpdateControls(); });
  connect(d->Memo, &QPlainTextEdit::selectionChanged, d, [this]() { d->UpdateControls(); });
  connect(d->Memo->document(), &QTextDocument::modificationChanged, d, [this]() { d->UpdateControls(); });
  connect(d->Memo->document(), &QTextDocument::undoAvailable, d, [this]() { d->UpdateControls(); });
  connect(d->Memo->document(), &QTextDocument::redoAvailable, d, [this]() { d->UpdateControls(); });
  connect(d->Memo, &QWidget::customContextMenuRequested, d, [this](const QPoint & Pos) { d->ShowContextMenu(Pos); });

  d->ApplyEditorPreferences();
  d->LoadConfiguration();
  d->Loaded = d->LoadFile();
  d->UpdateControls();
}

EditorWindow::~EditorWindow()
{
  EditorInstances.removeAll(this);
  // an editor destroyed without being closed (e.g. with its parent window)
  if (!d->Closing && d->Loaded)
  {
    d->BackupSave();
    d->Closing = true;
    emit Closed(d->FileName);
  }
  delete d;
}

QList<EditorWindow *> EditorWindow::Instances()
{
  return EditorInstances;
}

void EditorWindow::SaveAll()
{
  // TCustomScpExplorerForm::SaveAllInternalEditors
  const QList<EditorWindow *> Editors = EditorInstances;
  for (EditorWindow * Editor : Editors)
  {
    if (Editor->IsModified())
    {
      Editor->Save();
    }
  }
}

void EditorWindow::SetBackgroundColor(const QColor & Color)
{
  d->CustomBackground = Color;
  d->UpdateColors();
}

QColor EditorWindow::BackgroundColor() const
{
  return d->CustomBackground;
}

void EditorWindow::SetReloadHandler(ReloadHandler Handler)
{
  d->OnFileReload = std::move(Handler);
}

void EditorWindow::Reload()
{
  d->Reload();
}

void EditorWindow::ApplyConfiguration()
{
  d->ApplyEditorPreferences();
  d->UpdateControls();
}

void EditorWindow::ForceClose()
{
  if (d->Closing)
  {
    return;
  }
  d->BackupSave();
  d->Closing = true;
  d->SaveWindowConfiguration();
  if (d->FindDlg != nullptr) d->FindDlg->hide();
  if (d->ReplaceDlg != nullptr) d->ReplaceDlg->hide();
  emit Closed(d->FileName);
  // closeEvent sees Closing and does not ask any more
  close();
}

bool EditorWindow::IsLoaded() const
{
  return d->Loaded;
}

QString EditorWindow::LocalFileName() const
{
  return d->FileName;
}

bool EditorWindow::IsModified() const
{
  return d->Memo->document()->isModified();
}

EditorWindow::Encoding EditorWindow::FileEncoding() const
{
  return d->FileEncoding;
}

EditorWindow::LineEnding EditorWindow::FileLineEnding() const
{
  return d->Eol;
}

bool EditorWindow::Save()
{
  return d->SaveFile();
}

void EditorWindow::SetShowLineNumbers(bool Show)
{
  d->Memo->SetShowLineNumbers(Show);
  d->UpdateControls();
}

void EditorWindow::SetNewFile(bool NewFile)
{
  d->NewFile = NewFile;
  d->UpdateControls();
}

void EditorWindow::SetReadOnly(bool ReadOnly)
{
  d->ReadOnly = ReadOnly;
  d->Memo->setReadOnly(ReadOnly);
  d->UpdateControls();
}

bool EditorWindow::eventFilter(QObject * Watched, QEvent * Event)
{
  if ((Watched == d->Memo) && (Event->type() == QEvent::KeyPress))
  {
    QKeyEvent * Key = static_cast<QKeyEvent *>(Event);
    // TEditorForm::FormKeyDown
    if (((Key->key() == Qt::Key_Escape) || (Key->key() == Qt::Key_F10)) && (Key->modifiers() == Qt::NoModifier))
    {
      close();
      return true;
    }
    // Qt text controls treat Ctrl+H as backspace on some platforms
    if ((Key->key() == Qt::Key_H) && (Key->modifiers() == Qt::ControlModifier))
    {
      d->ReplaceAction->trigger();
      return true;
    }
  }
  else if ((Watched == d->Memo) && (Event->type() == QEvent::ShortcutOverride))
  {
    QKeyEvent * Key = static_cast<QKeyEvent *>(Event);
    if ((Key->key() == Qt::Key_H) && (Key->modifiers() == Qt::ControlModifier))
    {
      // let the shortcut through instead of the control's backspace
      Event->ignore();
      return true;
    }
  }
  return QMainWindow::eventFilter(Watched, Event);
}

void EditorWindow::closeEvent(QCloseEvent * Event)
{
  if (d->Closing)
  {
    // ForceClose
    QMainWindow::closeEvent(Event);
    return;
  }
  // TEditorForm::FormCloseQuery
  if (d->Loaded && IsModified() && !d->ReadOnly)
  {
    activateWindow();
    QMessageBox::StandardButton Answer = QMessageBox::question(this, QStringLiteral("Confirm"),
      QStringLiteral("File was modified. Save changes?"),   // SAVE_CHANGES
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (Answer == QMessageBox::Cancel)
    {
      Event->ignore();
      return;
    }
    if ((Answer == QMessageBox::Yes) && !d->SaveFile())
    {
      Event->ignore();
      return;
    }
  }
  d->Closing = true;
  d->SaveWindowConfiguration();
  if (d->FindDlg != nullptr) d->FindDlg->hide();
  if (d->ReplaceDlg != nullptr) d->ReplaceDlg->hide();
  emit Closed(d->FileName);
  QMainWindow::closeEvent(Event);
}
