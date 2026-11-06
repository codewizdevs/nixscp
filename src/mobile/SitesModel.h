// The saved sites (Configuration::StoredSessions) for the Sites screen: a
// flat list of section headers and sites. Without a search: Favourites,
// Recent, then each folder (collapsible); with a search: the matching sites
// of every folder. Favourites, recent sites and collapsed folders are this
// phone's own (PrivateState, read after the unlock).
#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QVariantMap>

class MobileSettings;

class SitesModel : public QAbstractListModel
{
  Q_OBJECT
  // sites in the configuration (not rows)
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  // sites shown for the search
  Q_PROPERTY(int matches READ matches NOTIFY rowsChanged)
  Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
  enum Roles { NameRole = Qt::UserRole + 1, TitleRole, FolderRole, HostRole, UserRole, ProtocolRole,
    IsHeaderRole, SectionRole, CollapsedRole, SiteCountRole, FavouriteRole };

  explicit SitesModel(MobileSettings * Settings, QObject * Parent = nullptr);
  // the phone's own lists, once they can be read
  void LoadState();
  // Key files of this phone kept in sites by earlier versions become the
  // phone's own choice (the site's key file is synced to other devices)
  void MigrateKeys();
  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role) const override;
  QHash<int, QByteArray> roleNames() const override;

  int count() const { return static_cast<int>(FSites.size()); }
  int matches() const { return FMatches; }
  QString filter() const { return FFilter; }
  void setFilter(const QString & Value);

  // Re-reads the configuration (after sync or an import)
  Q_INVOKABLE void reload();
  // Imports sites from a WinSCP.ini file; the number imported, -1 on error
  Q_INVOKABLE int importWinScpIni(const QString & FileName);
  // name, protocol, host, port (0 = default), user, hasPassword, directory, keyFile
  Q_INVOKABLE QVariantMap siteData(const QString & Name) const;
  // Saves a site: a new one (Original empty) or changes Original (renamed
  // when Name differs; its other settings, e.g. the key file, are kept).
  // Protocol: "SFTP", "SCP", "FTP", "FTPS", "WebDAV", "WebDAVS", "S3";
  // Port 0 = default. An empty password keeps the saved one with
  // KeepPassword, otherwise it is asked for when connecting. KeyFile: the
  // private key's path ("" = none).
  // An empty string on success, otherwise the reason.
  Q_INVOKABLE QString saveSite(const QString & Original, const QString & Name, const QString & Protocol,
    const QString & Host, int Port, const QString & User, const QString & Password, bool KeepPassword,
    const QString & Directory, const QString & KeyFile);
  // A copy as "Name (2)"; the new name
  Q_INVOKABLE QString duplicateSite(const QString & Name);
  Q_INVOKABLE void removeSite(const QString & Name);
  Q_INVOKABLE void toggleFavourite(const QString & Name);
  Q_INVOKABLE void toggleSection(const QString & Section);
  // a site was connected: first of the recent ones
  Q_INVOKABLE void addRecent(const QString & Name);
  // a site got another name outside this list (core/TeamSync): its favourite,
  // recent and key entries follow
  void SiteMoved(const QString & From, const QString & To);

signals:
  void countChanged();
  void rowsChanged();
  void filterChanged();
  // something could not be saved
  void failed(const QString & message);

private:
  struct Site
  {
    QString Name; QString Title; QString Folder; QString Host; QString User; QString Protocol;
    bool operator==(const Site &) const = default;
  };
  struct Row
  {
    bool Header = false;
    QString Section;   // key of the section (header and its sites)
    QString Title;     // header: the section's title
    int Count = 0;     // header: sites in the section
    const Site * S = nullptr;
  };
  QList<Site> FSites;
  QList<Row> FRows;
  QString FFilter;
  int FMatches = 0;
  QStringList FFavourites;
  QStringList FRecent;
  QSet<QString> FCollapsed;
  MobileSettings * FMobile;
  bool FReloaded = false;

  void Rebuild();
  void SaveState();
  void RenameState(const QString & From, const QString & To);
};
