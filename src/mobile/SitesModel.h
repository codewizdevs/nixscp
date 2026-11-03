// The saved sites (Configuration::StoredSessions) for the Sites screen:
// one row per site, with its folder ("Team: Acme/web" etc.) as a section.
#pragma once

#include <QAbstractListModel>

class SitesModel : public QAbstractListModel
{
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
  enum Roles { NameRole = Qt::UserRole + 1, TitleRole, FolderRole, HostRole, UserRole, ProtocolRole };

  explicit SitesModel(QObject * Parent = nullptr);
  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role) const override;
  QHash<int, QByteArray> roleNames() const override;

  // Re-reads the configuration (after sync or an import)
  Q_INVOKABLE void reload();
  // Imports sites from a WinSCP.ini file; the number imported, -1 on error
  Q_INVOKABLE int importWinScpIni(const QString & FileName);
  // Saves a new site (or replaces the one of the same name); Protocol:
  // "SFTP", "SCP", "FTP", "FTPS", "WebDAV", "WebDAVS"; Port 0 = default.
  // An empty password is asked for when connecting.
  Q_INVOKABLE bool addSite(const QString & Name, const QString & Protocol, const QString & Host, int Port,
    const QString & User, const QString & Password);
  Q_INVOKABLE void removeSite(const QString & Name);

signals:
  void countChanged();

private:
  struct Row { QString Name; QString Title; QString Folder; QString Host; QString User; QString Protocol; };
  QList<Row> FRows;
};
