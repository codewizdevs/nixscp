// Pro > Teams...: NixSCP Teams (core/TeamSync): create and join teams, share
// site folders, invite with one-time codes, manage members and roles.
#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

class TeamsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit TeamsDialog(QWidget * Parent = nullptr);
  static void Execute(QWidget * Parent);

private:
  QTableWidget * FTable;
  QLabel * FStatus;
  QPushButton * FInvite;
  QPushButton * FMembers;
  QPushButton * FShare;
  QPushButton * FRename;
  QPushButton * FLeave;
  QPushButton * FDelete;

  void Reload();
  void UpdateButtons();
  QString SelectedUuid() const;
  // runs an operation with a busy cursor and reports its error
  void Run(const std::function<void(const std::function<void(bool, const QString &)> &)> & Operation);

  void NewTeam();
  void Join();
  void Invite();
  void ShowMembers();
  void ShareFolder();
  void Rename();
  void Leave();
  void Delete();
};
