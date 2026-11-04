// NixSCP Teams on the phone (core/TeamSync): the teams and their members
// and invites; create, rename, delete, leave, join with an invite code,
// share a site folder. Each operation answers with finished(op, ok, error,
// data). A team's sites appear in the site list as "Team: <name>/...".
#pragma once

#include <QObject>
#include <QVariantList>

class TeamsModel : public QObject
{
  Q_OBJECT
  // [{uuid, name, role, canEdit, members, needsRotation, keyReady}]
  Q_PROPERTY(QVariantList teams READ teams NOTIFY changed)
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
  explicit TeamsModel(QObject * Parent = nullptr);
  void Start();
  QVariantList teams() const;
  bool available() const;
  bool busy() const { return FBusy > 0; }

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void create(const QString & Name);
  Q_INVOKABLE void rename(const QString & Uuid, const QString & Name);
  Q_INVOKABLE void remove(const QString & Uuid);
  Q_INVOKABLE void leave(const QString & Uuid);
  Q_INVOKABLE void join(const QString & Code);
  // the sites of a folder of the site list move into the team
  Q_INVOKABLE void shareFolder(const QString & Uuid, const QString & Folder);
  // Role: admin or member; data: the code to hand over
  Q_INVOKABLE void invite(const QString & Uuid, const QString & Role, int Days);
  // data: [{id, role, expires_at...}]
  Q_INVOKABLE void invites(const QString & Uuid);
  Q_INVOKABLE void revokeInvite(const QString & Uuid, const QString & Id);
  // data: [{user, email, role...}]
  Q_INVOKABLE void members(const QString & Uuid);
  Q_INVOKABLE void setRole(const QString & Uuid, const QString & User, const QString & Role);
  Q_INVOKABLE void removeMember(const QString & Uuid, const QString & User);
  // the folders of the site list that are not team folders
  Q_INVOKABLE QStringList localFolders() const;

signals:
  void changed();
  void busyChanged();
  void finished(const QString & op, bool ok, const QString & error, const QVariant & data);

private:
  int FBusy = 0;
  std::function<void(bool, const QString &)> Done(const QString & Op);
};
