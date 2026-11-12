// The saved sites (Configuration::StoredSessions) for the Sites screen: a
// flat list of section headers and sites. Without a search: Favourites,
// Recent, then each folder (collapsible); with a search: the matching sites
// of every folder. Favourites, recent sites and collapsed folders are this
// phone's own (PrivateState, read after the unlock). Note items
// (SessionData::NoteOnly) are listed with the sites but have no server: they
// open as a note and never connect.
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
  // only what can connect: note items are left out (picking a site for a new
  // tab or for shared files)
  Q_PROPERTY(bool connectOnly READ connectOnly WRITE setConnectOnly NOTIFY connectOnlyChanged)
  // a site's note: at most this many bytes of UTF-8 (AgentAccess::MaxNoteBytes)
  Q_PROPERTY(int maxNoteBytes READ maxNoteBytes CONSTANT)

public:
  enum Roles { NameRole = Qt::UserRole + 1, TitleRole, FolderRole, HostRole, UserRole, ProtocolRole,
    IsHeaderRole, SectionRole, CollapsedRole, SiteCountRole, FavouriteRole,
    // a note item; the site has a note; the note's first line
    NoteItemRole, HasNoteRole, NoteLineRole };

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
  bool connectOnly() const { return FConnectOnly; }
  void setConnectOnly(bool Value);
  int maxNoteBytes() const;

  // Re-reads the configuration (after sync or an import)
  Q_INVOKABLE void reload();
  // Imports sites from a WinSCP.ini file; the number imported, -1 on error
  Q_INVOKABLE int importWinScpIni(const QString & FileName);
  // name, protocol, host, port (0 = default), user, hasPassword, directory,
  // keyFile, note, noteItem, canEditNote
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
  // Gives a site or note item another name ("folder/name", the same cleaning
  // as saveSite); everything else stays. An empty string on success,
  // otherwise the reason.
  Q_INVOKABLE QString renameSite(const QString & Original, const QString & Name);
  // A name as it is stored: no empty folders, no "/" at the ends
  Q_INVOKABLE QString cleanName(const QString & Name) const;

  // ---- notes (SessionData::Note) ----
  // a note item: no server, it opens as a note
  Q_INVOKABLE bool isNote(const QString & Name) const;
  // Whether this phone's user may change the site's note. The only place
  // with the rule: a team's site has its note edited by the team's owners
  // and admins only.
  Q_INVOKABLE bool canEditNote(const QString & Name) const;
  // Replaces the site's note, and nothing else of it. The text is refused
  // when AgentAccess::NoteAcceptable refuses it. An empty string on success,
  // otherwise the reason.
  Q_INVOKABLE QString saveNote(const QString & Name, const QString & Text);
  // A new note item (a "/" makes folders); a taken name gets " (1)". The
  // name it got, empty when it could not be saved (failed tells why).
  Q_INVOKABLE QString newNote(const QString & Name);
  // "2.1 KB of 64 KB"
  Q_INVOKABLE QString noteSize(const QString & Text) const;
  Q_INVOKABLE int noteBytes(const QString & Text) const;
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
  void connectOnlyChanged();
  // something could not be saved
  void failed(const QString & message);

private:
  struct Site
  {
    QString Name; QString Title; QString Folder; QString Host; QString User; QString Protocol;
    bool NoteOnly = false;
    QString Note;
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
  bool FConnectOnly = false;

  void Rebuild();
  void SaveState();
  void RenameState(const QString & From, const QString & To);
};
