// Port of WinSCP source/forms/SelectMask.{cpp,dfm} and source/forms/EditMask.{cpp,dfm}
#include "gui/EditMaskDialog.h"
#include "gui/HelpSystem.h"
#include "gui/HistoryComboBox.h"

#include "core/Configuration.h"
#include "core/FileMasks.h"
#include "core/IniStorage.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

static const QString AnyMask = QStringLiteral("*.*");

//---------------------------------------------------------------------------
// Configuration helpers
//---------------------------------------------------------------------------

QStringList EditMaskDialog::History(const QString & Key)
{
  return HistoryComboBox::History(Key);
}

void EditMaskDialog::SaveToHistory(const QString & Key, const QString & Value)
{
  HistoryComboBox::SaveToHistory(Key, Value);
}

void EditMaskDialog::LoadHistoryCombo(QComboBox * Combo, const QString & Key)
{
  HistoryComboBox::LoadHistoryCombo(Combo, Key);
}

static QString ReadInterfaceString(const QString & Name, const QString & Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), false))
  {
    return Storage.ReadString(Name, Default);
  }
  return Default;
}

static bool ReadInterfaceBool(const QString & Name, bool Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), false))
  {
    return Storage.ReadBool(Name, Default);
  }
  return Default;
}

static IniStorage OpenInterface(bool & Ok)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  Ok = Storage.OpenSubKey(QStringLiteral("Configuration"), true) && Storage.OpenSubKey(QStringLiteral("Interface"), true);
  return Storage;
}

QString EditMaskDialog::SelectMask()
{
  return ReadInterfaceString(QStringLiteral("SelectMask"), AnyMask);
}

void EditMaskDialog::SetSelectMask(const QString & Mask)
{
  bool Ok;
  IniStorage Storage = OpenInterface(Ok);
  if (Ok)
  {
    Storage.WriteString(QStringLiteral("SelectMask"), Mask);
  }
}

bool EditMaskDialog::SelectDirectories()
{
  return ReadInterfaceBool(QStringLiteral("SelectDirectories"), false);
}

void EditMaskDialog::SetSelectDirectories(bool Value)
{
  bool Ok;
  IniStorage Storage = OpenInterface(Ok);
  if (Ok)
  {
    Storage.WriteBool(QStringLiteral("SelectDirectories"), Value);
  }
}

QString EditMaskDialog::MaskHints(bool PathMask, bool DirectoryMask)
{
  QStringList Parts;
  // MASK_HINT2
  Parts << QStringLiteral("* matches any number of characters.\n? matches exactly one character.\n"
    "[abc] matches one character from the set.\n[a-z] matches one character from the range.\nExample: *.html; photo??.png");
  // FILE_MASK_EX_HINT
  Parts << QStringLiteral(">size matches file larger than size\n<size matches file smaller than size\n"
    ">yyyy-mm-dd matches file modified after the date\n<yyyy-mm-dd matches file modified before the date\n"
    "Example: *.zip>1G; <2012-01-21");
  // COMBINING_MASKS_HINT
  Parts << QStringLiteral("Masks are separated by semicolon or comma.\nPlace exclude masks after pipe.");
  if (PathMask)
  {
    // PATH_MASK_HINT2
    Parts << QStringLiteral("Mask can be extended with path mask.\nExample: */public_html/*.html");
  }
  if (DirectoryMask)
  {
    // DIRECTORY_MASK_HINT
    Parts << QStringLiteral("Mask ending with slash selects directories.");
  }
  // MASK_HELP
  Parts << QStringLiteral("See help for more options.");
  return Parts.join(QStringLiteral("\n\n"));
}

//---------------------------------------------------------------------------
// Validation / composition
//---------------------------------------------------------------------------

bool EditMaskDialog::ValidateMask(QWidget * Parent, const QString & Mask, QComboBox * Combo)
{
  // ValidateMaskEdit (windows/Tools.cpp)
  try
  {
    FileMasks Masks(Mask);
    return true;
  }
  catch (const FileMasksException & E)
  {
    QMessageBox::critical(Parent, QStringLiteral("Error"), E.Message());
    if ((Combo != nullptr) && (Combo->lineEdit() != nullptr))
    {
      Combo->setFocus();
      Combo->lineEdit()->setSelection(E.ErrorStart, E.ErrorLen);
    }
    return false;
  }
}

static const QString AllFileMasksDelimiters = QStringLiteral(";,|");
static const QString DirectoryMaskDelimiters = QStringLiteral("/\\");
static const QString FileMasksDelimiterStr = QStringLiteral("; ");

// TFileMasks::MakeDirectoryMask
static QString MakeDirectoryMask(QString Str)
{
  if (Str.isEmpty() || !DirectoryMaskDelimiters.contains(Str.back()))
  {
    int D = -1;
    for (int I = Str.size() - 1; I >= 0; I--)
    {
      if (DirectoryMaskDelimiters.contains(Str.at(I)))
      {
        D = I;
        break;
      }
    }
    Str += (D >= 0) ? Str.at(D) : DirectoryMaskDelimiters.at(0);
  }
  return Str;
}

static void AddToList(QString & List, const QString & Value, const QString & Delimiter)
{
  if (!Value.isEmpty())
  {
    if (!List.isEmpty())
    {
      List += Delimiter;
    }
    List += Value;
  }
}

// TFileMasks::ComposeMaskStr(TStrings *, bool)
static QString ComposeMaskList(const QStringList & MasksStr, bool Directory)
{
  QString Result;
  QString ResultNoDirMask;
  for (const QString & Line : MasksStr)
  {
    QString Str = Line.trimmed();
    if (Str.isEmpty())
    {
      continue;
    }
    for (int P = 0; P < Str.size(); P++)
    {
      if (AllFileMasksDelimiters.contains(Str.at(P)))
      {
        Str.insert(P, Str.at(P));
        P++;
      }
    }
    QString StrNoDirMask;
    if (Directory)
    {
      StrNoDirMask = Str;
      Str = MakeDirectoryMask(Str);
    }
    else
    {
      while (!Str.isEmpty() && DirectoryMaskDelimiters.contains(Str.back()))
      {
        Str.chop(1);
      }
      StrNoDirMask = Str;
    }
    AddToList(Result, Str, FileMasksDelimiterStr);
    AddToList(ResultNoDirMask, StrNoDirMask, FileMasksDelimiterStr);
  }
  // For directories the slash added above would circumvent validation of
  // size and time masks; validate the masks without it
  FileMasks Temp;
  Temp = ResultNoDirMask;
  return Result;
}

QString EditMaskDialog::ComposeMaskStr(const QStringList & IncludeFileMasks, const QStringList & ExcludeFileMasks,
  const QStringList & IncludeDirectoryMasks, const QStringList & ExcludeDirectoryMasks)
{
  QString IncludeMasks = ComposeMaskList(IncludeFileMasks, false);
  AddToList(IncludeMasks, ComposeMaskList(IncludeDirectoryMasks, true), FileMasksDelimiterStr);
  QString ExcludeMasks = ComposeMaskList(ExcludeFileMasks, false);
  AddToList(ExcludeMasks, ComposeMaskList(ExcludeDirectoryMasks, true), FileMasksDelimiterStr);

  QString Result = IncludeMasks;
  if (!ExcludeMasks.isEmpty())
  {
    if (!Result.isEmpty())
    {
      Result += QLatin1Char(' ');
    }
    Result += QStringLiteral("| ") + ExcludeMasks;
  }
  return Result;
}

//---------------------------------------------------------------------------
// "mask hints" label
//---------------------------------------------------------------------------

namespace
{

// HintLabel (windows/Tools.cpp): link-like label showing a hint on hover/click
class HintLabel : public QLabel
{
public:
  HintLabel(const QString & Caption, const QString & Hint, QWidget * Parent) :
    QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(Caption.toHtmlEscaped()), Parent), FHint(Hint)
  {
    setToolTip(Hint);
    setTextFormat(Qt::RichText);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(this, &QLabel::linkActivated, this, [this]()
    {
      QToolTip::showText(mapToGlobal(QPoint(0, height())), FHint, this);
    });
  }

private:
  QString FHint;
};

// TEditMaskDialog
class MaskEditorDialog : public QDialog
{
public:
  explicit MaskEditorDialog(QWidget * Parent) : QDialog(Parent)
  {
    setWindowTitle(QStringLiteral("Edit file mask"));
    QVBoxLayout * Layout = new QVBoxLayout(this);

    QGroupBox * FilesGroup = new QGroupBox(QStringLiteral("Files masks"), this);
    QGridLayout * FilesLayout = new QGridLayout(FilesGroup);
    IncludeFileMasksMemo = CreateMemo(FilesGroup);
    ExcludeFileMasksMemo = CreateMemo(FilesGroup);
    FilesLayout->addWidget(CreateLabel(QStringLiteral("&Include files:"), IncludeFileMasksMemo, FilesGroup), 0, 0);
    FilesLayout->addWidget(CreateLabel(QStringLiteral("&Exclude files:"), ExcludeFileMasksMemo, FilesGroup), 0, 1);
    FilesLayout->addWidget(IncludeFileMasksMemo, 1, 0);
    FilesLayout->addWidget(ExcludeFileMasksMemo, 1, 1);
    Layout->addWidget(FilesGroup, 1);

    QGroupBox * DirectoriesGroup = new QGroupBox(QStringLiteral("Directories masks"), this);
    QGridLayout * DirectoriesLayout = new QGridLayout(DirectoriesGroup);
    IncludeDirectoryMasksMemo = CreateMemo(DirectoriesGroup);
    ExcludeDirectoryMasksMemo = CreateMemo(DirectoriesGroup);
    DirectoriesLayout->addWidget(CreateLabel(QStringLiteral("I&nclude directories:"), IncludeDirectoryMasksMemo, DirectoriesGroup), 0, 0);
    DirectoriesLayout->addWidget(CreateLabel(QStringLiteral("E&xclude directories:"), ExcludeDirectoryMasksMemo, DirectoriesGroup), 0, 1);
    DirectoriesLayout->addWidget(IncludeDirectoryMasksMemo, 1, 0);
    DirectoriesLayout->addWidget(ExcludeDirectoryMasksMemo, 1, 1);
    ExcludeDirectoryAllCheck = new QCheckBox(QStringLiteral("&All (do not recurse)"), DirectoriesGroup);
    DirectoriesLayout->addWidget(ExcludeDirectoryAllCheck, 2, 1);
    Layout->addWidget(DirectoriesGroup, 1);

    QHBoxLayout * HintLayout = new QHBoxLayout();
    HintLayout->addStretch(1);
    HintLayout->addWidget(new HintLabel(QStringLiteral("mask hints"), EditMaskDialog::MaskHints(true, false), this));
    Layout->addLayout(HintLayout);

    QGroupBox * MaskGroup = new QGroupBox(QStringLiteral("Mask"), this);
    QVBoxLayout * MaskLayout = new QVBoxLayout(MaskGroup);
    MaskMemo = new QPlainTextEdit(MaskGroup);
    MaskMemo->setReadOnly(true);
    MaskMemo->setFixedHeight(MaskMemo->fontMetrics().lineSpacing() * 3 + 12);
    QPalette Palette = MaskMemo->palette();
    Palette.setColor(QPalette::Base, palette().color(QPalette::Window));
    MaskMemo->setPalette(Palette);
    MaskLayout->addWidget(MaskMemo);
    Layout->addWidget(MaskGroup);

    QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton * ClearButton = Buttons->addButton(QStringLiteral("&Clear"), QDialogButtonBox::ResetRole);
    HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_editmask"));
    Layout->addWidget(Buttons);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]() { if (Validate()) accept(); });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ClearButton, &QPushButton::clicked, this, [this]()
    {
      IncludeFileMasksMemo->clear();
      ExcludeFileMasksMemo->clear();
      IncludeDirectoryMasksMemo->clear();
      ExcludeDirectoryMasksMemo->clear();
      ExcludeDirectoryAllCheck->setChecked(false);
      UpdateControls();
    });
    for (QPlainTextEdit * Memo : { IncludeFileMasksMemo, ExcludeFileMasksMemo, IncludeDirectoryMasksMemo })
    {
      connect(Memo, &QPlainTextEdit::textChanged, this, [this]() { UpdateControls(); });
    }
    connect(ExcludeDirectoryMasksMemo, &QPlainTextEdit::textChanged, this, [this]()
    {
      // ExcludeDirectoryMasksMemoChange
      if (!Changing)
      {
        Changing = true;
        QString Mask = ExcludeDirectoryMasksMemo->toPlainText().trimmed();
        ExcludeDirectoryAllCheck->setChecked((Mask == QStringLiteral("*")) || (Mask == QStringLiteral("*/")));
        Changing = false;
      }
      UpdateControls();
    });
    connect(ExcludeDirectoryAllCheck, &QCheckBox::toggled, this, [this](bool Checked)
    {
      // ExcludeDirectoryAllCheckClick
      if (!Changing)
      {
        Changing = true;
        if (Checked)
        {
          SavedExcludeDirectoryMasks = ExcludeDirectoryMasksMemo->toPlainText();
          ExcludeDirectoryMasksMemo->setPlainText(QStringLiteral("*"));
        }
        else
        {
          ExcludeDirectoryMasksMemo->setPlainText(SavedExcludeDirectoryMasks);
        }
        Changing = false;
      }
      UpdateControls();
    });
    resize(520, 580);
  }

  void LoadFileMasks(const QString & Mask)
  {
    FileMasks Masks;
    try
    {
      Masks = FileMasks(Mask);
    }
    catch (const FileMasksException &)
    {
    }
    Changing = true;
    IncludeFileMasksMemo->setPlainText(Masks.IncludeFileMasksStr().join(QLatin1Char('\n')));
    ExcludeFileMasksMemo->setPlainText(Masks.ExcludeFileMasksStr().join(QLatin1Char('\n')));
    IncludeDirectoryMasksMemo->setPlainText(Masks.IncludeDirectoryMasksStr().join(QLatin1Char('\n')));
    ExcludeDirectoryMasksMemo->setPlainText(Masks.ExcludeDirectoryMasksStr().join(QLatin1Char('\n')));
    QString Exclude = ExcludeDirectoryMasksMemo->toPlainText().trimmed();
    ExcludeDirectoryAllCheck->setChecked((Exclude == QStringLiteral("*")) || (Exclude == QStringLiteral("*/")));
    Changing = false;
    UpdateControls();
  }

  QString SaveFileMasks() const
  {
    return EditMaskDialog::ComposeMaskStr(Lines(IncludeFileMasksMemo), Lines(ExcludeFileMasksMemo),
      Lines(IncludeDirectoryMasksMemo), Lines(ExcludeDirectoryMasksMemo));
  }

private:
  QPlainTextEdit * IncludeFileMasksMemo;
  QPlainTextEdit * ExcludeFileMasksMemo;
  QPlainTextEdit * IncludeDirectoryMasksMemo;
  QPlainTextEdit * ExcludeDirectoryMasksMemo;
  QCheckBox * ExcludeDirectoryAllCheck;
  QPlainTextEdit * MaskMemo;
  bool Changing = false;
  QString SavedExcludeDirectoryMasks;

  static QStringList Lines(const QPlainTextEdit * Memo)
  {
    return Memo->toPlainText().split(QLatin1Char('\n'));
  }

  QPlainTextEdit * CreateMemo(QWidget * Parent)
  {
    QPlainTextEdit * Memo = new QPlainTextEdit(Parent);
    Memo->setLineWrapMode(QPlainTextEdit::NoWrap);
    Memo->setTabChangesFocus(true);
    return Memo;
  }

  static QLabel * CreateLabel(const QString & Caption, QWidget * Buddy, QWidget * Parent)
  {
    QLabel * Label = new QLabel(Caption, Parent);
    Label->setBuddy(Buddy);
    return Label;
  }

  void UpdateControls()
  {
    try
    {
      MaskMemo->setPlainText(SaveFileMasks());
    }
    catch (const FileMasksException & E)
    {
      MaskMemo->setPlainText(E.Message());
    }
  }

  bool Validate()
  {
    try
    {
      SaveFileMasks();
      return true;
    }
    catch (const FileMasksException & E)
    {
      QMessageBox::critical(this, QStringLiteral("Error"), E.Message());
      return false;
    }
  }
};

} // namespace

bool EditMaskDialog::EditMask(QWidget * Parent, QString & Mask)
{
  MaskEditorDialog Dialog(Parent);
  Dialog.LoadFileMasks(Mask);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  Mask = Dialog.SaveFileMasks();
  return true;
}

//---------------------------------------------------------------------------
// TSelectMaskDialog
//---------------------------------------------------------------------------

struct EditMaskDialog::Private
{
  Mode DialogMode = Select;
  QComboBox * MaskEdit = nullptr;
  QCheckBox * ApplyToDirectoriesCheck = nullptr;
  QPushButton * OkButton = nullptr;
  // smFileColor
  QPushButton * ColorButton = nullptr;
  QLabel * ColorFileNamesLabel = nullptr;
  int Color = 0;
  void UpdateFileColor();
};

void EditMaskDialog::Private::UpdateFileColor()
{
  if (ColorFileNamesLabel == nullptr)
  {
    return;
  }
  QPalette Palette = ColorFileNamesLabel->palette();
  QColor Text = (Color > 0) ? QColor(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF) :
    ColorFileNamesLabel->parentWidget()->palette().color(QPalette::Text);
  Palette.setColor(QPalette::WindowText, Text);
  Palette.setColor(QPalette::Window, ColorFileNamesLabel->parentWidget()->palette().color(QPalette::Base));
  ColorFileNamesLabel->setPalette(Palette);
  // TSelectMaskDialog::UpdateControls: OK only with a mask
  OkButton->setEnabled(!MaskEdit->currentText().trimmed().isEmpty());
}

EditMaskDialog::EditMaskDialog(Mode AMode, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->DialogMode = AMode;
  switch (AMode)
  {
    case Select: setWindowTitle(QStringLiteral("Select")); break;          // SELECT_MASK_SELECT_CAPTION
    case Unselect: setWindowTitle(QStringLiteral("Deselect")); break;      // SELECT_MASK_DESELECT_CAPTION
    case Filter: setWindowTitle(QStringLiteral("Filter")); break;          // FILTER_MASK_CAPTION
    case FileColor: setWindowTitle(QStringLiteral("File color")); break;   // FILE_COLOR_CAPTION
  }

  QVBoxLayout * Layout = new QVBoxLayout(this);
  QGroupBox * MaskGroup = new QGroupBox(this);
  QGridLayout * GroupLayout = new QGridLayout(MaskGroup);
  d->MaskEdit = new QComboBox(MaskGroup);
  d->MaskEdit->setEditable(true);
  d->MaskEdit->setInsertPolicy(QComboBox::NoInsert);
  d->MaskEdit->setMinimumContentsLength(40);
  d->MaskEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  d->MaskEdit->addItems(History(QStringLiteral("Mask")));
  QLabel * MaskLabel = new QLabel(QStringLiteral("File &mask:"), MaskGroup);
  MaskLabel->setBuddy(d->MaskEdit);
  GroupLayout->addWidget(MaskLabel, 0, 0);
  GroupLayout->addWidget(new HintLabel(QStringLiteral("mask hints"), MaskHints(false, false), MaskGroup), 0, 1,
    Qt::AlignRight);
  QPushButton * MaskButton = new QPushButton(QStringLiteral("&Edit..."), MaskGroup);
  GroupLayout->addWidget(d->MaskEdit, 1, 0, 1, 2);
  GroupLayout->addWidget(MaskButton, 1, 2);
  d->ApplyToDirectoriesCheck = new QCheckBox(QStringLiteral("Apply to &directories"), MaskGroup);
  GroupLayout->addWidget(d->ApplyToDirectoriesCheck, 2, 0, 1, 3);
  GroupLayout->setColumnStretch(0, 1);
  Layout->addWidget(MaskGroup);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  d->OkButton = Buttons->button(QDialogButtonBox::Ok);
  if (AMode == Filter)
  {
    QPushButton * ClearButton = Buttons->addButton(QStringLiteral("&Clear"), QDialogButtonBox::ResetRole);
    connect(ClearButton, &QPushButton::clicked, this, [this]() { d->MaskEdit->setEditText(QString()); });
    d->ApplyToDirectoriesCheck->hide();
  }
  if (AMode == FileColor)
  {
    d->ApplyToDirectoriesCheck->hide();
    d->ColorButton = new QPushButton(QStringLiteral("&Color"), MaskGroup);
    GroupLayout->addWidget(d->ColorButton, 2, 2);
    d->ColorFileNamesLabel = new QLabel(QStringLiteral("about.html\nindex.html\nphoto.jpg"), MaskGroup);
    d->ColorFileNamesLabel->setAutoFillBackground(true);
    d->ColorFileNamesLabel->setFrameShape(QFrame::StyledPanel);
    d->ColorFileNamesLabel->setMargin(4);
    GroupLayout->addWidget(d->ColorFileNamesLabel, 2, 0, 1, 2);
    QMenu * ColorMenu = new QMenu(d->ColorButton);
    QAction * DefaultAction = ColorMenu->addAction(QStringLiteral("&Default"));
    DefaultAction->setCheckable(true);
    QAction * MoreAction = ColorMenu->addAction(QStringLiteral("&More Colors..."));
    connect(ColorMenu, &QMenu::aboutToShow, this, [this, DefaultAction]() { DefaultAction->setChecked(d->Color == 0); });
    connect(DefaultAction, &QAction::triggered, this, [this]()
    {
      d->Color = 0;
      d->UpdateFileColor();
    });
    connect(MoreAction, &QAction::triggered, this, [this]()
    {
      QColor Initial = (d->Color > 0) ? QColor(d->Color & 0xFF, (d->Color >> 8) & 0xFF, (d->Color >> 16) & 0xFF) : QColor(Qt::red);
      QColor Selected = QColorDialog::getColor(Initial, this, windowTitle());
      if (Selected.isValid())
      {
        d->Color = std::max(1, Selected.red() | (Selected.green() << 8) | (Selected.blue() << 16));
        d->UpdateFileColor();
      }
    });
    d->ColorButton->setMenu(ColorMenu);
    connect(d->MaskEdit, &QComboBox::editTextChanged, this, [this]() { d->UpdateFileColor(); });
  }
  // HELP_FILTER / ui_select
  // HELP_FILTER / HELP_FILE_COLORS / form's ui_select
  HelpSystem::AddHelpButton(Buttons, this, (AMode == Filter) ? QStringLiteral("ui_filter") :
    ((AMode == FileColor) ? QStringLiteral("ui_file_color") : QStringLiteral("ui_select")));
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::accepted, this, [this]()
  {
    // FormCloseQuery: MaskEditExit
    if (ValidateMask(this, d->MaskEdit->currentText(), d->MaskEdit))
    {
      accept();
    }
  });
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(MaskButton, &QPushButton::clicked, this, [this]()
  {
    QString Mask = d->MaskEdit->currentText();
    if (EditMask(this, Mask))
    {
      d->MaskEdit->setEditText(Mask);
    }
  });
  setSizeGripEnabled(false);
  layout()->setSizeConstraint(QLayout::SetFixedSize);
}

EditMaskDialog::~EditMaskDialog()
{
  delete d;
}

bool EditMaskDialog::ExecuteFileColor(QWidget * Parent, QString & Mask, int & Color)
{
  EditMaskDialog Dialog(FileColor, Parent);
  Dialog.d->Color = Color;
  Dialog.d->MaskEdit->setEditText(Mask);
  Dialog.d->MaskEdit->lineEdit()->selectAll();
  Dialog.d->MaskEdit->setFocus();
  Dialog.d->UpdateFileColor();
  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    Mask = Dialog.d->MaskEdit->currentText();
    Color = Dialog.d->Color;
    SaveToHistory(QStringLiteral("Mask"), Mask);
  }
  return Result;
}

bool EditMaskDialog::Execute(QWidget * Parent, Mode AMode, QString & Mask, bool & ApplyToDirectories)
{
  EditMaskDialog Dialog(AMode, Parent);
  bool UseRemembered = (AMode != Filter) && Mask.isEmpty();
  if (UseRemembered)
  {
    // DoSelectMaskDialog
    Mask = SelectMask();
    ApplyToDirectories = SelectDirectories();
  }
  Dialog.d->ApplyToDirectoriesCheck->setChecked(ApplyToDirectories);
  Dialog.d->MaskEdit->setEditText(Mask);
  Dialog.d->MaskEdit->lineEdit()->selectAll();
  Dialog.d->MaskEdit->setFocus();
  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    Mask = Dialog.d->MaskEdit->currentText();
    ApplyToDirectories = Dialog.d->ApplyToDirectoriesCheck->isChecked();
    SaveToHistory(QStringLiteral("Mask"), Mask);
    if (AMode != Filter)
    {
      SetSelectMask(Mask);
      SetSelectDirectories(ApplyToDirectories);
    }
  }
  return Result;
}
