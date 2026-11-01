// Port of WinSCP source/packages/my/HistoryComboBox.pas (THistoryComboBox:
// editable combo box remembering the last values) and its persistence in
// windows/CustomWinConfiguration.cpp (TCustomWinConfiguration::History,
// DefaultHistory, SaveData: [Configuration\History\<key>] 0=..., 1=...).
//
// The list is kept in Configuration::Instance().Ini() (saved with
// Configuration::Save), most recent first, at most MaxHistorySize items,
// exactly as WinSCP.ini stores it, so histories are shared with WinSCP.
// Keys used by WinSCP: "Mask", "LocalDirectory", "RemoteDirectory",
// "Commands", "SpeedLimit", "PuttyPath", "RemoteTarget", "LocalTarget",
// "SyncMask", "CustomCommandParam"... (the HistoryKey of each combo).
#pragma once

#include <QComboBox>
#include <QString>
#include <QStringList>

class HistoryComboBox : public QComboBox
{
  Q_OBJECT

public:
  static constexpr int MaxHistorySize = 30;   // DefaultMaxHistorySize

  explicit HistoryComboBox(QWidget * Parent = nullptr);
  HistoryComboBox(const QString & HistoryKey, QWidget * Parent);

  QString HistoryKey() const { return FHistoryKey; }
  // Sets the key and loads its history (keeps the edit text)
  void SetHistoryKey(const QString & Key);
  // Reloads the items from the configuration (keeps the edit text)
  void LoadHistory();
  // THistoryComboBox::SaveToHistory: moves the current text to the top of
  // the items and stores the list (does nothing for an empty text)
  void SaveToHistory();

  // TCustomWinConfiguration::History[Key]; Defaults (DefaultHistory) are
  // returned while the key has never been stored
  static QStringList History(const QString & Key, const QStringList & Defaults = QStringList());
  // Replaces the stored list (trimmed to MaxHistorySize, duplicates removed)
  static void SetHistory(const QString & Key, const QStringList & Items);
  // Value to the top of the stored list
  static void SaveToHistory(const QString & Key, const QString & Value);
  // Fills any combo box with the history, keeping its edit text
  static void LoadHistoryCombo(QComboBox * Combo, const QString & Key);
  // Prepends Value to Items (removing an equal entry) and trims to MaxHistorySize
  static QStringList AddToHistoryList(const QStringList & Items, const QString & Value);

private:
  QString FHistoryKey;
};
