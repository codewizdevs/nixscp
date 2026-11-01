// Port of WinSCP source/forms/Properties.{cpp,dfm}, forms/Rights.{cpp,dfm}
// and forms/CreateDirectory.{cpp,dfm}
#pragma once

#include "core/FileSystem.h"
#include "core/RemoteFile.h"

#include <QDialog>
#include <QWidget>

#include <functional>

// The "Permissions" frame (TRightsFrame): 3x3 R/W/X check boxes + Set UID /
// Set GID / Sticky bit + Octal edit + optional "Add X to directories".
// With AllowUndef the check boxes are three-state (grayed = the bit is left
// as it is on each file); the octal edit is empty then. The row buttons
// (Owner/Group/Others) cycle Yes / No / Undefined.
class RightsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit RightsWidget(QWidget * Parent = nullptr);
  // All bits defined; does not emit Changed()
  void SetRights(unsigned int Mode);
  // Bits set (checked) of the 12 rights bits
  unsigned int Rights() const;
  // TRights NumberSet / NumberUnset: the bits that are neither set nor
  // unset are undefined (grayed)
  void SetStates(unsigned int Set, unsigned int Unset);
  unsigned int RightsSet() const { return Rights(); }
  unsigned int RightsUnset() const;
  bool IsUndef() const;
  void SetAllowUndef(bool Value);
  bool AllowUndef() const;
  void SetAddXToDirectoriesVisible(bool Visible);
  bool AddXToDirectories() const;
  void SetAddXToDirectories(bool Value);
  bool IsValid() const;   // octal edit contains valid value (or is empty with undefined bits)

signals:
  void Changed();

private:
  struct Private;
  Private * d;
};

class PropertiesDialog : public QDialog
{
  Q_OBJECT

public:
  // TPropertiesDialog AllowedChanges (cpMode, cpOwner, cpGroup)
  enum { cpMode = 0x01, cpOwner = 0x02, cpGroup = 0x04 };

  // TCalculateSizeStats
  struct SizeStats
  {
    int Files = 0;
    int Directories = 0;
    int SymLinks = 0;
  };
  // TCalculateSizeEvent: calculates the size of the selection (shows its own
  // progress, blocks until done); false when failed or cancelled
  using CalculateSizeFunc = std::function<bool(qint64 & Size, SizeStats & Stats)>;
  // TCalculatedChecksumEvent
  using ChecksumResultFunc = std::function<void(const QString & FileName, const QString & Alg, const QString & Checksum)>;
  // TCalculateChecksumEvent: calculates the checksums of the selection with
  // Alg, reporting each through OnChecksum; blocks until done
  using CalculateChecksumFunc = std::function<void(const QString & Alg, const ChecksumResultFunc & OnChecksum)>;

  // Files: selection (at least 1). Location: directory containing them.
  // The permissions can be changed (cpMode); call SetAllowedChanges for owner/group.
  PropertiesDialog(const QList<RemoteFile> & Files, const QString & Location, bool Remote, QWidget * Parent = nullptr);
  ~PropertiesDialog() override;

  void SetAllowedChanges(int AllowedChanges);
  // Candidate lists of the editable Owner/Group combos (TRemoteTokenList)
  void SetUsersGroups(const QStringList & Users, const QStringList & Groups);
  // "C&alculate" button (shown when a directory is selected)
  void SetOnCalculateSize(CalculateSizeFunc OnCalculateSize);
  // Checksum tab (disabled without a handler); Algs fill the combo, the
  // remembered ChecksumAlg (Configuration\Interface) is selected
  void SetChecksum(const QStringList & Algs, CalculateChecksumFunc OnCalculateChecksum);

  // The changed properties (TRemoteProperties::ChangedProperties): with
  // Recursive everything defined is applied again to the contents
  RemoteProperties Properties() const;

  unsigned int Rights() const;
  bool RightsChanged() const;   // user changed permissions
  bool Recursive() const;       // "Set owner, group and permissions recursively" (only shown when a directory is selected)
  bool AddXToDirectories() const;

  // ChecksumAlg of Configuration\Interface (GUIConfiguration, default "sha1")
  static QString ConfiguredChecksumAlg();
  static void SetConfiguredChecksumAlg(const QString & Alg);

  // validates the owner/group (TPropertiesDialog::StoreRemoteToken) before closing
  void accept() override;

protected:
  void showEvent(QShowEvent * Event) override;

private:
  struct Private;
  Private * d;
};

class CreateDirectoryDialog : public QDialog
{
  Q_OBJECT

public:
  explicit CreateDirectoryDialog(bool Remote, QWidget * Parent = nullptr);

  QString DirectoryName() const;
  void SetDirectoryName(const QString & Name);
  bool SetRights() const;      // "Set permissions" checked (remote only)
  unsigned int Rights() const; // default 0755

  // TCreateDirectoryDialog::Execute with Properties and SaveSettings:
  // "Set pe&rmissions" is disabled without cpMode in AllowedChanges
  void SetAllowedChanges(int AllowedChanges);
  void SetProperties(const RemoteProperties & Properties);
  RemoteProperties Properties() const;
  // "Use &same settings next time"
  void SetSaveSettings(bool Value);
  bool SaveSettings() const;

  // GUIConfiguration NewDirectoryProperties (Configuration\Interface\NewDirectory2:
  // Valid (binary set, vpRights = 0x01), Rights ("rwxr-xr-x")); default 0755, not set
  static RemoteProperties LoadNewDirectoryProperties();
  static void SaveNewDirectoryProperties(const RemoteProperties & Properties);

private:
  struct Private;
  Private * d;
};
