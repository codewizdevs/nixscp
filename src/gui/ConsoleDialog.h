// Port of WinSCP source/forms/Console.{cpp,dfm} (TConsoleDialog, DoConsoleDialog)
#pragma once

#include <QDialog>
#include <QString>

class Terminal;

class ConsoleDialog : public QDialog
{
  Q_OBJECT

public:
  // The terminal must be open and outlive the dialog (the dialog closes
  // itself when the terminal disconnects)
  explicit ConsoleDialog(Terminal * ATerminal, QWidget * Parent = nullptr);
  ~ConsoleDialog() override;

  // DoConsoleDialog: shows the dialog modally; a non-empty Command is
  // executed right away. Returns true when a command changed the current
  // directory of the terminal ("cd"), so the caller should reload the remote
  // panel at Terminal::CurrentDirectory() (WinSCP reloads it on close).
  static bool Execute(Terminal * ATerminal, QWidget * Parent, const QString & Command = QString());

  // Whether a command changed Terminal::CurrentDirectory() since the dialog opened
  bool DirectoryChanged() const;
  bool Executing() const;

signals:
  // After every executed command: commands may have changed remote files,
  // the panel may want to refresh
  void CommandExecuted();

protected:
  void closeEvent(QCloseEvent * Event) override;
  void reject() override;

private:
  struct Private;
  Private * d;
};
