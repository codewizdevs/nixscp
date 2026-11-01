// Port of WinSCP source/packages/my/HistoryComboBox.cpp and the History part
// of windows/CustomWinConfiguration.cpp
#include "gui/HistoryComboBox.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"

#include <QLineEdit>
#include <QSignalBlocker>

static bool OpenHistoryKey(IniStorage & Storage, const QString & Key, bool CanCreate)
{
  return
    Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
    Storage.OpenSubKey(QStringLiteral("History"), CanCreate) &&
    Storage.OpenSubKey(Key, CanCreate);
}

HistoryComboBox::HistoryComboBox(QWidget * Parent) :
  QComboBox(Parent)
{
  setEditable(true);
  setInsertPolicy(QComboBox::NoInsert);
}

HistoryComboBox::HistoryComboBox(const QString & HistoryKey, QWidget * Parent) :
  HistoryComboBox(Parent)
{
  SetHistoryKey(HistoryKey);
}

void HistoryComboBox::SetHistoryKey(const QString & Key)
{
  FHistoryKey = Key;
  LoadHistory();
}

void HistoryComboBox::LoadHistory()
{
  if (!FHistoryKey.isEmpty())
  {
    LoadHistoryCombo(this, FHistoryKey);
  }
}

void HistoryComboBox::SaveToHistory()
{
  QString Text = currentText();
  if (Text.isEmpty())
  {
    return;
  }
  if (!FHistoryKey.isEmpty())
  {
    SaveToHistory(FHistoryKey, Text);
    LoadHistory();
  }
  else
  {
    // in-memory only (no key): THistoryComboBox::AddToHistory
    QStringList Items;
    for (int Index = 0; Index < count(); Index++)
    {
      Items.append(itemText(Index));
    }
    Items = AddToHistoryList(Items, Text);
    QSignalBlocker Blocker(this);
    clear();
    addItems(Items);
    setEditText(Text);
  }
}

QStringList HistoryComboBox::AddToHistoryList(const QStringList & Items, const QString & Value)
{
  // ::SaveToHistory (TComboBox Items->IndexOf is case insensitive)
  QStringList Result = Items;
  if (!Value.isEmpty())
  {
    for (int Index = Result.size() - 1; Index >= 0; Index--)
    {
      if (Result.at(Index).compare(Value, Qt::CaseInsensitive) == 0)
      {
        Result.removeAt(Index);
      }
    }
    Result.prepend(Value);
  }
  while (Result.size() > MaxHistorySize)
  {
    Result.removeLast();
  }
  return Result;
}

QStringList HistoryComboBox::History(const QString & Key, const QStringList & Defaults)
{
  QStringList Result;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (OpenHistoryKey(Storage, Key, false))
  {
    // THierarchicalStorage::ReadValues: "0", "1", ...
    for (int Index = 0; ; Index++)
    {
      QString Name = QString::number(Index);
      if (!Storage.ValueExists(Name))
      {
        break;
      }
      QString Value = Storage.ReadString(Name, QString());
      if (!Value.isEmpty() && !Result.contains(Value))
      {
        Result.append(Value);
      }
    }
    return Result;
  }
  return Defaults;
}

void HistoryComboBox::SetHistory(const QString & Key, const QStringList & Items)
{
  QStringList Values;
  for (const QString & Item : Items)
  {
    if (!Item.isEmpty() && !Values.contains(Item) && (Values.size() < MaxHistorySize))
    {
      Values.append(Item);
    }
  }
  IniStorage Storage(&Configuration::Instance().Ini());
  if (OpenHistoryKey(Storage, Key, true))
  {
    // THierarchicalStorage::WriteValues: ClearValues + "0", "1", ...
    for (const QString & Name : Storage.GetValueNames())
    {
      Storage.DeleteValue(Name);
    }
    for (int Index = 0; Index < Values.size(); Index++)
    {
      Storage.WriteString(QString::number(Index), Values.at(Index));
    }
  }
}

void HistoryComboBox::SaveToHistory(const QString & Key, const QString & Value)
{
  SetHistory(Key, AddToHistoryList(History(Key), Value));
}

void HistoryComboBox::LoadHistoryCombo(QComboBox * Combo, const QString & Key)
{
  QString Text = Combo->currentText();
  QSignalBlocker Blocker(Combo);
  Combo->clear();
  Combo->addItems(History(Key));
  if (Combo->isEditable())
  {
    Combo->setEditText(Text);
  }
}
