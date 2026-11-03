// NixSCP Pro encryption of the local configuration (core/ConfigVault):
// encrypted on disk, locked files never overwritten, unlocking with the
// password or the recovery key, plain and encrypted exports, imports of
// backups made with another password, newer wraps from another instance, and
// the protected companion files.
#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static QByteArray ReadRaw(const QString & FileName)
{
  QFile File(FileName);
  return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

static void WriteRaw(const QString & FileName, const QByteArray & Data)
{
  QFile File(FileName);
  CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
  File.write(Data);
}

static CloudCrypto::SnapshotHeader NewWrap(const QByteArray & Dek, const QString & Password, const QByteArray & Recovery)
{
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("user-1");
  // fast parameters for the test
  Wrap.Kdf.MemoryKib = 8 * 1024;
  Wrap.Kdf.Iterations = 1;
  CHECK(CloudCrypto::WrapDek(Wrap, Dek, Password, Recovery));
  return Wrap;
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Dir;
  QString Ini = Dir.path() + QStringLiteral("/nixscp.ini");
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Ini);
  Config.Load();
  SessionData Site;
  Site.Name = QStringLiteral("Secret Project/web1");
  Site.HostName = QStringLiteral("web1.internal.example");
  Site.UserName = QStringLiteral("deploy");
  Site.Password = QStringLiteral("hunter2-site-password");
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(Config.Save());
  CHECK(ReadRaw(Ini).contains("web1.internal.example"));

  // ---- encryption on: the whole file is ciphertext
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Recovery = CloudCrypto::NewRecoveryKey();
  QString RecoveryText = CloudCrypto::FormatRecoveryKey(Recovery);
  CloudCrypto::SnapshotHeader Wrap = NewWrap(Dek, QStringLiteral("correct horse"), Recovery);
  ConfigVault::SetKey(Dek, Wrap);
  CHECK(ConfigVault::Enabled());
  CHECK(Config.Rewrite());
  QByteArray Raw = ReadRaw(Ini);
  CHECK(ConfigVault::IsSealedFile(Ini));
  CHECK(!Raw.contains("web1") && !Raw.contains("Secret") && !Raw.contains("deploy"));
#ifndef _WIN32
  CHECK((QFile::permissions(Ini) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
#endif
  // a save of a change stays encrypted
  Config.StoredSessions().FindByName(Site.Name)->Note = QStringLiteral("changed");
  CHECK(Config.Save());
  CHECK(ConfigVault::IsSealedFile(Ini));
  // reloading with the key in memory works
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) != nullptr);

  // ---- companion files are protected; exports and other files are not
  CHECK(ConfigVault::IsProtectedFile(Ini));
  CHECK(ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/nixscp-cloud-session.ini")));
  CHECK(ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/backups/nixscp-20260101-000000.ini")));
  CHECK(!ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/nixscp-cloud.ini")));
  CHECK(!ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/export.ini")));
  {
    IniFile Backup;
    Backup.WriteRaw(QStringLiteral("Test"), QStringLiteral("Key"), QStringLiteral("backup value"));
    QDir().mkpath(Dir.path() + QStringLiteral("/backups"));
    QString Name = Dir.path() + QStringLiteral("/backups/nixscp-20260101-000000.ini");
    CHECK(Backup.Save(Name));
    CHECK(ConfigVault::IsSealedFile(Name));
    IniFile Back;
    CHECK(Back.Load(Name) && (Back.ReadRaw(QStringLiteral("Test"), QStringLiteral("Key")) == QStringLiteral("backup value")));
  }

  // ---- plain export: a WinSCP-readable file with the site password
  QString Plain = Dir.path() + QStringLiteral("/export.ini");
  CHECK(Config.Export(Plain));
  CHECK(!ConfigVault::IsSealedFile(Plain));
  CHECK(ReadRaw(Plain).contains("web1.internal.example"));
  {
    IniFile Exported;
    CHECK(Exported.Load(Plain));
    StoredSessionList List;
    List.Load(Exported);
    auto Found = List.FindByName(Site.Name);
    CHECK((Found != nullptr) && (Found->Password == Site.Password));
  }

  // ---- encrypted export: opens with the password or the recovery key only
  QByteArray Sealed = ConfigVault::SealExport(Config.ExportData().SaveToData());
  CHECK(ConfigVault::IsSealed(Sealed));
  QByteArray Opened;
  QString Error;
  CHECK(!ConfigVault::OpenWithSecret(Sealed, QStringLiteral("wrong"), false, Opened, &Error) && Error.contains(QStringLiteral("password")));
  CHECK(ConfigVault::OpenWithSecret(Sealed, QStringLiteral("correct horse"), false, Opened) && Opened.contains("web1.internal.example"));
  CHECK(ConfigVault::OpenWithSecret(Sealed, RecoveryText.toLower(), true, Opened) && Opened.contains("web1.internal.example"));
  CHECK(!ConfigVault::OpenWithSecret(Sealed, QStringLiteral("AAAA-BBBB"), true, Opened));
  // a backup of another account / password opens with its own password and leaves the current key alone
  {
    QByteArray OtherDek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
    CloudCrypto::SnapshotHeader Other = NewWrap(OtherDek, QStringLiteral("other password"), QByteArray());
    Other.Slot = QString::fromLatin1(ConfigVault::Slot);
    QByteArray Foreign = CloudCrypto::EncodeSnapshot(Other, OtherDek, "[Configuration]\nX=1\n");
    QByteArray Plaintext;
    CHECK(!ConfigVault::Open(Foreign, Plaintext));
    CHECK(ConfigVault::OpenWithSecret(Foreign, QStringLiteral("other password"), false, Plaintext) && Plaintext.contains("X=1"));
    CHECK(ConfigVault::Key() == Dek);
  }

  // ---- another NixSCP on this computer changed the password: its newer wraps are adopted
  {
    CloudCrypto::SnapshotHeader Newer = NewWrap(Dek, QStringLiteral("new password"), Recovery);
    Newer.Slot = QString::fromLatin1(ConfigVault::Slot);
    Newer.SavedAt = QDateTime::currentDateTimeUtc().addSecs(5);
    WriteRaw(Ini, CloudCrypto::EncodeSnapshot(Newer, Dek, Config.ExportData().SaveToData()));
    Config.Load();
    CHECK(!Config.IsLocked());
    CHECK(Config.Rewrite());
    // the file now opens with the new password, not the old one
    CHECK(ConfigVault::OpenWithSecret(ReadRaw(Ini), QStringLiteral("new password"), false, Opened));
    CHECK(!ConfigVault::OpenWithSecret(ReadRaw(Ini), QStringLiteral("correct horse"), false, Opened));
  }

  // ---- a new process: locked until unlocked, and a locked file is never overwritten
  ConfigVault::Forget();
  CHECK(!ConfigVault::Enabled());
  QByteArray Before = ReadRaw(Ini);
  Config.Load();
  CHECK(Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) == nullptr);
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(!Config.Save());
  CHECK(!Config.Rewrite());
  CHECK(ReadRaw(Ini) == Before);
  CHECK(!ConfigVault::Unlock(Ini, QStringLiteral("correct horse"), false, &Error) && Error.contains(QStringLiteral("not correct")));
  CHECK(!ConfigVault::Enabled());
  CHECK(ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) != nullptr);
  CHECK(Config.StoredSessions().FindByName(Site.Name)->Password == Site.Password);
  // the recovery key unlocks too
  ConfigVault::Forget();
  CHECK(ConfigVault::Unlock(Ini, RecoveryText, true));
  CHECK(ConfigVault::Key() == Dek);

  // ---- a damaged file: not unlockable, still never overwritten
  ConfigVault::Forget();
  QByteArray Damaged = ReadRaw(Ini);
  Damaged[Damaged.size() - 3] = static_cast<char>(Damaged.at(Damaged.size() - 3) ^ 0x55);
  WriteRaw(Ini, Damaged);
  CHECK(!ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  CHECK(Config.IsLocked());
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(!Config.Save());
  CHECK(ReadRaw(Ini) == Damaged);

  // ---- nixscp-cli on an encrypted configuration: asks for the password; refused without input
  {
    QString Cli = qEnvironmentVariable("NIXSCP_CLI", QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp-cli"));
    QString Copy = Dir.path() + QStringLiteral("/cli/nixscp.ini");
    QDir().mkpath(QFileInfo(Copy).absolutePath());
    WriteRaw(Copy, Before);
    if (QFileInfo(Cli).isExecutable())
    {
      QProcess P;
      P.start(Cli, {QStringLiteral("/ini=") + Copy, QStringLiteral("/command"), QStringLiteral("exit")});
      P.closeWriteChannel();
      CHECK(P.waitForFinished(60000));
      QByteArray Out = P.readAllStandardOutput() + P.readAllStandardError();
      CHECK((P.exitCode() != 0) && Out.contains("encrypted"));
      // the password typed in (wrong first, then right)
      P.start(Cli, {QStringLiteral("/ini=") + Copy, QStringLiteral("/command"), QStringLiteral("exit")});
      P.write("wrong password\nnew password\n");
      P.closeWriteChannel();
      CHECK(P.waitForFinished(60000));
      Out = P.readAllStandardOutput() + P.readAllStandardError();
      CHECK((P.exitCode() == 0) && Out.contains("not correct"));
      // the file is still encrypted afterwards
      CHECK(ConfigVault::IsSealedFile(Copy));
    }
  }

  // ---- encryption removed: written readable again
  WriteRaw(Ini, Before);
  CHECK(ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  ConfigVault::Disable();
  CHECK(Config.Rewrite());
  CHECK(!ConfigVault::IsSealedFile(Ini));
  CHECK(ReadRaw(Ini).contains("web1.internal.example"));

  if (Failures == 0)
  {
    std::printf("All configuration encryption tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
