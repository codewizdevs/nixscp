// The logic of the Android app (src/mobile), linked and run on the desktop:
// text files of the built-in editor, the queue of questions, the file list,
// the cache and the private state, and real sessions against the test
// servers (NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_FTP_PORT: without them those
// parts are skipped). A small proxy can cut connections, and a small libssh
// server refuses a terminal. Site notes against the fake cloud of the team
// tests (TeamsFake.h): a member's team note is read-only. The pages (QML) run
// only on a device.
#include "core/AgentAccess.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/CloudCrypto.h"
#include "core/IniStorage.h"
#include "core/PuttyKey.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "mobile/AndroidFiles.h"
#include "mobile/AppLock.h"
#include "mobile/FileListModel.h"
#include "mobile/KeyStore.h"
#include "mobile/LocalBrowser.h"
#include "mobile/MobileQueue.h"
#include "mobile/MobileSettings.h"
#include "mobile/MobileUserInterface.h"
#include "mobile/PrivateState.h"
#include "mobile/RemoteSession.h"
#include "mobile/SessionManager.h"
#include "mobile/SiteChecker.h"
#include "mobile/SitesModel.h"
#include "mobile/TerminalSession.h"
#include "mobile/TextFile.h"

#include <QBuffer>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <libssh/libssh.h>
#include <libssh/server.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool Wait(const std::function<bool()> & Done, int Ms = 15000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Done() && !Timer.hasExpired(Ms))
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(2);
  }
  return Done();
}

// the fake cloud of the team tests (TestTeamNotes)
#include "TeamsFake.h"

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QDir().mkpath(QFileInfo(Path).absolutePath());
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

// every file below the folder
static QStringList FilesBelow(const QString & Folder)
{
  QStringList Result;
  QDirIterator It(Folder, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (It.hasNext())
  {
    Result.append(It.next());
  }
  return Result;
}

// Empties a folder of the FTP server and keeps the folders in it. They must
// not be deleted: pyftpdlib's threaded server changes the working directory
// of its process for CWD, two connections at once can leave it in a subfolder,
// and once that folder is gone the server drops everyone who sends CWD
static void EmptyFolder(const QString & Folder)
{
  for (const QString & File : FilesBelow(Folder))
  {
    QFile::remove(File);
  }
}

//---------------------------------------------------------------------------
// what a TextEdit gives back: the same as TextFile::Displayed
static QString Through(const QString & Text)
{
  QTextDocument Document;
  Document.setPlainText(Text);
  return Document.toPlainText();
}

static QByteArray Saved(const QByteArray & Original, const QString & Encoding, const std::function<QString(QString)> & Edit)
{
  QString Text;
  CHECK(TextFile::Decode(Original, Encoding, Text));
  QByteArray Result;
  QString Error;
  CHECK(TextFile::Merge(Original, Edit(Through(Text)), Encoding, Result, Error));
  return Result;
}

static void TestTextFile()
{
  // the code pages: known letters of each
  QString Text;
  CHECK(TextFile::Decode("\xE8\xE6\x9A\x9E\xF0\xC8\xC6\x8A\x8E\xD0", QStringLiteral("Windows-1250"), Text));
  CHECK(Text == QStringLiteral("čćšžđČĆŠŽĐ"));
  CHECK(TextFile::Decode("\xE8\xE6\xB9\xBE\xF0", QStringLiteral("ISO-8859-2"), Text));
  CHECK(Text == QStringLiteral("čćšžđ"));
  CHECK(TextFile::Decode("\xE9\x80", QStringLiteral("Windows-1252"), Text));
  CHECK(Text == QStringLiteral("é€"));
  CHECK(TextFile::Decode("\xE9\x80", QStringLiteral("Latin-1"), Text));
  CHECK(Text == QString(QChar(0xE9)) + QChar(0x80));
  CHECK(!TextFile::Decode("x", QStringLiteral("Klingon"), Text));

  // every byte of every code page comes back
  QByteArray All;
  for (int I = 1; I < 256; I++)
  {
    All.append(char(I));
  }
  for (const QString & Encoding : TextFile::LegacyEncodings())
  {
    QByteArray Back;
    CHECK(TextFile::Decode(All, Encoding, Text));
    CHECK(TextFile::Encode(Text, Encoding, Back));
    CHECK(Back == All);
  }

  // UTF-8 is told from the rest; U+FFFD in a valid file is not an error
  CHECK(TextFile::IsUtf8("plain"));
  CHECK(TextFile::IsUtf8("\xC4\x8D"));
  CHECK(TextFile::IsUtf8("\xEF\xBF\xBD"));
  CHECK(TextFile::IsUtf8("\xEF\xBB\xBF" "bom"));
  CHECK(!TextFile::IsUtf8("caf\xE9"));
  CHECK(!TextFile::IsUtf8("\xC0\x80"));        // overlong
  CHECK(!TextFile::IsUtf8("\xED\xA0\x80"));    // a surrogate
  CHECK(!TextFile::IsUtf8("\xC4"));            // cut off

  // A8: one letter changed in a Latin-1 file with CR LF: nothing else moves
  QByteArray Latin = "caf\xE9\r\nthe\r\nend\r\n";
  CHECK(Saved(Latin, QStringLiteral("Latin-1"), [](QString T) { return T.replace(QStringLiteral("the"), QStringLiteral("THE")); })
        == "caf\xE9\r\nTHE\r\nend\r\n");
  // Windows-1250 with new text: in that encoding, with the file's line ending
  QByteArray Bosnian = "\xE8" "a\xE8" "kalica\r\n\x9A" "uma\r\n";
  CHECK(Saved(Bosnian, QStringLiteral("Windows-1250"), [](QString T) { return T + QStringLiteral("đak\nžaba\n"); })
        == "\xE8" "a\xE8" "kalica\r\n\x9A" "uma\r\n\xF0" "ak\r\n\x9E" "aba\r\n");
  // nothing changed: the same bytes, whatever is in the file
  QByteArray Odd = "a\r\nb\nc\rd\xC2\xA0" "e\xE2\x80\xA8" "f\xE2\x80\xA9g\r\n";
  CHECK(Saved(Odd, QLatin1String(TextFile::Utf8), [](QString T) { return T; }) == Odd);
  // mixed endings, no-break spaces and separators outside the change stay
  CHECK(Saved(Odd, QLatin1String(TextFile::Utf8), [](QString T) { return T.replace(QLatin1Char('b'), QLatin1Char('B')); })
        == "a\r\nB\nc\rd\xC2\xA0" "e\xE2\x80\xA8" "f\xE2\x80\xA9g\r\n");
  // a BOM stays
  CHECK(Saved("\xEF\xBB\xBF" "one\n", QLatin1String(TextFile::Utf8), [](QString T) { return T + QStringLiteral("two\n"); })
        == "\xEF\xBB\xBF" "one\ntwo\n");
  // no final newline stays that way; a file without line ends gets LF
  CHECK(Saved("a\r\nb", QLatin1String(TextFile::Utf8), [](QString T) { return T + QStringLiteral("c"); }) == "a\r\nbc");
  CHECK(Saved("a", QLatin1String(TextFile::Utf8), [](QString T) { return T + QStringLiteral("\nb"); }) == "a\nb");
  // deleting everything, and starting from nothing
  CHECK(Saved("a\r\nb\r\n", QLatin1String(TextFile::Utf8), [](QString) { return QString(); }).isEmpty());
  CHECK(Saved("", QLatin1String(TextFile::Utf8), [](QString) { return QStringLiteral("x\ny"); }) == "x\ny");

  // what the encoding cannot hold is refused, never replaced
  QByteArray Out;
  QString Error;
  CHECK(!TextFile::Merge("abc\n", QStringLiteral("ab€c\n"), QStringLiteral("Latin-1"), Out, Error));
  CHECK(Error.contains(QStringLiteral("€")));
  CHECK(!TextFile::Merge("abc\n", QStringLiteral("😀\n"), QStringLiteral("Windows-1250"), Out, Error));
  CHECK(Error.contains(QStringLiteral("😀")));
  // the editor cannot hand over half a character either
  CHECK(!TextFile::Encode(QString(QChar(0xD83D)), QLatin1String(TextFile::Utf8), Out));
  // a file that is not in the encoding claimed is not touched
  CHECK(!TextFile::Merge("caf\xE9", QStringLiteral("cafe"), QLatin1String(TextFile::Utf8), Out, Error));

  // Displayed is what a TextEdit does, on anything
  QRandomGenerator Random(4711);
  const QList<QChar> Parts = {u'a', u'b', u'\r', u'\n', u'\t', QChar(0xA0), QChar(0x2028), QChar(0x2029), QChar(0xFDD0), QChar(0xFDD1), QChar(0x10D), u' '};
  for (int Round = 0; Round < 3000; Round++)
  {
    QString Sample;
    int Length = Random.bounded(30);
    for (int I = 0; I < Length; I++)
    {
      Sample.append(Parts.at(Random.bounded(Parts.size())));
    }
    CHECK(TextFile::Displayed(Sample) == Through(Sample));
    // and a random edit keeps the rest of the file: what is shown is what was asked for
    QString Shown = Through(Sample);
    int At = Shown.isEmpty() ? 0 : Random.bounded(Shown.size());
    QString Edited = Shown.left(At) + QStringLiteral("X\nY") + Shown.mid(At + Random.bounded(3));
    QByteArray Original = Sample.toUtf8();
    QByteArray Merged;
    CHECK(TextFile::Merge(Original, Edited, QLatin1String(TextFile::Utf8), Merged, Error));
    CHECK(Through(QString::fromUtf8(Merged)) == Edited);
  }

  CHECK(TextFile::LineEnding("a\nb\n") == QStringLiteral("LF"));
  CHECK(TextFile::LineEnding("a\r\nb\r\n") == QStringLiteral("CRLF"));
  CHECK(TextFile::LineEnding("a\rb\r") == QStringLiteral("CR"));
  CHECK(TextFile::LineEnding("a\r\nb\n") == QStringLiteral("Mixed"));
  CHECK(TextFile::LineEnding("a").isEmpty());
  CHECK(TextFile::Sample(QStringLiteral("ascii\n  čačkalica \nnext")) == QStringLiteral("čačkalica"));
}

// M10: a device that fails in the middle
class BrokenDevice : public QIODevice
{
public:
  explicit BrokenDevice(bool Fail) : FFail(Fail) {}
  bool isSequential() const override { return true; }
  bool atEnd() const override { return FSent >= 3 && !FFail; }

protected:
  qint64 readData(char * Data, qint64) override
  {
    if (FSent < 3)
    {
      FSent++;
      Data[0] = 'x';
      return 1;
    }
    return FFail ? -1 : 0;
  }
  qint64 writeData(const char *, qint64) override { return -1; }

private:
  bool FFail;
  int FSent = 0;
};

static void TestCopy()
{
  BrokenDevice Good(false);
  Good.open(QIODevice::ReadOnly);
  QBuffer Target;
  Target.open(QIODevice::WriteOnly);
  CHECK(TextFile::Copy(Good, Target));
  CHECK(Target.data() == "xxx");

  BrokenDevice Bad(true);
  Bad.open(QIODevice::ReadOnly);
  QBuffer Truncated;
  Truncated.open(QIODevice::WriteOnly);
  CHECK(!TextFile::Copy(Bad, Truncated));

  // M10: two documents of the same name are two files
  QTemporaryDir Dir;
  CHECK(TextFile::UniqueName(Dir.path(), QStringLiteral("report.pdf")) == QStringLiteral("report.pdf"));
  WriteFile(Dir.path() + QStringLiteral("/report.pdf"), "1");
  CHECK(TextFile::UniqueName(Dir.path(), QStringLiteral("report.pdf")) == QStringLiteral("report (2).pdf"));
  WriteFile(Dir.path() + QStringLiteral("/report (2).pdf"), "2");
  CHECK(TextFile::UniqueName(Dir.path(), QStringLiteral("report.pdf")) == QStringLiteral("report (3).pdf"));
  WriteFile(Dir.path() + QStringLiteral("/README"), "3");
  CHECK(TextFile::UniqueName(Dir.path(), QStringLiteral("README")) == QStringLiteral("README (2)"));
  WriteFile(Dir.path() + QStringLiteral("/a.tar.gz"), "4");
  CHECK(TextFile::UniqueName(Dir.path(), QStringLiteral("a.tar.gz")) == QStringLiteral("a.tar (2).gz"));
}

//---------------------------------------------------------------------------
// A3: without ICU, as on Android. Own thread: the choice of the comparison is per thread
static void TestSortingWithoutCollator()
{
  qputenv("NIXSCP_NO_COLLATOR", "1");
  QThread * Thread = QThread::create([]()
  {
    auto Less = [](const char * A, const char * B)
    {
      return StoredSessionList::CompareLogicalText(QString::fromUtf8(A), QString::fromUtf8(B)) < 0;
    };
    QStringList Names = {QStringLiteral("gamma"), QStringLiteral("Beta"), QStringLiteral("file10"), QStringLiteral("Zeta"),
      QStringLiteral("alpha"), QStringLiteral("file9")};
    std::sort(Names.begin(), Names.end(), [](const QString & A, const QString & B) { return StoredSessionList::CompareLogicalText(A, B) < 0; });
    CHECK(Names == (QStringList{QStringLiteral("alpha"), QStringLiteral("Beta"), QStringLiteral("file9"), QStringLiteral("file10"),
      QStringLiteral("gamma"), QStringLiteral("Zeta")}));
    CHECK(Less("alpha", "Zeta"));
    CHECK(Less("site2", "site10"));
    CHECK(Less("Site2", "site10"));
    CHECK(Less("site002", "site10"));
    // accented letters beside their base letters, in any case
    CHECK(Less("čaj", "Dan"));
    CHECK(Less("Čaj", "dan"));
    CHECK(Less("ćevap", "Đak"));
    CHECK(Less("đak", "Eva"));
    CHECK(Less("šuma", "Tuzla"));
    CHECK(Less("žaba", "ZZ") == Less("zaba", "ZZ"));
    // still a strict order
    CHECK(StoredSessionList::CompareLogicalText(QStringLiteral("a"), QStringLiteral("a")) == 0);
    CHECK(StoredSessionList::CompareLogicalText(QStringLiteral("A"), QStringLiteral("a")) != 0);
    CHECK(Less("A", "a") != Less("a", "A"));
    CHECK(Less("a1", "a1b"));
    CHECK(Less("1", "a"));
    CHECK(!Less("a", "1"));
  });
  Thread->start();
  Thread->wait();
  delete Thread;
  qunsetenv("NIXSCP_NO_COLLATOR");
}

//---------------------------------------------------------------------------
static void TestFileList()
{
  FileListModel Model;
  QList<FileListModel::Entry> Entries;
  FileListModel::Entry E;
  E.Name = QStringLiteral("ftp.txt");
  // A2: FTP gives UTC, the phone shows its own time zone
  E.Modified = QDateTime(QDate(2026, 1, 15), QTime(8, 38), QTimeZone::UTC);
  Entries.append(E);
  Model.SetEntries(Entries);
  QString Shown = Model.data(Model.index(0), FileListModel::DateRole).toString();
  CHECK(Shown == QLocale().toString(E.Modified.toLocalTime(), QLocale::ShortFormat));
  CHECK(Shown.contains(QStringLiteral("9:38")));   // TZ is Europe/Belgrade here

  // L10: thumbnails of a large folder, by index
  Entries.clear();
  for (int I = 0; I < 20000; I++)
  {
    E.Name = QStringLiteral("image%1.png").arg(I);
    Entries.append(E);
  }
  Model.SetEntries(Entries);
  QElapsedTimer Timer;
  Timer.start();
  for (int I = 0; I < 20000; I++)
  {
    Model.SetThumbnail(QStringLiteral("image%1.png").arg(I), QStringLiteral("file:///t%1").arg(I));
  }
  CHECK(Timer.elapsed() < 1000);   // walking the lists takes minutes
  CHECK(Model.data(Model.index(1), FileListModel::ThumbnailRole).toString() == QStringLiteral("file:///t1"));
  // after a filter the rows are others, and a hidden file is only remembered
  Model.setFilter(QStringLiteral("image19999"));
  Model.SetThumbnail(QStringLiteral("image19999.png"), QStringLiteral("file:///new"));
  Model.SetThumbnail(QStringLiteral("image5.png"), QStringLiteral("file:///hidden"));
  CHECK(Model.data(Model.index(0), FileListModel::ThumbnailRole).toString() == QStringLiteral("file:///new"));
  Model.setFilter(QString());
  Model.SetThumbnail(QStringLiteral("no such file"), QStringLiteral("x"));
}

//---------------------------------------------------------------------------
// M2: questions of two connections, one sheet
static void TestQuestions()
{
  MobileUserInterface UI;
  QStringList Shown;
  QObject::connect(&UI, &MobileUserInterface::promptRequested, &UI, [&](int, const QString & Name, const QString &, const QVariantList &)
  {
    Shown.append(Name);
  });
  QString ResultA;
  QString ResultB;
  auto Ask = [&UI](const QString & Name, QString & Result)
  {
    return QThread::create([&UI, Name, &Result]()
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("Password:"), false, QString()}};
      if (UI.PromptUser(UserInterface::pkPassword, Name, QString(), Prompts))
      {
        Result = Prompts[0].Result;
      }
    });
  };
  QThread * A = Ask(QStringLiteral("A"), ResultA);
  QThread * B = Ask(QStringLiteral("B"), ResultB);
  A->start();
  CHECK(Wait([&]() { return Shown.size() == 1; }));
  B->start();
  // the second waits: the sheet shows the first
  Wait([]() { return false; }, 200);
  CHECK(Shown == QStringList{QStringLiteral("A")});
  UI.answerPrompt(true, {QStringLiteral("password-of-A")});
  CHECK(Wait([&]() { return Shown.size() == 2; }));
  CHECK(Shown.last() == QStringLiteral("B"));
  UI.answerPrompt(true, {QStringLiteral("password-of-B")});
  A->wait();
  B->wait();
  delete A;
  delete B;
  CHECK(ResultA == QStringLiteral("password-of-A"));
  CHECK(ResultB == QStringLiteral("password-of-B"));

  // M5: a closed session's question ends, shown or waiting, and the next one comes up
  auto First = UI.NewAsker();
  auto Second = UI.NewAsker();
  auto Third = UI.NewAsker();
  QSignalSpy Cancelled(&UI, &MobileUserInterface::questionCancelled);
  std::atomic<int> Ended{0};
  auto Query = [&Ended](UserInterface * Ui, unsigned int * Answer)
  {
    return QThread::create([Ui, Answer, &Ended]()
    {
      *Answer = Ui->QueryUser(QStringLiteral("Q"), QString(), qaYes | qaNo);
      Ended++;
    });
  };
  unsigned int AnswerFirst = 0;
  unsigned int AnswerSecond = 0;
  unsigned int AnswerThird = 0;
  QStringList Queries;
  QObject::connect(&UI, &MobileUserInterface::queryRequested, &UI, [&](const QString & Text, const QString &, const QVariantList &) { Queries.append(Text); });
  QThread * T1 = Query(First.get(), &AnswerFirst);
  T1->start();
  CHECK(Wait([&]() { return Queries.size() == 1; }));
  QThread * T2 = Query(Second.get(), &AnswerSecond);
  T2->start();
  QThread * T3 = Query(Third.get(), &AnswerThird);
  Wait([]() { return false; }, 100);
  T3->start();
  Wait([]() { return false; }, 100);
  Second->Cancel();                // waiting
  CHECK(Wait([&]() { return Ended == 1; }));
  CHECK(AnswerSecond == qaNo);
  First->Cancel();                 // shown
  CHECK(Wait([&]() { return Ended == 2; }));
  CHECK(AnswerFirst == qaNo);
  CHECK(Cancelled.size() == 1);
  CHECK(Wait([&]() { return Queries.size() == 2; }));
  UI.answerQuery(qaYes);
  CHECK(Wait([&]() { return Ended == 3; }));
  CHECK(AnswerThird == qaYes);
  // cancelled: no more questions at all
  QList<Prompt> Prompts{Prompt{QStringLiteral("Password:"), false, QString()}};
  CHECK(!First->PromptUser(UserInterface::pkPassword, QString(), QString(), Prompts));
  CHECK(First->VerifyHostKey(QString(), 22, QString(), QString(), QString(), UserInterface::HostKeyUnknown) == UserInterface::HostKeyCancel);
  CHECK(First->QueryUser(QString(), QString(), qaOK) == qaOK);
  // a late answer of a closing sheet changes nothing
  UI.answerQuery(qaYes);
  for (QThread * T : {T1, T2, T3})
  {
    T->wait();
    delete T;
  }

  // L6: the passphrase is typed once for all connections of the session
  auto Session = UI.NewAsker();
  int Asked = 0;
  QObject::connect(&UI, &MobileUserInterface::promptRequested, &UI, [&](int Kind, const QString &, const QString &, const QVariantList &)
  {
    if (Kind == UserInterface::pkPassphrase)
    {
      Asked++;
      UI.answerPrompt(true, {QStringLiteral("secret %1").arg(Asked)});
    }
  });
  auto Connect = [&](int Times)
  {
    QString Result;
    QThread * T = QThread::create([&Result, Times, &Session]()
    {
      for (int I = 0; I < Times; I++)
      {
        QList<Prompt> One{Prompt{QStringLiteral("Passphrase:"), false, QString()}};
        Session->PromptUser(UserInterface::pkPassphrase, QString(), QString(), One);
        Result = One[0].Result;
      }
    });
    T->start();
    Wait([T]() { return T->isFinished(); });
    delete T;
    return Result;
  };
  CHECK(Connect(1) == QStringLiteral("secret 1"));
  CHECK(Connect(1) == QStringLiteral("secret 1"));
  CHECK(Asked == 1);
  // the same connection asking twice: it was wrong, the user is asked
  CHECK(Connect(2) == QStringLiteral("secret 2"));
  CHECK(Asked == 2);
}

//---------------------------------------------------------------------------
static QByteArray TestKey()
{
  return QByteArray(CloudCrypto::KeyLength, 'k');
}

static void TurnEncryptionOn()
{
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.DekPassword = "wrap";
  ConfigVault::SetKey(TestKey(), Wrap);
  CHECK(ConfigVault::Enabled());
}

// S16: what names sites, folders and keys is encrypted
static void TestPrivateState(const QString & Folder)
{
  QString File = Folder + QStringLiteral("/state/sites.dat");
  {
    PrivateState State(File);
    // before the unlock nothing may be written over the file
    CHECK(!State.SetStrings(QStringLiteral("x"), {QStringLiteral("y")}));
    CHECK(!QFile::exists(File));
    State.Load();
    CHECK(State.SetStrings(QStringLiteral("favourites"), {QStringLiteral("Secret site")}));
    CHECK(ReadFile(File).contains("Secret site"));
    CHECK((QFile::permissions(File) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
    TurnEncryptionOn();
    State.Reseal();
    CHECK(!ReadFile(File).contains("Secret site"));
    CHECK(ConfigVault::IsSealedFile(File));
    CHECK(State.SetStrings(QStringLiteral("key/Secret site"), {QStringLiteral("prod-key")}));
    QByteArray Sealed = ReadFile(File);
    CHECK(!Sealed.contains("Secret site"));
    CHECK(!Sealed.contains("prod-key"));
  }
  {
    PrivateState State(File);
    State.Load();
    CHECK(State.Strings(QStringLiteral("favourites")) == QStringList{QStringLiteral("Secret site")});
    CHECK(State.Strings(QStringLiteral("key/Secret site")) == QStringList{QStringLiteral("prod-key")});
    // encryption off again: the file follows
    ConfigVault::Disable();
    State.Reseal();
    CHECK(ReadFile(File).contains("Secret site"));
  }
  ConfigVault::Forget();

  // the phone's settings hold none of it
  MobileSettings Settings;
  Settings.Private().Load();
  Settings.setSiteKey(QStringLiteral("Hidden site"), QStringLiteral("hidden-key"));
  Settings.addRecentFolder(QStringLiteral("Hidden site"), QStringLiteral("/var/hidden"));
  CHECK(Settings.siteKey(QStringLiteral("Hidden site")) == QStringLiteral("hidden-key"));
  CHECK(Settings.recentFolders(QStringLiteral("Hidden site")) == QStringList{QStringLiteral("/var/hidden")});
  Settings.setTimeout(20);
  QString Ini = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mobile.ini");
  QByteArray Plain = ReadFile(Ini);
  CHECK(!Plain.contains("hidden"));
  CHECK(!Plain.contains(QByteArray("Hidden site").toBase64(QByteArray::Base64UrlEncoding)));
  Settings.setSiteKey(QStringLiteral("Hidden site"), QString());
  CHECK(Settings.siteKey(QStringLiteral("Hidden site")).isEmpty());
}

// A configuration no password opens: the lock says so and starts nothing, and the
// way out is the user's
static void TestAppLockProblems(const QString & Folder)
{
  QString Dir = Folder + QStringLiteral("/lock");
  CHECK(QDir().mkpath(Dir));
  QString Ini = Dir + QStringLiteral("/nixscp.ini");
  QString State = Dir + QStringLiteral("/nixscp-cloud.ini");
  QString Previous = Configuration::Instance().IniFileName();
  Configuration::Instance().SetIniFileName(Ini);
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("user-1");
  Wrap.Kdf.MemoryKib = 8 * 1024;
  Wrap.Kdf.Iterations = 1;
  CHECK(CloudCrypto::WrapDek(Wrap, Dek, QStringLiteral("a phone password"), QByteArray()));
  ConfigVault::SetKey(Dek, Wrap);
  IniFile File;
  File.WriteRaw(QStringLiteral("Test"), QStringLiteral("Key"), QStringLiteral("value"));
  CHECK(File.Save(State));
  CHECK(File.Save(Ini));
  QByteArray Sealed = ReadFile(Ini);
  ConfigVault::Forget();
  auto WriteConfig = [&Ini](const QByteArray & Data)
  {
    QFile Out(Ini);
    CHECK(Out.open(QIODevice::WriteOnly | QIODevice::Truncate));
    Out.write(Data);
  };
  // Replaced by a plain file, removed, damaged, of NixSCP 1.2.3
  struct { QByteArray Data; const char * Says; } const Cases[] =
  {
    {QByteArray("[Sessions\\Planted]\nHostName=evil.example\n"), "replaced"},
    {QByteArray(), "replaced"},
    {QByteArray("NXS1 anything"), "damaged"},
    {QByteArray(Sealed).replace("\"fmt\":2", "\"fmt\":1"), "1.2.3"},
  };
  for (const auto & Case : Cases)
  {
    if (Case.Data.isEmpty())
    {
      QFile::remove(Ini);
    }
    else
    {
      WriteConfig(Case.Data);
    }
    int Started = 0;
    AppLock Lock(Ini, [&Started]() { Started++; });
    Lock.Begin();
    CHECK(Lock.locked());
    CHECK(Started == 0);
    CHECK(Lock.problem().contains(QLatin1String(Case.Says)));
  }
  // A good file is no problem: it only asks for the password
  WriteConfig(Sealed);
  {
    int Started = 0;
    AppLock Lock(Ini, [&Started]() { Started++; });
    Lock.Begin();
    CHECK(Lock.locked() && Lock.problem().isEmpty());
    Lock.startOver();
    CHECK(QFile::exists(Ini) && (Started == 0));
  }
  // Starting over, when the user asks: everything of the configuration goes
  WriteConfig("NXS1 anything");
  {
    int Started = 0;
    AppLock Lock(Ini, [&Started]() { Started++; });
    Lock.Begin();
    Lock.startOver();
    CHECK(!Lock.locked() && Lock.problem().isEmpty());
    CHECK(Started == 1);
    CHECK(!QFile::exists(Ini) && !QFile::exists(State));
  }
  Configuration::Instance().SetIniFileName(Previous);
}

static void TestCache()
{
  // leftovers of a killed app
  QString Root = MobileCache::Root();
  WriteFile(Root + QStringLiteral("/edit/1/2/secret.conf"), "x");
  WriteFile(Root + QStringLiteral("/open/1/secret.pdf"), "x");
  WriteFile(Root + QStringLiteral("/upload/1/1/a"), "x");
  WriteFile(Root + QStringLiteral("/archive/1/a"), "x");
  WriteFile(Root + QStringLiteral("/thumbfetch/1/a.png"), "x");
  WriteFile(Root + QStringLiteral("/thumbs/shared/a.png"), "x");
  WriteFile(Root + QStringLiteral("/thumbs/7/b.png"), "x");
  // a thumbnail cache without encryption lives on, a session's folder does not
  MobileCache::Clean(false);
  CHECK(FilesBelow(Root) == QStringList{Root + QStringLiteral("/thumbs/shared/a.png")});
  // with encryption nothing is left
  MobileCache::Clean(true);
  CHECK(FilesBelow(Root).isEmpty());

  // a session takes its folders with it, and only its own
  WriteFile(MobileCache::Folder(QStringLiteral("edit"), 3) + QStringLiteral("/a"), "x");
  WriteFile(MobileCache::Folder(QStringLiteral("edit"), 4) + QStringLiteral("/b"), "x");
  MobileCache::RemoveSession(3);
  CHECK(FilesBelow(Root) == QStringList{MobileCache::Folder(QStringLiteral("edit"), 4) + QStringLiteral("/b")});
  MobileCache::Clean(true);

  CHECK(MobileCache::Thumbnails(5).endsWith(QStringLiteral("/thumbs/shared")));
  TurnEncryptionOn();
  CHECK(MobileCache::Thumbnails(5).endsWith(QStringLiteral("/thumbs/5")));
  ConfigVault::Forget();
}

//---------------------------------------------------------------------------
static QByteArray GenerateKeyFile(ssh_keytypes_e Type)
{
  ssh_key Key = nullptr;
  CHECK(ssh_pki_generate(Type, 0, &Key) == SSH_OK);
  char * Base64 = nullptr;
  CHECK(ssh_pki_export_privkey_base64(Key, nullptr, nullptr, nullptr, &Base64) == SSH_OK);
  QByteArray Result(Base64);
  ssh_string_free_char(Base64);
  ssh_key_free(Key);
  return Result;
}

// S16, M21: keys
static void TestKeyStore()
{
  QDir(KeyStore::Folder()).removeRecursively();
  KeyStore Keys;
  // M16's other half: a key file not in the store has no name
  CHECK(Keys.nameOf(QStringLiteral("/elsewhere/key")).isEmpty());

  // L3: generated on its own thread
  QSignalSpy Generated(&Keys, &KeyStore::generated);
  // the page shows the public key from inside the signal
  QString ShownPublic;
  QObject::connect(&Keys, &KeyStore::generated, &Keys, [&](const QString & Name, const QString &) { ShownPublic = Keys.publicKey(Name); });
  Keys.generate(QStringLiteral("plain"), QStringLiteral("ed25519"), QString());
  CHECK(Keys.generating());
  CHECK(Wait([&]() { return !Generated.isEmpty(); }));
  CHECK(!Keys.generating());
  CHECK(Generated.first().at(0).toString() == QStringLiteral("plain"));
  CHECK(ShownPublic.startsWith(QStringLiteral("ssh-ed25519 ")));
  QString PlainKey = Keys.path(QStringLiteral("plain"));
  CHECK(ReadFile(PlainKey).contains("PRIVATE KEY"));
  CHECK(!Keys.publicKey(QStringLiteral("plain")).isEmpty());

  // with encryption the file is encrypted from the start: nothing plain on the disk, ever
  TurnEncryptionOn();
  Generated.clear();
  Keys.generate(QStringLiteral("sealed"), QStringLiteral("ed25519"), QString());
  CHECK(Wait([&]() { return !Generated.isEmpty(); }));
  CHECK(ConfigVault::IsSealedFile(Keys.path(QStringLiteral("sealed"))));
  QString ImportFile = QDir::tempPath() + QStringLiteral("/nixscp-mobile-import-key");
  WriteFile(ImportFile, GenerateKeyFile(SSH_KEYTYPE_ED25519));
  QString Imported = Keys.importKey(ImportFile);
  QFile::remove(ImportFile);
  CHECK(!Imported.isEmpty());
  CHECK(ConfigVault::IsSealedFile(Keys.path(Imported)));

  // the public part of an encrypted key, without a decrypted file anywhere
  QFile::remove(Keys.path(Imported) + QStringLiteral(".pub"));
  MobileCache::Clean(true);
  QString Public = Keys.publicKey(Imported);
  CHECK(Public.startsWith(QStringLiteral("ssh-ed25519 ")));
  CHECK(FilesBelow(MobileCache::Root()).isEmpty());

  // SealAll: a plain key becomes encrypted and still opens
  QByteArray Before = ReadFile(PlainKey);
  KeyStore::SealAll();
  CHECK(ConfigVault::IsSealedFile(PlainKey));
  QByteArray Open;
  CHECK(ConfigVault::Open(ReadFile(PlainKey), Open));
  CHECK(Open == Before);

  // M21: when the encrypted file cannot be written the key stays as it was
  ConfigVault::Forget();
  QString Stubborn = Keys.path(QStringLiteral("stubborn"));
  WriteFile(Stubborn, "-----BEGIN OPENSSH PRIVATE KEY-----\nonly copy\n");
  TurnEncryptionOn();
  QFile::setPermissions(KeyStore::Folder(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  KeyStore::SealAll();
  QFile::setPermissions(KeyStore::Folder(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  CHECK(ReadFile(Stubborn).contains("only copy"));
  ConfigVault::Forget();
  QDir(KeyStore::Folder()).removeRecursively();
  QDir().mkpath(KeyStore::Folder());
}

// S16: known_hosts tells nothing about the hosts; libssh's reading is tested with a real connection below
static void TestHashKnownHosts()
{
  QString File = QDir::tempPath() + QStringLiteral("/nixscp-mobile-known_hosts");
  QByteArray Key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ";
  WriteFile(File, "# a comment\nsecret.example.com " + Key + "\n[other.example.com]:2222 " + Key + "\na.example.com,b.example.com " + Key + "\n");
  KeyStore::HashKnownHosts(File);
  QByteArray Hashed = ReadFile(File);
  CHECK(!Hashed.contains("example.com"));
  CHECK(Hashed.contains("# a comment"));
  CHECK(Hashed.count("|1|") == 4);   // a comma list becomes two lines
  CHECK(Hashed.count(Key) == 4);
  // the known form: |1|salt|HMAC-SHA1(salt, host)
  QList<QByteArray> Fields = Hashed.split('\n').at(1).split('|');
  CHECK(Fields.size() == 4);
  CHECK(QByteArray::fromBase64(Fields.at(2)).size() == 20);
  CHECK(QByteArray::fromBase64(Fields.at(3).split(' ').first()).size() == 20);
  // what is hashed stays as it is
  KeyStore::HashKnownHosts(File);
  CHECK(ReadFile(File) == Hashed);
  QFile::remove(File);
  KeyStore::HashKnownHosts(File);   // no file: nothing happens
  CHECK(!QFile::exists(File));
}

//---------------------------------------------------------------------------
// L1: on a real shell, with its own home
static void TestInstallKeyCommand()
{
  QTemporaryDir Home;
  CHECK(Home.isValid());
  auto Run = [&Home](const QString & Key)
  {
    QProcess Shell;
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("HOME"), Home.path());
    Shell.setProcessEnvironment(Env);
    Shell.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), RemoteSession::InstallKeyCommand(Key)});
    Shell.waitForFinished();
    return Shell.exitCode();
  };
  QString File = Home.path() + QStringLiteral("/.ssh/authorized_keys");
  // into nothing
  CHECK(Run(QStringLiteral("ssh-ed25519 AAAA first")) == 0);
  CHECK(ReadFile(File) == "ssh-ed25519 AAAA first\n");
  // the same key once
  CHECK(Run(QStringLiteral("ssh-ed25519 AAAA first")) == 0);
  CHECK(ReadFile(File) == "ssh-ed25519 AAAA first\n");
  // a file without a final newline: the old line stays whole and the new one is a line
  WriteFile(File, "ssh-rsa BBBB old");
  CHECK(Run(QStringLiteral("ssh-ed25519 CCCC new")) == 0);
  CHECK(ReadFile(File) == "ssh-rsa BBBB old\nssh-ed25519 CCCC new\n");
  // a name with a quote stays one argument
  CHECK(Run(QStringLiteral("ssh-ed25519 DDDD it's")) == 0);
  CHECK(ReadFile(File).endsWith("ssh-ed25519 DDDD it's\n"));
  // a key that cannot get there is reported
  QFile::setPermissions(File, QFileDevice::ReadOwner);
  CHECK(Run(QStringLiteral("ssh-ed25519 EEEE denied")) != 0);
  QFile::setPermissions(File, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// M19: a folder goes file by file
static void TestFilesToPublish()
{
  QTemporaryDir Dir;
  WriteFile(Dir.path() + QStringLiteral("/folder/a.txt"), "a");
  WriteFile(Dir.path() + QStringLiteral("/folder/sub/b.txt"), "b");
  WriteFile(Dir.path() + QStringLiteral("/single.txt"), "c");
  auto Files = AndroidFiles::FilesToPublish(Dir.path() + QStringLiteral("/folder"));
  std::sort(Files.begin(), Files.end());
  CHECK(Files.size() == 2);
  CHECK(Files.value(0).second == QStringLiteral("folder"));
  CHECK(Files.value(1).second == QStringLiteral("folder/sub"));
  CHECK(AndroidFiles::FilesToPublish(Dir.path() + QStringLiteral("/single.txt")).value(0).second.isEmpty());
}

// L17: the sign-in and the transfers each keep the service
static void TestKeepAlive()
{
  AndroidFiles Files;
  CHECK(!Files.KeptAlive());
  Files.keepAlive(true, QStringLiteral("Signing in"), -1, QStringLiteral("signin"));
  Files.keepAlive(true, QStringLiteral("file"), 50);
  Files.keepAlive(false, QString(), -1, QStringLiteral("signin"));
  CHECK(Files.KeptAlive());
  Files.keepAlive(true, QStringLiteral("Signing in"), -1, QStringLiteral("signin"));
  Files.keepAlive(false, QString());
  CHECK(Files.KeptAlive());
  Files.keepAlive(false, QString(), -1, QStringLiteral("signin"));
  CHECK(!Files.KeptAlive());
  CHECK(Files.fileUrl(QStringLiteral("/a/b#c?d%e.png")).toLocalFile() == QStringLiteral("/a/b#c?d%e.png"));   // L7

  // M-H6: an editor whose session ended saves a copy: in the file's encoding,
  // in UTF-8 when that cannot hold the text (nothing is replaced)
  QString Copy = Files.saveCopy(QStringLiteral("copy.txt"), QStringLiteral("čaj\n"), QStringLiteral("Windows-1250"));
  CHECK(ReadFile(Copy) == "\xE8" "aj\n");
  Copy = Files.saveCopy(QStringLiteral("copy2.txt"), QStringLiteral("€ and č"), QStringLiteral("Latin-1"));
  CHECK(QString::fromUtf8(ReadFile(Copy)) == QStringLiteral("€ and č"));
  MobileCache::Clean(true);
}

// L16
static void TestLocalBrowser()
{
  LocalBrowser Phone;
  QSignalSpy Failed(&Phone, &LocalBrowser::failed);
  Phone.cd(QStringLiteral("/no/such/folder/here"));
  CHECK(Failed.size() == 1);
  CHECK(Failed.first().at(0).toString().contains(QStringLiteral("/no/such/folder/here")));
}

//---------------------------------------------------------------------------
// M20, L18, M6 and the site list
static void TestSites()
{
  Configuration & Config = Configuration::Instance();
  MobileSettings Settings;
  Settings.Private().Load();
  SitesModel Sites(&Settings);
  Sites.LoadState();
  QString Error = Sites.saveSite(QString(), QStringLiteral("One"), QStringLiteral("SFTP"), QStringLiteral("one.example.com"), 0,
    QStringLiteral("u"), QString(), false, QString(), QString());
  CHECK(Error.isEmpty());
  QSignalSpy Reset(&Sites, &QAbstractItemModel::modelReset);
  Sites.reload();
  Sites.reload();
  CHECK(Reset.isEmpty());   // M20: nothing new, the list stays where it is
  QString Copy = Sites.duplicateSite(QStringLiteral("One"));
  CHECK(!Copy.isEmpty());
  CHECK(!Reset.isEmpty());

  // the phone's lists survive a restart of the model, and hold no state in mobile.ini
  Sites.toggleFavourite(QStringLiteral("One"));
  Sites.addRecent(QStringLiteral("One"));
  {
    MobileSettings Again;
    Again.Private().Load();
    SitesModel Other(&Again);
    Other.LoadState();
    Other.reload();
    bool Favourite = false;
    for (int Row = 0; Row < Other.rowCount(); Row++)
    {
      Favourite = Favourite || Other.data(Other.index(Row), SitesModel::FavouriteRole).toBool();
    }
    CHECK(Favourite);
  }
  CHECK(!ReadFile(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mobile.ini")).contains("Favourites"));

  // L18: a failed save is told
  QSignalSpy Failed(&Sites, &SitesModel::failed);
  QString Ini = Config.IniFileName();
  QString Folder = QFileInfo(Ini).absolutePath();
  QFile::setPermissions(Ini, QFileDevice::ReadOwner);
  QFile::setPermissions(Folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  Sites.removeSite(Copy);
  QFile::setPermissions(Folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QFile::setPermissions(Ini, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  CHECK(!Failed.isEmpty());
  Sites.removeSite(QStringLiteral("One"));
  Config.Save();
}

//---------------------------------------------------------------------------
// Site notes and note items

// the first row of the site list showing Name (-1: not listed); Section: only in that section
static int RowOf(const SitesModel & Sites, const QString & Name, const QString & Section = QString())
{
  for (int Row = 0; Row < Sites.rowCount(); Row++)
  {
    QModelIndex Index = Sites.index(Row);
    if (!Sites.data(Index, SitesModel::IsHeaderRole).toBool() && (Sites.data(Index, SitesModel::NameRole).toString() == Name) &&
        (Section.isEmpty() || (Sites.data(Index, SitesModel::SectionRole).toString() == Section)))
    {
      return Row;
    }
  }
  return -1;
}

static QVariant RowData(const SitesModel & Sites, const QString & Name, int Role)
{
  int Row = RowOf(Sites, Name);
  return (Row >= 0) ? Sites.data(Sites.index(Row), Role) : QVariant();
}

// what the configuration stores of a site, without its password (stored
// with random padding)
static QByteArray StoredText(SessionData Data)
{
  Data.Password.clear();
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Site"), true);
  Data.Save(Storage);
  return File.SaveToData();
}

// The site page's note: shown, saved on its own (nothing else of the site
// changes), refused when too long or with control characters
static void TestSiteNotes()
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Stored = Config.StoredSessions();
  MobileSettings Settings;
  Settings.Private().Load();
  SitesModel Sites(&Settings);
  Sites.LoadState();
  CHECK(Sites.saveSite(QString(), QStringLiteral("Shop"), QStringLiteral("SFTP"), QStringLiteral("shop.example.com"), 2222,
    QStringLiteral("deploy"), QStringLiteral("S3cret"), false, QStringLiteral("/var/www"), QString()).isEmpty());
  std::shared_ptr<SessionData> Shop = Stored.FindByName(QStringLiteral("Shop"));
  CHECK(Shop != nullptr);
  if (Shop == nullptr)
  {
    return;
  }
  // settings the phone does not show stay too
  Shop->Color = 0x336699;
  Shop->LocalDirectory = QStringLiteral("/home/me/shop");
  Config.Save();
  QVariantMap Data = Sites.siteData(QStringLiteral("Shop"));
  CHECK(Data.value(QStringLiteral("note")).toString().isEmpty());
  CHECK(Data.value(QStringLiteral("canEditNote")).toBool());
  CHECK(!Data.value(QStringLiteral("noteItem")).toBool());
  CHECK(!RowData(Sites, QStringLiteral("Shop"), SitesModel::HasNoteRole).toBool());

  SessionData Before = *Shop;
  const QString Note = QStringLiteral("\nLogs: /var/log/shop\nNever restart MySQL during business hours.\tTabs are fine.\r\n");
  QSignalSpy Rows(&Sites, &SitesModel::rowsChanged);
  CHECK(Sites.saveNote(QStringLiteral("Shop"), Note).isEmpty());
  CHECK(!Rows.isEmpty());
  Shop = Stored.FindByName(QStringLiteral("Shop"));
  CHECK(Shop->Note == Note);
  CHECK(Sites.siteData(QStringLiteral("Shop")).value(QStringLiteral("note")).toString() == Note);
  // nothing else of the site
  Before.Note = Note;
  CHECK(StoredText(*Shop) == StoredText(Before));
  CHECK(Shop->Password == Before.Password);
  CHECK(Shop->Password == QStringLiteral("S3cret"));
  // listed with a mark and its first line
  CHECK(RowData(Sites, QStringLiteral("Shop"), SitesModel::HasNoteRole).toBool());
  CHECK(RowData(Sites, QStringLiteral("Shop"), SitesModel::NoteLineRole).toString() == QStringLiteral("Logs: /var/log/shop"));
  // in the configuration file
  Config.Load();
  Shop = Stored.FindByName(QStringLiteral("Shop"));
  CHECK((Shop != nullptr) && (Shop->Note == Note) && (Shop->Color == 0x336699) && (Shop->PortNumber == 2222));
  if (Shop == nullptr)
  {
    return;
  }
  Sites.reload();

  // refused: what AgentAccess::NoteAcceptable refuses; the stored note stays
  const QString Limit(AgentAccess::MaxNoteBytes, QLatin1Char('x'));
  CHECK(Sites.saveNote(QStringLiteral("Shop"), Limit).isEmpty());
  QString Reason = Sites.saveNote(QStringLiteral("Shop"), Limit + QLatin1Char('x'));
  CHECK_MSG(Reason.contains(QStringLiteral("too long")), Reason);
  // bytes of UTF-8, not characters
  Reason = Sites.saveNote(QStringLiteral("Shop"), QString(AgentAccess::MaxNoteBytes / 2 + 1, QChar(0x10D)));
  CHECK_MSG(Reason.contains(QStringLiteral("too long")), Reason);
  Reason = Sites.saveNote(QStringLiteral("Shop"), QStringLiteral("bell") + QChar(7));
  CHECK_MSG(Reason.contains(QStringLiteral("control characters")), Reason);
  Reason = Sites.saveNote(QStringLiteral("Shop"), QStringLiteral("delete") + QChar(0x7F));
  CHECK_MSG(Reason.contains(QStringLiteral("control characters")), Reason);
  CHECK(Stored.FindByName(QStringLiteral("Shop"))->Note == Limit);
  CHECK(!Sites.saveNote(QStringLiteral("gone"), QStringLiteral("x")).isEmpty());
  // the size shown under the note
  CHECK(Sites.noteSize(QStringLiteral("abc")) == QStringLiteral("3 bytes of 64 KB"));
  CHECK(Sites.noteSize(QString(2150, QLatin1Char('a'))) == QStringLiteral("2.1 KB of 64 KB"));
  CHECK(Sites.noteSize(Limit) == QStringLiteral("64.0 KB of 64 KB"));
  CHECK(Sites.noteBytes(QString(QChar(0x10D))) == 2);
  CHECK(Sites.maxNoteBytes() == 64 * 1024);
  // the site page's Save keeps the note
  CHECK(Sites.saveNote(QStringLiteral("Shop"), Note).isEmpty());
  CHECK(Sites.saveSite(QStringLiteral("Shop"), QStringLiteral("Shop"), QStringLiteral("SFTP"), QStringLiteral("shop2.example.com"), 0,
    QStringLiteral("deploy"), QString(), true, QStringLiteral("/"), QString()).isEmpty());
  Shop = Stored.FindByName(QStringLiteral("Shop"));
  CHECK((Shop->Note == Note) && (Shop->HostName == QStringLiteral("shop2.example.com")));
  // an empty note: no mark
  CHECK(Sites.saveNote(QStringLiteral("Shop"), QString()).isEmpty());
  CHECK(!RowData(Sites, QStringLiteral("Shop"), SitesModel::HasNoteRole).toBool());
  Sites.removeSite(QStringLiteral("Shop"));
}

// Note items: created with New note, listed, opened as notes, never
// connected (nor checked or put on the home screen), renamed and deleted
static void TestNoteItems()
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Stored = Config.StoredSessions();
  MobileSettings Settings;
  Settings.Private().Load();
  SitesModel Sites(&Settings);
  Sites.LoadState();
  CHECK(Sites.saveSite(QString(), QStringLiteral("Office/Router"), QStringLiteral("SFTP"), QStringLiteral("192.168.1.1"), 0,
    QStringLiteral("admin"), QString(), false, QString(), QString()).isEmpty());

  // New note: folders with "/", a taken name gets " (2)"
  QSignalSpy Failed(&Sites, &SitesModel::failed);
  QString Name = Sites.newNote(QStringLiteral(" /Office//Network/ "));
  CHECK_MSG(Name == QStringLiteral("Office/Network"), Name);
  CHECK(Sites.newNote(QStringLiteral("Office/Network")) == QStringLiteral("Office/Network (1)"));
  CHECK(Sites.newNote(QStringLiteral(" / ")).isEmpty());
  CHECK(Failed.size() == 1);
  std::shared_ptr<SessionData> Item = Stored.FindByName(Name);
  CHECK((Item != nullptr) && Item->NoteOnly && Item->Note.isEmpty() && !Item->CanLogin() && !Item->CanOpen());
  if (Item == nullptr)
  {
    return;
  }
  CHECK(Sites.isNote(Name));
  CHECK(!Sites.isNote(QStringLiteral("Office/Router")));
  CHECK(!Sites.isNote(QStringLiteral("gone")));

  // listed in its folder, as a note
  CHECK(RowOf(Sites, Name, QStringLiteral("Office")) >= 0);
  CHECK(RowData(Sites, Name, SitesModel::NoteItemRole).toBool());
  CHECK(!RowData(Sites, QStringLiteral("Office/Router"), SitesModel::NoteItemRole).toBool());
  CHECK(RowData(Sites, Name, SitesModel::TitleRole).toString() == QStringLiteral("Network"));
  CHECK(RowData(Sites, Name, SitesModel::HostRole).toString().isEmpty());
  CHECK(RowData(Sites, Name, SitesModel::ProtocolRole).toString().isEmpty());
  CHECK(Sites.saveNote(Name, QStringLiteral("Wi-Fi: office-5G\nVPN: vpn.example.com (ask Ana for an account)")).isEmpty());
  CHECK(RowData(Sites, Name, SitesModel::NoteLineRole).toString() == QStringLiteral("Wi-Fi: office-5G"));
  // a note item has no mark of its own: it is a note
  CHECK(RowData(Sites, Name, SitesModel::HasNoteRole).toBool());
  QVariantMap Data = Sites.siteData(Name);
  CHECK(Data.value(QStringLiteral("noteItem")).toBool());
  CHECK(Data.value(QStringLiteral("note")).toString().startsWith(QStringLiteral("Wi-Fi")));
  // found by its name
  Sites.setFilter(QStringLiteral("network"));
  CHECK(Sites.matches() == 2);
  Sites.setFilter(QString());
  // the site page cannot make it a server
  CHECK(!Sites.saveSite(Name, Name, QStringLiteral("SFTP"), QStringLiteral("x.example.com"), 0, QString(), QString(), false,
    QString(), QString()).isEmpty());
  Item = Stored.FindByName(Name);
  CHECK(Item->NoteOnly && Item->HostName.isEmpty());

  // never in the recent connections, never offered to connect to
  Sites.toggleFavourite(Name);
  CHECK(RowOf(Sites, Name, QStringLiteral("\x01" "favourites")) >= 0);
  Sites.addRecent(Name);
  Sites.addRecent(QStringLiteral("Office/Router"));
  CHECK(RowOf(Sites, Name, QStringLiteral("\x01" "recent")) < 0);
  CHECK(RowOf(Sites, QStringLiteral("Office/Router"), QStringLiteral("\x01" "recent")) >= 0);
  QSignalSpy ConnectOnly(&Sites, &SitesModel::connectOnlyChanged);
  Sites.setConnectOnly(true);
  CHECK(ConnectOnly.size() == 1);
  CHECK(RowOf(Sites, Name) < 0);
  CHECK(RowOf(Sites, QStringLiteral("Office/Network (1)")) < 0);
  CHECK(RowOf(Sites, QStringLiteral("Office/Router")) >= 0);
  Sites.setFilter(QStringLiteral("office"));
  CHECK(Sites.matches() == 1);
  Sites.setFilter(QString());
  Sites.setConnectOnly(false);
  CHECK(RowOf(Sites, Name, QStringLiteral("\x01" "favourites")) >= 0);

  // never connected: no tab, no connection, no check, no shortcut
  {
    MobileUserInterface UI;
    MobileQueue Queue;
    SessionManager Sessions(&UI, &Settings, &Queue);
    QSignalSpy Errors(&Sessions, &SessionManager::error);
    QSignalSpy Tabs(&Sessions, &SessionManager::sessionsChanged);
    Sessions.open(Name);
    CHECK(Errors.size() == 1);
    CHECK(!Errors.isEmpty() && Errors.first().at(1).toString().contains(QStringLiteral("is a note")));
    CHECK(Tabs.isEmpty() && (Sessions.count() == 0));
    // a session asked directly refuses too
    RemoteSession Session(&UI, &Settings, nullptr);
    QSignalSpy SessionErrors(&Session, &RemoteSession::error);
    QSignalSpy Closed(&Session, &RemoteSession::closed);
    Session.open(Name);
    CHECK((SessionErrors.size() == 1) && (Closed.size() == 1));
    CHECK(!Session.busy() && !Session.connected());
    SiteChecker Checker;
    QSignalSpy Finished(&Checker, &SiteChecker::finished);
    Checker.check(Name);
    CHECK((Finished.size() == 1) && !Finished.first().at(1).toBool() && !Checker.checking());
    AndroidFiles Files;
    CHECK(!Files.pinShortcut(Name));
  }

  // renamed: the note, the favourite and nothing else move
  CHECK(Sites.renameSite(Name, QStringLiteral("Office/Wi-Fi")).isEmpty());
  CHECK(Stored.FindByName(Name) == nullptr);
  Item = Stored.FindByName(QStringLiteral("Office/Wi-Fi"));
  CHECK((Item != nullptr) && Item->NoteOnly && Item->Note.startsWith(QStringLiteral("Wi-Fi")));
  CHECK(RowOf(Sites, QStringLiteral("Office/Wi-Fi"), QStringLiteral("\x01" "favourites")) >= 0);
  CHECK(RowOf(Sites, Name) < 0);
  CHECK(Sites.renameSite(QStringLiteral("Office/Wi-Fi"), QStringLiteral("Office/Wi-Fi")).isEmpty());
  // only the case changes (names are compared without case)
  CHECK(Sites.renameSite(QStringLiteral("Office/Wi-Fi"), QStringLiteral("Office/WI-FI")).isEmpty());
  CHECK((Stored.FindByName(QStringLiteral("Office/WI-FI")) != nullptr) && (Stored.FindByName(QStringLiteral("Office/WI-FI"))->Name == QStringLiteral("Office/WI-FI")));
  CHECK(Sites.renameSite(QStringLiteral("Office/WI-FI"), QStringLiteral("Office/Wi-Fi")).isEmpty());
  CHECK(!Sites.renameSite(QStringLiteral("Office/Wi-Fi"), QStringLiteral("Office/Router")).isEmpty());
  CHECK(!Sites.renameSite(QStringLiteral("Office/Wi-Fi"), QStringLiteral(" / ")).isEmpty());
  CHECK(!Sites.renameSite(QStringLiteral("gone"), QStringLiteral("here")).isEmpty());
  CHECK(Sites.cleanName(QStringLiteral(" /a//b/ ")) == QStringLiteral("a/b"));
  // still a note after loading the configuration
  Config.Load();
  Item = Stored.FindByName(QStringLiteral("Office/Wi-Fi"));
  CHECK((Item != nullptr) && Item->NoteOnly && Item->Note.contains(QStringLiteral("VPN")));
  Sites.reload();

  // deleted
  Sites.removeSite(QStringLiteral("Office/Wi-Fi"));
  Sites.removeSite(QStringLiteral("Office/Network (1)"));
  Sites.removeSite(QStringLiteral("Office/Router"));
  CHECK(Stored.FindByName(QStringLiteral("Office/Wi-Fi")) == nullptr);
  CHECK(RowOf(Sites, QStringLiteral("Office/Wi-Fi")) < 0);
  CHECK(RowOf(Sites, QStringLiteral("Office/Network (1)")) < 0);
}

// A team's note: its owners and admins edit it, its members read it
// (SitesModel::canEditNote). Against the fake cloud of the team tests; it
// signs in and turns on a data key, so it runs last.
static void TestTeamNotes(const QString & Folder)
{
  Fake Server;
  Server.Start();
  Srv = &Server;
  qputenv("NIXSCP_CLOUD_URL", Server.Base.toLatin1());
  Dir = Folder + QStringLiteral("/teams");
  QDir().mkpath(Dir);
  Boot();
  MobileSettings Settings;
  Settings.Private().Load();
  SitesModel Sites(&Settings);
  Sites.LoadState();

  // a member: the team shares no note, so the note is the member's own (private)
  Joined Member = MemberOf(QStringLiteral("Ops"), {{"db", "db.internal", "S3cret-password"}});
  const QString Db = Member.Folder + QStringLiteral("/db");
  CHECK_MSG(Site(Db) != nullptr, Db);
  Sites.reload();
  CHECK(RowOf(Sites, Db) >= 0);
  CHECK(Sites.canEditNote(Db));
  CHECK(Sites.siteData(Db).value(QStringLiteral("noteStatus")).toString().contains(QStringLiteral("private")));
  CHECK(Sites.saveNote(Db, QStringLiteral("My own reminder.")).isEmpty());
  CHECK((Site(Db) != nullptr) && Site(Db)->Note.isEmpty() && (Site(Db)->PrivateNote == QStringLiteral("My own reminder.")));
  CHECK(Sites.siteData(Db).value(QStringLiteral("note")).toString() == QStringLiteral("My own reminder."));
  // the team's note (as a sync brings it): members read it, the private one is hidden
  Site(Db)->Note = QStringLiteral("Restart with systemctl.");
  Site(Db)->NoteBy = QStringLiteral("owner@nixscp.test");
  Site(Db)->NoteAt = QStringLiteral("2026-11-12T08:00:00Z");
  CHECK(!Sites.canEditNote(Db));
  QVariantMap Data = Sites.siteData(Db);
  CHECK(!Data.value(QStringLiteral("canEditNote")).toBool());
  CHECK(Data.value(QStringLiteral("note")).toString() == QStringLiteral("Restart with systemctl."));
  CHECK_MSG(Data.value(QStringLiteral("noteStatus")).toString().contains(QStringLiteral("Last edited by a former member")), Data.value(QStringLiteral("noteStatus")).toString());
  QString Reason = Sites.saveNote(Db, QStringLiteral("Restarted it at 3 am."));
  CHECK_MSG(Reason.contains(QStringLiteral("owners and admins")), Reason);
  CHECK(Site(Db)->Note == QStringLiteral("Restart with systemctl."));

  // an admin edits the team's note once it is shared; until then it is private
  Joined Admin = MemberOf(QStringLiteral("Dev"), {{"api", "api.internal", "S3cret-password"}}, QStringLiteral("admin"));
  const QString Api = Admin.Folder + QStringLiteral("/api");
  CHECK_MSG(Site(Api) != nullptr, Api);
  CHECK(Sites.canEditNote(Api));
  CHECK(Sites.saveNote(Api, QStringLiteral("Deploy with make deploy.")).isEmpty());
  CHECK((Site(Api) != nullptr) && (Site(Api)->PrivateNote == QStringLiteral("Deploy with make deploy.")));
  CHECK(Wait([&](const auto & D) { Teams().SetNoteShared(Api, true, D); }));
  CHECK(Site(Api)->Note == QStringLiteral("Deploy with make deploy.") && Site(Api)->PrivateNote.isEmpty());
  CHECK(Sites.saveNote(Api, QStringLiteral("Deploy with make release.")).isEmpty());
  CHECK(Site(Api)->Note == QStringLiteral("Deploy with make release."));
  CHECK(Sites.siteData(Api).value(QStringLiteral("noteStatus")).toString().contains(QStringLiteral("Last edited by")));
  QString Owned = CreateTeam(QStringLiteral("Mine"));
  const QString Web = FolderOf(Owned) + QStringLiteral("/web");
  AddSite(Web);
  CHECK(Sites.canEditNote(Web));
  CHECK(Sites.saveNote(Web, QStringLiteral("Behind Cloudflare.")).isEmpty());
  AddSite(QStringLiteral("Personal"));
  CHECK(Sites.canEditNote(QStringLiteral("Personal")));
  Srv = nullptr;
}

// M18: a cloud pull brings the desktop's settings; the phone's stay over them
static void TestPhoneSettings()
{
  Configuration & Config = Configuration::Instance();
  MobileSettings Settings;
  Settings.setConfirmOverwrite(false);
  Settings.ApplyToConfiguration();
  CHECK(!Config.DefaultCopyParam().ConfirmOverwriting);
  Config.DefaultCopyParam().ConfirmOverwriting = true;   // what a pull does
  emit Config.Changed();
  CHECK(!Config.DefaultCopyParam().ConfirmOverwriting);
  Settings.setConfirmOverwrite(true);
  CHECK(Config.DefaultCopyParam().ConfirmOverwriting);
}

// M6: a shortcut to a site that is gone
static void TestDeletedSite()
{
  MobileUserInterface UI;
  MobileSettings Settings;
  Settings.Private().Load();
  MobileQueue Queue;
  SessionManager Sessions(&UI, &Settings, &Queue);
  QSignalSpy Errors(&Sessions, &SessionManager::error);
  Sessions.open(QStringLiteral("gone"));
  CHECK(Errors.size() == 1);
  CHECK(Wait([&]() { return Sessions.count() == 0; }));
}

// L3: names are resolved without blocking
static void TestSiteChecker()
{
  Configuration & Config = Configuration::Instance();
  SessionData Data = Config.StoredSessions().DefaultSettings();
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = 1;
  Config.StoredSessions().NewSession(QStringLiteral("Checked"), Data);
  SiteChecker Checker;
  QSignalSpy Finished(&Checker, &SiteChecker::finished);
  Checker.check(QStringLiteral("Checked"));
  CHECK(Checker.checking());
  CHECK(Finished.isEmpty());
  CHECK(Wait([&]() { return !Finished.isEmpty(); }));
  CHECK(!Checker.checking());
  CHECK(!Finished.first().at(1).toBool());
  Checker.check(QStringLiteral("Checked"));
  Checker.stop();
  CHECK(!Checker.checking());
  Config.StoredSessions().Remove(QStringLiteral("Checked"));
}

//---------------------------------------------------------------------------
// A port in front of a server: its connections can be cut, or never answered
class Proxy : public QTcpServer
{
public:
  explicit Proxy(quint16 TargetPort) : FTarget(TargetPort)
  {
    connect(this, &QTcpServer::newConnection, this, [this]()
    {
      while (hasPendingConnections())
      {
        QTcpSocket * Client = nextPendingConnection();
        Accepted++;
        if (Silent)
        {
          // a server that takes the connection and says nothing
          FSilent.append(Client);
          continue;
        }
        auto * Server = new QTcpSocket(this);
        Server->connectToHost(QHostAddress::LocalHost, FTarget);
        FPairs.append(qMakePair(Client, Server));
        connect(Client, &QTcpSocket::readyRead, Server, [this, Client, Server]()
        {
          QByteArray Data = Client->readAll();
          ClientBytes += Data.size();
          Server->write(Data);
        });
        connect(Server, &QTcpSocket::readyRead, Client, [Client, Server]() { Client->write(Server->readAll()); });
        connect(Client, &QTcpSocket::disconnected, Server, &QTcpSocket::disconnectFromHost);
        connect(Server, &QTcpSocket::disconnected, Client, &QTcpSocket::disconnectFromHost);
      }
    });
    listen(QHostAddress::LocalHost);
  }
  // cuts what is open now (new connections work, unless close() stopped them)
  void Cut()
  {
    for (auto & [Client, Server] : FPairs)
    {
      Client->abort();
      Server->abort();
    }
    FPairs.clear();
  }

  bool Silent = false;
  int Accepted = 0;
  qint64 ClientBytes = 0;

private:
  quint16 FTarget;
  QList<QPair<QTcpSocket *, QTcpSocket *>> FPairs;
  QList<QTcpSocket *> FSilent;
};

struct App
{
  MobileUserInterface UI;
  MobileSettings Settings;
  MobileQueue Queue;
  SessionManager Sessions{&UI, &Settings, &Queue};
  QStringList Errors;
  QStringList Prompts;
  int HostKeys = 0;
  int ChangedKeys = 0;
  // the last button offered (Abort, Cancel) instead of the first
  bool Refuse = false;
  App()
  {
    Settings.Private().Load();
    Settings.setTimeout(5);
    QObject::connect(&Sessions, &SessionManager::error, &UI, [this](const QString &, const QString & Message, const QString & Details)
    {
      Errors.append(Message + QLatin1Char(' ') + Details);
    });
    // an unknown server is accepted, as the user would
    QObject::connect(&UI, &MobileUserInterface::hostKeyRequested, &UI, [this](const QString &, int, const QString &, const QString &, int State)
    {
      HostKeys++;
      ChangedKeys += State;
      UI.answerHostKey(State ? 2 : 0);
    });
    QObject::connect(&UI, &MobileUserInterface::queryRequested, &UI, [this](const QString & Query, const QString &, const QVariantList & Buttons)
    {
      Prompts.append(Query);
      // Yes, Retry, OK...: the first one offered, as a tap
      UI.answerQuery(Buttons.value(Refuse ? Buttons.size() - 1 : 0).toMap().value(QStringLiteral("answer")).toInt());
    });
  }
  RemoteSession * Current() { return Sessions.current(); }
  bool Open(const QString & Site)
  {
    QSignalSpy Established(&Sessions, &SessionManager::established);
    Sessions.open(Site);
    if (!Wait([&]() { return !Established.isEmpty() || (Sessions.count() == 0); }))
    {
      return false;
    }
    return Wait([this]() { return (Sessions.count() > 0) && !Current()->busy(); }) && (Sessions.count() > 0);
  }
};

static QString AddSite(const QString & Name, TFSProtocol Protocol, int Port, const QString & User, const QString & Password,
  const QString & Directory = QString())
{
  Configuration & Config = Configuration::Instance();
  SessionData Data = Config.StoredSessions().DefaultSettings();
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port;
  Data.UserName = User;
  Data.Password = Password;
  Data.FSProtocol = Protocol;
  Data.RemoteDirectory = Directory;
  Data.TryAgent = false;
  Config.StoredSessions().NewSession(Name, Data);
  return Name;
}

static int SshPort() { return qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT"); }
static int FtpPort() { return qEnvironmentVariableIntValue("NIXSCP_TEST_FTP_PORT"); }
static QString FtpRoot() { return qEnvironmentVariable("NIXSCP_TEST_FTP_ROOT"); }

// an edit round trip as EditorPage does it
static QString BeginEdit(App & A, const QString & Name, QVariantMap * File = nullptr)
{
  QSignalSpy Ready(&A.Sessions, &SessionManager::editReady);
  A.Current()->editFile(Name);
  if (!Wait([&]() { return !Ready.isEmpty(); }))
  {
    return QString();
  }
  QString Local = Ready.first().at(1).toString();
  if (File != nullptr)
  {
    *File = A.Current()->readEdit(Local);
  }
  return Local;
}

static bool SaveEdit(App & A, const QString & Local, const QString & Text, const QString & Encoding)
{
  RemoteSession * Session = A.Current();
  QSignalSpy Saved(Session, &RemoteSession::editSaved);
  Session->saveText(Local, Text, Encoding);
  return Wait([&]() { return !Saved.isEmpty(); }) && Saved.first().at(1).toBool();
}

// A8, S16, M-H2, M1, M11, M22: editing over FTP, files checked on the server's disk
static void TestEditing()
{
  if ((FtpPort() == 0) || FtpRoot().isEmpty())
  {
    std::printf("SKIP editing (NIXSCP_TEST_FTP_PORT)\n");
    return;
  }
  QString Root = FtpRoot() + QStringLiteral("/mobile-edit");
  EmptyFolder(Root);
  QDir().mkpath(Root);
  QString Site = AddSite(QStringLiteral("Edit"), fsFTP, FtpPort(), qEnvironmentVariable("NIXSCP_TEST_FTP_USER", QStringLiteral("test")),
    qEnvironmentVariable("NIXSCP_TEST_FTP_PASSWORD", QStringLiteral("secret")), QStringLiteral("/mobile-edit"));
  App A;
  CHECK(A.Open(Site));
  if (A.Sessions.count() == 0)
  {
    std::printf("FAIL no FTP connection: %s\n", qPrintable(A.Errors.join(QLatin1Char('|'))));
    Failures++;
    return;
  }
  RemoteSession * Session = A.Current();
  // FTP's commands are not a shell: no terminal, no archive on the server
  CHECK(!Session->canRunCommands());

  // A8 end to end: Latin-1 with CR LF, one letter changed
  WriteFile(Root + QStringLiteral("/latin.txt"), "caf\xE9\r\nline\r\n");
  QVariantMap File;
  QString Local = BeginEdit(A, QStringLiteral("latin.txt"), &File);
  CHECK(!Local.isEmpty());
  CHECK(File.value(QStringLiteral("needsEncoding")).toBool());
  CHECK(File.value(QStringLiteral("encodings")).toStringList().contains(QStringLiteral("Windows-1250")));
  CHECK(File.value(QStringLiteral("encodings")).toStringList().contains(QStringLiteral("Latin-1")));
  // S16: from here on the file is in memory only
  CHECK(!QFile::exists(Local));
  CHECK(FilesBelow(MobileCache::Root()).isEmpty());
  File = Session->readEdit(Local, QStringLiteral("Latin-1"));
  CHECK(File.value(QStringLiteral("text")).toString() == QStringLiteral("café\r\nline\r\n"));
  CHECK(File.value(QStringLiteral("lineEnding")).toString() == QStringLiteral("CRLF"));
  CHECK(SaveEdit(A, Local, QStringLiteral("café\nLine\n"), QStringLiteral("Latin-1")));
  CHECK(ReadFile(Root + QStringLiteral("/latin.txt")) == "caf\xE9\r\nLine\r\n");
  // S16: no copy stays after the upload either
  CHECK(FilesBelow(MobileCache::Root()).isEmpty());
  // a second save merges with what the server has now
  CHECK(SaveEdit(A, Local, QStringLiteral("café\nLINE\n"), QStringLiteral("Latin-1")));
  CHECK(ReadFile(Root + QStringLiteral("/latin.txt")) == "caf\xE9\r\nLINE\r\n");
  // what the encoding cannot hold: an error, nothing reaches the server
  CHECK(!SaveEdit(A, Local, QStringLiteral("€uro\n"), QStringLiteral("Latin-1")));
  CHECK(ReadFile(Root + QStringLiteral("/latin.txt")) == "caf\xE9\r\nLINE\r\n");
  CHECK(A.Errors.last().contains(QStringLiteral("€")));
  A.Errors.clear();
  Session->closeEdit(Local);
  CHECK(!Session->readEdit(Local).value(QStringLiteral("error")).toString().isEmpty());

  // a binary file is refused; the editor's close leaves nothing
  WriteFile(Root + QStringLiteral("/binary.bin"), QByteArray("ab\0cd", 5));
  Local = BeginEdit(A, QStringLiteral("binary.bin"), &File);
  CHECK(!File.value(QStringLiteral("error")).toString().isEmpty());
  Session->closeEdit(Local);
  CHECK(FilesBelow(MobileCache::Root()).isEmpty());

  // UTF-8 opens without a question
  WriteFile(Root + QStringLiteral("/utf8.txt"), "čačkalica\n");
  Local = BeginEdit(A, QStringLiteral("utf8.txt"), &File);
  CHECK(!File.contains(QStringLiteral("needsEncoding")));
  CHECK(File.value(QStringLiteral("encoding")).toString() == QLatin1String(TextFile::Utf8));
  CHECK(File.value(QStringLiteral("text")).toString() == QStringLiteral("čačkalica\n"));

  // M11: a failed upload is tried again, the changes are not lost
  QString Target = Root + QStringLiteral("/utf8.txt");
  ::chmod(QFile::encodeName(Root).constData(), 0555);
  ::chmod(QFile::encodeName(Target).constData(), 0444);
  A.Refuse = true;   // the error is answered with Abort, or Retry would go on for ever
  bool Refused = !SaveEdit(A, Local, QStringLiteral("changed\n"), QLatin1String(TextFile::Utf8));
  A.Refuse = false;
  ::chmod(QFile::encodeName(Root).constData(), 0755);
  ::chmod(QFile::encodeName(Target).constData(), 0644);
  if (getuid() != 0)
  {
    CHECK(Refused);
    CHECK(ReadFile(Target) == "čačkalica\n");
    // S16: the failed copy does not stay either
    CHECK(FilesBelow(MobileCache::Root()).isEmpty());
    QSignalSpy Saved(Session, &RemoteSession::editSaved);
    Session->checkEdits();
    CHECK(Wait([&]() { return !Saved.isEmpty(); }));
    CHECK(Saved.first().at(1).toBool());
    CHECK(ReadFile(Target) == "changed\n");
  }
  A.Errors.clear();

  // M1: full storage: the file on the server stays as it was
  WriteFile(Target, "safe\n");
  Local = BeginEdit(A, QStringLiteral("utf8.txt"), &File);
  rlimit Old{};
  getrlimit(RLIMIT_FSIZE, &Old);
  rlimit Limit = Old;
  Limit.rlim_cur = 2;
  ::signal(SIGXFSZ, SIG_IGN);
  setrlimit(RLIMIT_FSIZE, &Limit);
  QSignalSpy Saved(Session, &RemoteSession::editSaved);
  Session->saveText(Local, QStringLiteral("a longer text than the disk takes\n"), QLatin1String(TextFile::Utf8));
  setrlimit(RLIMIT_FSIZE, &Old);
  CHECK(Wait([&]() { return !Saved.isEmpty(); }));
  CHECK(!Saved.first().at(1).toBool());
  Wait([]() { return false; }, 300);
  CHECK(ReadFile(Target) == "safe\n");
  CHECK(!A.Errors.isEmpty());
  A.Errors.clear();
  Session->closeEdit(Local);

  // A save during an upload: the upload is of the text it started with, and the
  // page hears of the end only when what it saved last is on the server
  {
    WriteFile(Target, "first\n");
    Local = BeginEdit(A, QStringLiteral("utf8.txt"), &File);
    QSignalSpy EditSaved(Session, &RemoteSession::editSaved);
    QSignalSpy Notices(Session, &RemoteSession::notice);
    Session->saveText(Local, QStringLiteral("second\n"), QLatin1String(TextFile::Utf8));
    Session->saveText(Local, QStringLiteral("third\n"), QLatin1String(TextFile::Utf8));
    CHECK(Wait([&]() { return !EditSaved.isEmpty(); }));
    CHECK(EditSaved.size() == 1);
    CHECK(EditSaved.first().at(1).toBool());
    CHECK(ReadFile(Target) == "third\n");
    CHECK(Notices.size() == 1);
    CHECK(FilesBelow(MobileCache::Root()).isEmpty());
    // what the server has is known: the same text again is nothing to send
    EditSaved.clear();
    CHECK(SaveEdit(A, Local, QStringLiteral("third\n"), QLatin1String(TextFile::Utf8)));
    // back to the text from before the upload, while it runs: that is a change too
    Session->saveText(Local, QStringLiteral("fourth\n"), QLatin1String(TextFile::Utf8));
    Session->saveText(Local, QStringLiteral("third\n"), QLatin1String(TextFile::Utf8));
    CHECK(Wait([&]() { return !EditSaved.isEmpty(); }));
    CHECK(EditSaved.first().at(1).toBool());
    CHECK(ReadFile(Target) == "third\n");
    // and the later saves still merge with what the file was
    CHECK(SaveEdit(A, Local, QStringLiteral("fifth\n"), QLatin1String(TextFile::Utf8)));
    CHECK(ReadFile(Target) == "fifth\n");
    // the same text again during the upload: one upload is enough
    EditSaved.clear();
    Notices.clear();
    Session->saveText(Local, QStringLiteral("sixth\n"), QLatin1String(TextFile::Utf8));
    Session->saveText(Local, QStringLiteral("sixth\n"), QLatin1String(TextFile::Utf8));
    CHECK(Wait([&]() { return !EditSaved.isEmpty(); }));
    Wait([]() { return false; }, 300);
    CHECK(EditSaved.size() == 1);
    CHECK(Notices.size() == 1);
    CHECK(ReadFile(Target) == "sixth\n");
    // the page leaves while the second upload is waiting: the upload still goes up
    EditSaved.clear();
    Session->saveText(Local, QStringLiteral("seventh\n"), QLatin1String(TextFile::Utf8));
    Session->saveText(Local, QStringLiteral("eighth\n"), QLatin1String(TextFile::Utf8));
    Session->closeEdit(Local);
    CHECK(Wait([&]() { return !EditSaved.isEmpty(); }));
    CHECK(ReadFile(Target) == "eighth\n");
    CHECK(!Session->readEdit(Local).value(QStringLiteral("error")).toString().isEmpty());
    // the second upload fails: nothing lost, tried again
    Local = BeginEdit(A, QStringLiteral("utf8.txt"), &File);
    EditSaved.clear();
    Session->saveText(Local, QStringLiteral("ninth\n"), QLatin1String(TextFile::Utf8));
    Session->saveText(Local, QStringLiteral("tenth\n"), QLatin1String(TextFile::Utf8));
    ::chmod(QFile::encodeName(Root).constData(), 0555);
    ::chmod(QFile::encodeName(Target).constData(), 0444);
    A.Refuse = true;
    bool Done = Wait([&]() { return !EditSaved.isEmpty(); });
    A.Refuse = false;
    ::chmod(QFile::encodeName(Root).constData(), 0755);
    ::chmod(QFile::encodeName(Target).constData(), 0644);
    CHECK(Done);
    if (Done && !EditSaved.first().at(1).toBool())
    {
      EditSaved.clear();
      Session->checkEdits();
      CHECK(Wait([&]() { return !EditSaved.isEmpty(); }));
    }
    CHECK(ReadFile(Target) == "tenth\n");
    A.Errors.clear();
    Session->closeEdit(Local);
  }

  // M-H2, M22: two tabs: their own folders
  WriteFile(Root + QStringLiteral("/same.txt"), "from A\n");
  QString LocalA = BeginEdit(A, QStringLiteral("same.txt"));
  CHECK(A.Open(Site));
  CHECK(A.Sessions.count() == 2);
  QString LocalB = BeginEdit(A, QStringLiteral("same.txt"));
  CHECK(!LocalA.isEmpty() && !LocalB.isEmpty());
  CHECK(QFileInfo(LocalA).path() != QFileInfo(LocalB).path());
  CHECK(QFile::exists(LocalA));   // still in another app's hands, not removed by the other tab
  QSignalSpy Opened(&A.Sessions, &SessionManager::opened);
  A.Sessions.setCurrentIndex(0);
  A.Current()->openFile(QStringLiteral("same.txt"));
  CHECK(Wait([&]() { return Opened.size() == 1; }));
  QString OpenA = Opened.at(0).at(0).toString();
  A.Sessions.setCurrentIndex(1);
  A.Current()->openFile(QStringLiteral("latin.txt"));
  CHECK(Wait([&]() { return Opened.size() == 2; }));
  CHECK(QFile::exists(OpenA));    // the other tab's opening does not take it away
  CHECK(QFileInfo(OpenA).path() != QFileInfo(Opened.at(1).at(0).toString()).path());

  // S16: the session ends: everything goes
  A.Sessions.closeAt(1);
  CHECK(Wait([&]() { return A.Sessions.count() == 1; }));
  CHECK(QFile::exists(LocalA));
  CHECK(!QFile::exists(LocalB));

  // an external editor's changes that could not be uploaded are kept for the user
  QFile Changed(LocalA);
  CHECK(Changed.open(QIODevice::Append));
  Changed.write("edited elsewhere\n");
  Changed.close();
  QSignalSpy Unsent(&A.Sessions, &SessionManager::editUnsent);
  A.Sessions.closeAt(0);
  CHECK(Wait([&]() { return A.Sessions.count() == 0; }));
  CHECK(Unsent.size() == 1);
  if (!Unsent.isEmpty())
  {
    CHECK(Unsent.first().at(0).toString() == Site);
    QString Kept = Unsent.first().at(1).toString();
    CHECK(ReadFile(Kept).endsWith("edited elsewhere\n"));
    QFile::remove(Kept);
  }
  CHECK(Wait([]() { return RemoteSession::Closing() == 0; }));
  MobileCache::Clean(true);
  CHECK(FilesBelow(MobileCache::Root()).isEmpty());
  EmptyFolder(Root);
  Configuration::Instance().StoredSessions().Remove(Site);
}

// A session whose editor outlives it: the file again over a new connection
static void TestAdoptEdit()
{
  if ((FtpPort() == 0) || FtpRoot().isEmpty())
  {
    return;
  }
  QString Root = FtpRoot() + QStringLiteral("/mobile-adopt");
  QDir().mkpath(Root);
  WriteFile(Root + QStringLiteral("/a.txt"), "one\r\ntwo\r\n");
  QString Site = AddSite(QStringLiteral("Adopt"), fsFTP, FtpPort(), QStringLiteral("test"), QStringLiteral("secret"), QStringLiteral("/mobile-adopt"));
  App A;
  CHECK(A.Open(Site));
  QVariantMap File;
  QString Local = BeginEdit(A, QStringLiteral("a.txt"), &File);
  QSignalSpy Ended(&A.Sessions, &SessionManager::sessionEnded);
  A.Sessions.closeCurrent();
  CHECK(Wait([&]() { return !Ended.isEmpty(); }));
  // EditorPage: the text is still there; a new tab takes the file again
  CHECK(A.Open(Site));
  QSignalSpy Adopted(A.Current(), &RemoteSession::editAdopted);
  A.Current()->adoptEdit(QStringLiteral("/mobile-adopt/a.txt"));
  CHECK(Wait([&]() { return !Adopted.isEmpty(); }));
  QString NewLocal = Adopted.first().at(0).toString();
  CHECK(Adopted.first().at(1).toString() == QStringLiteral("/mobile-adopt/a.txt"));
  A.Current()->readEdit(NewLocal, QLatin1String(TextFile::Utf8));
  CHECK(SaveEdit(A, NewLocal, QStringLiteral("one\nTWO\n"), QLatin1String(TextFile::Utf8)));
  CHECK(ReadFile(Root + QStringLiteral("/a.txt")) == "one\r\nTWO\r\n");
  A.Sessions.closeCurrent();
  CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
  EmptyFolder(Root);
  Configuration::Instance().StoredSessions().Remove(Site);
}

// M8, M9, M12, M13, L8, L9: the queue
static void TestQueue()
{
  if ((FtpPort() == 0) || FtpRoot().isEmpty())
  {
    return;
  }
  QString Root = FtpRoot() + QStringLiteral("/mobile-queue");
  EmptyFolder(Root);
  QDir().mkpath(Root);
  QTemporaryDir Local;
  QString Site = AddSite(QStringLiteral("Queue"), fsFTP, FtpPort(), QStringLiteral("test"), QStringLiteral("secret"), QStringLiteral("/mobile-queue"));
  App A;
  CHECK(A.Open(Site));
  RemoteSession * Session = A.Current();
  auto Finished = [&A]() { return A.Queue.active() == 0; };
  auto StateOf = [&A](int Row) { return A.Queue.data(A.Queue.index(Row), MobileQueue::StateRole).toString(); };

  // M8: a partly transferred file, resumed: Done, not Failed
  QByteArray Content(300000, 'z');
  WriteFile(Root + QStringLiteral("/big.bin"), Content);
  WriteFile(Local.path() + QStringLiteral("/big.bin.filepart"), Content.left(100000));
  Session->refresh();
  CHECK(Wait([&]() { return !Session->busy(); }));
  Session->download({QStringLiteral("big.bin")}, Local.path());
  CHECK(Wait(Finished));
  CHECK(StateOf(0) == QStringLiteral("done"));
  CHECK(ReadFile(Local.path() + QStringLiteral("/big.bin")) == Content);
  A.Queue.clearFinished();

  // M9: an upload that failed can be retried: its copy of the document is still there
  QString Document = Local.path() + QStringLiteral("/document.txt");
  WriteFile(Document, "the document");
  QString Copies = MobileCache::Folder(QStringLiteral("upload"), 999);
  WriteFile(Copies + QStringLiteral("/document.txt"), "the document");
  QString Denied = Root + QStringLiteral("/denied");
  QDir().mkpath(Denied);
  ::chmod(QFile::encodeName(Denied).constData(), 0555);
  A.Queue.Add(Session, MobileQueue::Upload, {Copies + QStringLiteral("/document.txt")}, QStringLiteral("/mobile-queue/denied"), false, Copies);
  CHECK(Wait(Finished));
  if (getuid() != 0)
  {
    CHECK(StateOf(0) == QStringLiteral("failed"));
    CHECK(A.Queue.data(A.Queue.index(0), MobileQueue::CanRetryRole).toBool());
    CHECK(QFile::exists(Copies + QStringLiteral("/document.txt")));
    ::chmod(QFile::encodeName(Denied).constData(), 0755);
    A.Queue.retry(A.Queue.data(A.Queue.index(0), MobileQueue::IdRole).toInt());
    CHECK(Wait(Finished));
    CHECK(StateOf(0) == QStringLiteral("done"));
    CHECK(ReadFile(Denied + QStringLiteral("/document.txt")) == "the document");
    // the copy goes once it arrived
    CHECK(!QFile::exists(Copies));
  }
  ::chmod(QFile::encodeName(Denied).constData(), 0755);
  A.Errors.clear();
  A.Queue.clearFinished();

  // M10: two documents of one name are two files
  QString SameA = Local.path() + QStringLiteral("/a/same.txt");
  WriteFile(SameA, "first");
  // (the names of content:// documents need Android; here the copy of plain files is checked)
  Session->upload({SameA});
  CHECK(Wait([&]() { return !Session->busy() && Finished(); }));
  CHECK(ReadFile(Root + QStringLiteral("/same.txt")) == "first");

  // L9: the listing shows what the queue uploaded
  CHECK(Wait([&]() { return !Session->busy() && !Session->files()->entry(QStringLiteral("same.txt")).isEmpty(); }));
  A.Queue.clearFinished();

  // a cancelled upload is offered again as long as the tab lives; L8: not after it closed
  WriteFile(Copies + QStringLiteral("/left.txt"), "x");
  int Id = A.Queue.Add(Session, MobileQueue::Upload, {Copies + QStringLiteral("/left.txt")}, QStringLiteral("/no/such/folder"), false, Copies);
  CHECK(Wait(Finished));
  A.Errors.clear();
  CHECK(A.Queue.data(A.Queue.index(0), MobileQueue::CanRetryRole).toBool());
  A.Sessions.closeCurrent();
  CHECK(Wait([&]() { return A.Sessions.count() == 0; }));
  CHECK(!A.Queue.data(A.Queue.index(0), MobileQueue::CanRetryRole).toBool());
  A.Queue.retry(Id);
  CHECK(StateOf(0) != QStringLiteral("waiting"));
  CHECK(!QFile::exists(Copies));
  A.Queue.clearFinished();
  CHECK(Wait([]() { return RemoteSession::Closing() == 0; }));
  EmptyFolder(Root);
  Configuration::Instance().StoredSessions().Remove(Site);
}

// M12, M13, M3, M5: connections that break or never answer
static void TestLostConnections()
{
  if (FtpPort() == 0)
  {
    return;
  }
  QString Root = FtpRoot() + QStringLiteral("/mobile-lost");
  EmptyFolder(Root);
  QDir().mkpath(Root);
  for (int I = 0; I < 4; I++)
  {
    WriteFile(Root + QStringLiteral("/file%1.bin").arg(I), QByteArray(30000000, char('a' + I)));
  }
  QTemporaryDir Local;
  Proxy Front(static_cast<quint16>(FtpPort()));
  QString Site = AddSite(QStringLiteral("Lost"), fsFTP, Front.serverPort(), QStringLiteral("test"), QStringLiteral("secret"), QStringLiteral("/mobile-lost"));
  {
    // M12: a fatal end of the transfer connection (the server is gone and the
    // reopen time is over): the item fails, and the ones behind it must end
    // too, not wait for a new connection that a late DropBack took away
    Configuration & Config = Configuration::Instance();
    int ReopenTimeout = Config.SessionReopenTimeout;
    Config.SessionReopenTimeout = 1;
    App A;
    CHECK(A.Open(Site));
    RemoteSession * Session = A.Current();
    Session->download({QStringLiteral("file0.bin")}, Local.path());
    Session->download({QStringLiteral("file1.bin")}, Local.path());
    Session->download({QStringLiteral("file2.bin")}, Local.path());
    CHECK(Wait([&]() { return A.Queue.active() < 3 || (A.Queue.progress() >= 0); }));
    quint16 Port = Front.serverPort();
    Front.close();
    Front.Cut();
    CHECK(Wait([&]() { return (A.Queue.active() == 0) || (A.Sessions.count() == 0); }, 30000));
    CHECK(A.Queue.active() == 0);
    A.Queue.clearFinished();
    Config.SessionReopenTimeout = ReopenTimeout;
    Front.listen(QHostAddress::LocalHost, Port);
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
  }
  {
    // M13: a tab that is not shown loses its connection: it closes, its queue ends
    App A;
    CHECK(A.Open(Site));
    RemoteSession * Background = A.Current();
    QString Other = AddSite(QStringLiteral("Other"), fsFTP, FtpPort(), QStringLiteral("test"), QStringLiteral("secret"));
    CHECK(A.Open(Other));
    CHECK(A.Sessions.count() == 2);
    CHECK(A.Current() != Background);
    // the server is gone for good
    quint16 Port = Front.serverPort();
    Front.close();
    Front.Cut();
    A.Refuse = true;
    A.Queue.Add(Background, MobileQueue::Download, {QStringLiteral("/mobile-lost/file3.bin")}, Local.path(), false);
    Background->refresh();
    CHECK(Wait([&]() { return (A.Sessions.count() == 1) && (A.Queue.active() == 0); }, 30000));
    CHECK(A.Current() != Background);
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
    Configuration::Instance().StoredSessions().Remove(Other);
    Front.listen(QHostAddress::LocalHost, Port);
  }
  {
    // M3: closing a tab in the middle of a transfer: the callbacks of its
    // connections run into a session that is still there
    App A;
    CHECK(A.Open(Site));
    A.Current()->download({QStringLiteral("file0.bin"), QStringLiteral("file1.bin")}, Local.path());
    QPointer<RemoteSession> Closed(A.Current());
    A.Sessions.closeCurrent();
    CHECK(Wait([&]() { return Closed == nullptr; }));
    CHECK(RemoteSession::Closing() == 0);
    CHECK(A.Queue.active() == 0);
  }
  {
    // M5: Cancel while connecting to a server that says nothing: the app goes on at once
    Front.Silent = true;
    App A;
    A.Settings.setTimeout(5);
    A.Sessions.open(Site);
    CHECK(Wait([&]() { return Front.Accepted > 0; }));
    Wait([]() { return false; }, 200);
    QElapsedTimer Timer;
    Timer.start();
    qint64 Longest = 0;
    qint64 Last = 0;
    QTimer Tick;
    Tick.setInterval(10);
    QObject::connect(&Tick, &QTimer::timeout, &Tick, [&]()
    {
      Longest = std::max(Longest, Timer.elapsed() - Last);
      Last = Timer.elapsed();
    });
    Tick.start();
    A.Current()->cancel();
    CHECK(Timer.elapsed() < 500);
    CHECK(Wait([&]() { return A.Sessions.count() == 0; }, 2000));
    // the worker ends by itself, the event loop never stands still for it
    CHECK(Wait([&]() { return RemoteSession::Closing() == 0; }, 12000));
    CHECK(Longest < 1000);
    Front.Silent = false;
  }
  Configuration::Instance().StoredSessions().Remove(Site);
  EmptyFolder(Root);
}

//---------------------------------------------------------------------------
// S16: an encrypted key logs in without a decrypted file; known_hosts hashed and still checked
static void TestSshSession()
{
  QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if ((SshPort() == 0) || Key.isEmpty())
  {
    std::printf("SKIP ssh sessions (NIXSCP_TEST_SSH_PORT)\n");
    return;
  }
  QString User = qEnvironmentVariable("USER");
  QString KnownHosts = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.ssh/known_hosts");
  QFile::remove(KnownHosts);
  QString Site = AddSite(QStringLiteral("Ssh"), fsSFTP, SshPort(), User, QString());
  QDir().mkpath(KeyStore::Folder());
  TurnEncryptionOn();
  QString Stored = KeyStore::Folder() + QStringLiteral("/test_key");
  WriteFile(Stored, ConfigVault::Seal(ReadFile(Key)));
  {
    App A;
    A.Settings.setSiteKey(Site, QStringLiteral("test_key"));
    QSignalSpy Established(&A.Sessions, &SessionManager::established);
    CHECK(A.Open(Site));
    CHECK(!Established.isEmpty());
    CHECK(A.HostKeys == 1);
    CHECK(A.Current()->canRunCommands());
    // no decrypted key anywhere in the app's folders
    QByteArray Plain = ReadFile(Key);
    for (const QString & File : FilesBelow(MobileCache::Root()) + FilesBelow(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)))
    {
      CHECK(ReadFile(File) != Plain);
      CHECK(!ReadFile(File).contains("PRIVATE KEY"));
    }
    // known_hosts names no host
    QByteArray Hosts = ReadFile(KnownHosts);
    CHECK(Hosts.startsWith("|1|"));
    CHECK(!Hosts.contains("127.0.0.1"));

    // the background connection logs in with it as well, and needs no question: the hashed entry is found
    QSignalSpy Unknown(&A.UI, &MobileUserInterface::hostKeyRequested);
    QTemporaryDir Local;
    QSignalSpy Copied(&A.Queue, &MobileQueue::copied);
    WriteFile(Local.path() + QStringLiteral("/up.txt"), "upload over sftp");
    A.Current()->upload({Local.path() + QStringLiteral("/up.txt")});
    CHECK(Wait([&]() { return (A.Queue.finished() == 1) && (A.Queue.active() == 0); }));
    CHECK(A.Queue.data(A.Queue.index(0), MobileQueue::StateRole).toString() == QStringLiteral("done"));
    CHECK(Unknown.isEmpty());
    A.Current()->remove({QStringLiteral("up.txt")});
    CHECK(Wait([&]() { return !A.Current()->busy(); }));
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
  }
  {
    // a reconnection finds the hashed key: no question; a changed key is still "changed", not "unknown"
    App A;
    A.Settings.setSiteKey(Site, QStringLiteral("test_key"));
    CHECK(A.Open(Site));
    CHECK(A.HostKeys == 0);
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));

    QByteArray Hosts = ReadFile(KnownHosts);
    QList<QByteArray> Lines = Hosts.split('\n');
    for (QByteArray & Line : Lines)
    {
      QList<QByteArray> Fields = Line.split(' ');
      if (Fields.size() >= 3)
      {
        // another key of the same type
        ssh_key Other = nullptr;
        ssh_pki_generate(ssh_key_type_from_name(Fields.at(1).constData()), 0, &Other);
        if (Other == nullptr)
        {
          ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &Other);
          Fields[1] = "ssh-ed25519";
        }
        char * Base64 = nullptr;
        ssh_pki_export_pubkey_base64(Other, &Base64);
        Fields[2] = Base64;
        ssh_string_free_char(Base64);
        ssh_key_free(Other);
        Line = Fields.join(' ');
      }
    }
    WriteFile(KnownHosts, Lines.join('\n'));
    App B;
    B.Settings.setSiteKey(Site, QStringLiteral("test_key"));
    B.Open(Site);
    CHECK(B.ChangedKeys >= 1);
    CHECK(B.Sessions.count() == 0);
    CHECK(Wait([]() { return RemoteSession::Closing() == 0; }));
  }
  QFile::remove(KnownHosts);
  QFile::remove(Stored);
  ConfigVault::Forget();

  // M-H5: only what changed, on each file's own rights
  {
    App A;
    A.Settings.setSiteKey(Site, QString());
    QTemporaryDir Scratch;
    WriteFile(KeyStore::Folder() + QStringLiteral("/plain_key"), ReadFile(Key));
    QFile::setPermissions(KeyStore::Folder() + QStringLiteral("/plain_key"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    A.Settings.setSiteKey(Site, QStringLiteral("plain_key"));
    CHECK(A.Open(Site));
    RemoteSession * Session = A.Current();
    QString Folder = QStringLiteral("mobile-rights-%1").arg(getpid());
    Session->createFolder(Folder);
    CHECK(Wait([&]() { return !Session->busy(); }));
    Session->enter(Folder);
    CHECK(Wait([&]() { return !Session->busy() && Session->directory().endsWith(Folder); }));
    Session->createFile(QStringLiteral("a.txt"));
    QSignalSpy Ready(&A.Sessions, &SessionManager::editReady);
    CHECK(Wait([&]() { return !Ready.isEmpty(); }));
    Session->closeEdit(Ready.first().at(1).toString());
    Session->createFile(QStringLiteral("b.txt"));
    CHECK(Wait([&]() { return Ready.size() == 2; }));
    Session->closeEdit(Ready.at(1).at(1).toString());
    CHECK(Wait([&]() { return !Session->busy(); }));
    Session->setProperties({QStringLiteral("a.txt")}, 0640, 0137, QString(), QString(), false);
    CHECK(Wait([&]() { return !Session->busy(); }));
    Session->setProperties({QStringLiteral("b.txt")}, 0604, 0173, QString(), QString(), false);
    CHECK(Wait([&]() { return !Session->busy(); }));
    // both: owner execute on; the rest of each file's rights stays
    Session->setProperties({QStringLiteral("a.txt"), QStringLiteral("b.txt")}, 0100, 0, QString(), QString(), false);
    CHECK(Wait([&]() { return !Session->busy(); }));
    Wait([&]() { return Session->files()->entry(QStringLiteral("a.txt")).value(QStringLiteral("mode")).toUInt() == 0740; }, 3000);
    CHECK(Session->files()->entry(QStringLiteral("a.txt")).value(QStringLiteral("mode")).toUInt() == 0740);
    CHECK(Session->files()->entry(QStringLiteral("b.txt")).value(QStringLiteral("mode")).toUInt() == 0704);
    // nothing changed: nothing is sent
    Session->setProperties({QStringLiteral("a.txt"), QStringLiteral("b.txt")}, 0, 0, QString(), QString(), false);
    CHECK(!Session->busy());
    Session->remove({QStringLiteral("a.txt"), QStringLiteral("b.txt")});
    CHECK(Wait([&]() { return !Session->busy(); }));
    Session->enter(QStringLiteral(".."));
    CHECK(Wait([&]() { return !Session->busy(); }));
    Session->remove({Folder});
    CHECK(Wait([&]() { return !Session->busy(); }));
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
    QFile::remove(KeyStore::Folder() + QStringLiteral("/plain_key"));
  }
  QFile::remove(KnownHosts);
  Configuration::Instance().StoredSessions().Remove(Site);
}

// L5, L4, backpressure: the shell against the OpenSSH test server, through the proxy
static void TestShell()
{
  QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if ((SshPort() == 0) || Key.isEmpty())
  {
    return;
  }
  MobileUserInterface UI;
  QObject::connect(&UI, &MobileUserInterface::hostKeyRequested, &UI, [&UI]() { UI.answerHostKey(1); });
  Proxy Front(static_cast<quint16>(SshPort()));
  SessionData Data;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Front.serverPort();
  Data.UserName = qEnvironmentVariable("USER");
  Data.PublicKeyFile = Key;
  Data.FSProtocol = fsSFTP;
  Data.TryAgent = false;
  Data.Timeout = 10;
  Data.PingType = ptNullPacket;
  Data.PingInterval = 1;
  Data.KnownHostsFile = QDir::tempPath() + QStringLiteral("/nixscp-mobile-shell-hosts");
  auto Screen = [](TerminalSession & Shell)
  {
    QString Text;
    for (int Row = 0; Row < Shell.Rows(); Row++)
    {
      for (int Col = 0; Col < Shell.Cols(); Col++)
      {
        VTermScreenCell Cell{};
        vterm_screen_get_cell(Shell.Screen(), VTermPos{Row, Col}, &Cell);
        if (Cell.chars[0] != 0)
        {
          Text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&Cell.chars[0]), 1);
        }
      }
      Text += QLatin1Char('\n');
    }
    return Text;
  };
  {
    TerminalSession Shell(Data, &UI);
    QSignalSpy Finished(&Shell, &TerminalSession::finished);
    Shell.Start(80, 24);
    CHECK(Wait([&]() { return Shell.running(); }));
    Shell.sendText(QStringLiteral("echo mark$((40+2))\n"));
    CHECK(Wait([&]() { return Screen(Shell).contains(QStringLiteral("mark42")); }));

    // L5: idle, and the keepalive still goes out
    qint64 Before = Front.ClientBytes;
    Wait([]() { return false; }, 2500);
    CHECK(Front.ClientBytes > Before);

    Shell.sendText(QStringLiteral("exit\n"));
    CHECK(Wait([&]() { return !Finished.isEmpty(); }));
    CHECK(!Shell.running());

    // L4: what is typed into the ended shell does not run in the next one
    Shell.sendText(QStringLiteral("echo typed-while-ended\n"));
    Shell.Start(80, 24);
    CHECK(Wait([&]() { return Shell.running(); }));
    Shell.sendText(QStringLiteral("echo next$((40+3))\n"));
    CHECK(Wait([&]() { return Screen(Shell).contains(QStringLiteral("next43")); }));
    CHECK(!Screen(Shell).contains(QStringLiteral("typed-while-ended")));
    Shell.sendText(QStringLiteral("exit\n"));
    CHECK(Wait([&]() { return !Shell.running(); }));
  }
  {
    // backpressure: a flood does not pile up while the screen is behind
    // (straight to the server: the proxy lives on the thread held back here)
    Data.PortNumber = SshPort();
    TerminalSession Shell(Data, &UI);
    Shell.Start(80, 24);
    CHECK(Wait([&]() { return Shell.running(); }));
    Shell.sendText(QStringLiteral("head -c 30000000 /dev/zero | tr '\\0' 'x'; echo; echo flood-done\n"));
    Wait([&]() { return Shell.Unprocessed() > 0; }, 5000);
    // the GUI thread is busy for a moment, as with a heavy page
    qint64 Highest = 0;
    QElapsedTimer Timer;
    Timer.start();
    while (!Timer.hasExpired(1500))
    {
      Highest = std::max(Highest, Shell.Unprocessed());
      QThread::msleep(5);
    }
    CHECK(Highest >= TerminalSession::MaxUnprocessed);   // it did fall behind
    CHECK(Highest <= TerminalSession::MaxUnprocessed + 2 * 16384);
    CHECK(Wait([&]() { return Screen(Shell).contains(QStringLiteral("flood-done")); }, 60000));
    Shell.sendText(QStringLiteral("exit\n"));
    CHECK(Wait([&]() { return !Shell.running(); }));
  }
  QFile::remove(Data.KnownHostsFile);
}

// M4: the offset of the scrollback stays inside it
static void TestScrollOffset()
{
  CHECK(TerminalSession::ValidOffset(5, 10) == 5);
  CHECK(TerminalSession::ValidOffset(15, 10) == 10);
  CHECK(TerminalSession::ValidOffset(15, 0) == 0);
  CHECK(TerminalSession::ValidOffset(-1, 10) == 0);
}

// M-H1, M5: a server that accepts the session and refuses the terminal; one
// that asks about its host key while the tab is closed
static void ServeOneConnection(ssh_bind Bind, std::atomic<bool> * Stop)
{
  ssh_session Session = ssh_new();
  if (ssh_bind_accept(Bind, Session) != SSH_OK)
  {
    ssh_free(Session);
    return;
  }
  ssh_set_blocking(Session, 1);
  if (ssh_handle_key_exchange(Session) != SSH_OK)
  {
    ssh_free(Session);
    return;
  }
  // the public key too: M16 needs the client to go for the key file first
  ssh_set_auth_methods(Session, SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_PUBLICKEY);
  while (!*Stop)
  {
    ssh_message Message = ssh_message_get(Session);
    if (Message == nullptr)
    {
      break;
    }
    int Type = ssh_message_type(Message);
    int Subtype = ssh_message_subtype(Message);
    if ((Type == SSH_REQUEST_AUTH) && (Subtype == SSH_AUTH_METHOD_PASSWORD))
    {
      ssh_message_auth_reply_success(Message, 0);
    }
    else if ((Type == SSH_REQUEST_CHANNEL_OPEN) && (Subtype == SSH_CHANNEL_SESSION))
    {
      ssh_message_channel_request_open_reply_accept(Message);
    }
    else
    {
      // the terminal, and everything else: refused
      ssh_message_reply_default(Message);
    }
    ssh_message_free(Message);
  }
  ssh_disconnect(Session);
  ssh_free(Session);
}

struct MiniServer
{
  ssh_bind Bind = ssh_bind_new();
  int Port = 0;
  std::atomic<bool> Stop{false};
  std::thread Thread;

  explicit MiniServer(const QString & Folder)
  {
    QString HostKey = Folder + QStringLiteral("/host_key");
    WriteFile(HostKey, GenerateKeyFile(SSH_KEYTYPE_ED25519));
    QByteArray KeyPath = QFile::encodeName(HostKey);
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1");
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_HOSTKEY, KeyPath.constData());
    // a free port
    {
      QTcpServer Probe;
      Probe.listen(QHostAddress::LocalHost);
      Port = Probe.serverPort();
    }
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_BINDPORT, &Port);
    CHECK(ssh_bind_listen(Bind) == SSH_OK);
    Thread = std::thread([this]() { ServeOneConnection(Bind, &Stop); });
  }
  ~MiniServer()
  {
    Stop = true;
    Thread.join();
    ssh_bind_free(Bind);
  }
};

// M16: the key of the site was deleted: a password is asked for, no fatal error
static void TestDeletedKey(const QString & Folder)
{
  MiniServer Server(Folder);
  QString Site = AddSite(QStringLiteral("Deleted key"), fsSFTP, Server.Port, QStringLiteral("u"), QString());
  QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.ssh/known_hosts"));
  {
    App A;
    A.Settings.setSiteKey(Site, QStringLiteral("deleted_key"));
    int Passwords = 0;
    QObject::connect(&A.UI, &MobileUserInterface::promptRequested, &A.UI, [&](int, const QString &, const QString &, const QVariantList &)
    {
      Passwords++;
      A.UI.answerPrompt(false, {});
    });
    A.Open(Site);
    CHECK(Passwords == 1);
    CHECK(!A.Errors.join(QLatin1Char(' ')).contains(QStringLiteral("key file")));
    A.Settings.setSiteKey(Site, QString());
    A.Sessions.CloseAll();
    CHECK(Wait([&]() { return (A.Sessions.count() == 0) && (RemoteSession::Closing() == 0); }));
  }
  Configuration::Instance().StoredSessions().Remove(Site);
}

static void TestRefusedTerminal(const QString & Folder)
{
  auto * Mini = new MiniServer(Folder);
  int Port = Mini->Port;

  MobileUserInterface UI;
  QObject::connect(&UI, &MobileUserInterface::hostKeyRequested, &UI, [&UI]() { UI.answerHostKey(1); });
  SessionData Data;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port;
  Data.UserName = QStringLiteral("u");
  Data.Password = QStringLiteral("p");
  Data.FSProtocol = fsSFTP;
  Data.TryAgent = false;
  Data.Timeout = 5;
  Data.KnownHostsFile = Folder + QStringLiteral("/known_hosts");
  {
    TerminalSession Shell(Data, &UI);
    QSignalSpy Finished(&Shell, &TerminalSession::finished);
    Shell.Start(80, 24);
    CHECK(Wait([&]() { return !Finished.isEmpty(); }));
    // a message and no crash: the channel went before its session
    CHECK(Finished.first().at(0).toString().contains(QStringLiteral("refused")));
    CHECK(!Shell.running());
  }
  delete Mini;

  // M5: a shell connecting while its tab closes: Dispose does not wait, and it goes away
  QTcpServer Silent;
  Silent.listen(QHostAddress::LocalHost);
  Data.PortNumber = Silent.serverPort();
  Data.Timeout = 3;
  auto * Shell = new TerminalSession(Data, &UI);
  QPointer<TerminalSession> Guard(Shell);
  Shell->Start(80, 24);
  Wait([&]() { return Silent.hasPendingConnections(); }, 2000);
  QElapsedTimer Timer;
  Timer.start();
  Shell->Dispose();
  CHECK(Timer.elapsed() < 200);
  CHECK(TerminalSession::Ending() == 1);
  CHECK(Wait([&]() { return Guard == nullptr; }, 10000));
  CHECK(TerminalSession::Ending() == 0);
}

// a key opened in memory: the same key as from the file
static void TestPuttyData()
{
  QString Sample = QStringLiteral(TEST_DATA_DIR "/ppk/ed25519-v3.ppk");
  if (!QFile::exists(Sample))
  {
    return;
  }
  ssh_key FromFile = nullptr;
  ssh_key FromData = nullptr;
  auto NoPassphrase = [](QString &, bool) { return false; };
  CHECK(PuttyKey::Load(Sample, NoPassphrase, &FromFile, nullptr) == PuttyKey::lrOk);
  CHECK(PuttyKey::LoadData(ReadFile(Sample), NoPassphrase, &FromData, nullptr) == PuttyKey::lrOk);
  CHECK(ssh_key_cmp(FromFile, FromData, SSH_KEY_CMP_PRIVATE) == 0);
  ssh_key_free(FromFile);
  ssh_key_free(FromData);
}

int main(int Argc, char ** Argv)
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("TZ", "Europe/Belgrade");
  tzset();
  QTemporaryDir Home;
  CHECK(Home.isValid());
  qputenv("HOME", Home.path().toLocal8Bit());
  qputenv("XDG_DATA_HOME", (Home.path() + QStringLiteral("/data")).toLocal8Bit());
  qputenv("XDG_CACHE_HOME", (Home.path() + QStringLiteral("/cache")).toLocal8Bit());
  qputenv("XDG_CONFIG_HOME", (Home.path() + QStringLiteral("/config")).toLocal8Bit());
  QGuiApplication App(Argc, Argv);
  QGuiApplication::setApplicationName(QStringLiteral("NixSCP"));
  QGuiApplication::setOrganizationName(QStringLiteral("NixSCP"));
  ssh_init();
  QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
  QDir().mkpath(MobileCache::Root());

  Configuration::Instance().SetIniFileName(Home.path() + QStringLiteral("/nixscp.ini"));
  Configuration::Instance().Load();
  Configuration::Instance().Save();
  // as the app (main.cpp): TeamSync decides whose a site's note is
  CloudSync::Instance().Initialize(Configuration::Instance());
  TeamSync::Instance().Initialize(Configuration::Instance(), CloudSync::Instance());

  TestTextFile();
  TestCopy();
  TestSortingWithoutCollator();
  TestFileList();
  TestQuestions();
  TestPrivateState(Home.path());
  TestAppLockProblems(Home.path());
  TestCache();
  TestKeyStore();
  TestHashKnownHosts();
  TestInstallKeyCommand();
  TestFilesToPublish();
  TestKeepAlive();
  TestLocalBrowser();
  TestSites();
  TestSiteNotes();
  TestNoteItems();
  TestPhoneSettings();
  TestDeletedSite();
  TestSiteChecker();
  TestScrollOffset();
  TestPuttyData();
  TestRefusedTerminal(Home.path());
  TestDeletedKey(Home.path());
  TestEditing();
  TestAdoptEdit();
  TestQueue();
  TestLostConnections();
  TestSshSession();
  TestShell();
  TestTeamNotes(Home.path());

  ssh_finalize();
  std::printf("%s (%d failures)\n", (Failures == 0) ? "OK" : "FAILED", Failures);
  return (Failures == 0) ? 0 : 1;
}
