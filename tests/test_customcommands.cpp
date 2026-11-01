// Custom command tests (core/CustomCommands.cpp): pattern expansion as
// TCustomCommand/TInteractiveCustomCommand/TFileCustomCommand/TLocalCustomCommand,
// examples from https://winscp.net/eng/docs/custom_command, and the WinSCP.ini
// format of TCustomCommandList
#include "core/Configuration.h"
#include "core/CustomCommands.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

#define CHECK_EQ(Actual, Expected) \
  do { QString A_ = (Actual); QString E_ = (Expected); if (A_ != E_) { \
    std::printf("FAIL %s:%d: %s\n  got:      [%s]\n  expected: [%s]\n", __FILE__, __LINE__, #Actual, \
      qPrintable(A_), qPrintable(E_)); Failures++; } } while (0)

static CustomCommandData Session()
{
  SessionData Data;
  Data.Name = QStringLiteral("My server");
  Data.HostName = QStringLiteral("example.com");
  Data.UserName = QStringLiteral("martin");
  Data.Password = QStringLiteral("pa$$!word");
  Data.PortNumber = 2222;
  Data.PublicKeyFile = QStringLiteral("/home/martin/.ssh/id_ed25519.ppk");
  return CustomCommandData::FromSession(Data);
}

// The way WinSCP runs a remote command on one file: interactive pass
// (prompts, local commands), then the file pass
static QString Remote(const QString & Command, const QString & FileName, const QString & FileList = QString(),
  InteractiveCustomCommand::PromptsCallback Prompts = InteractiveCustomCommand::PromptsCallback())
{
  CustomCommandData Data = Session();
  RemoteCustomCommand RemoteCommand(Data, QStringLiteral("/home/martin"));
  InteractiveCustomCommand Interactive(&RemoteCommand, Prompts);
  QString Cmd = Interactive.Complete(Command, false);
  return RemoteCustomCommand(Data, QStringLiteral("/home/martin"), FileName, FileList).Complete(Cmd, true);
}

static QString Local(const QString & Command, const QString & FileName, const QString & LocalFileName = QString(),
  const QString & FileList = QString())
{
  CustomCommandData Data = Session();
  LocalCustomCommand LocalCommand(Data, QStringLiteral("/var/www"), QStringLiteral("/home/me/"));
  InteractiveCustomCommand Interactive(&LocalCommand);
  QString Cmd = Interactive.Complete(Command, false);
  return LocalCustomCommand(Data, QStringLiteral("/var/www"), QStringLiteral("/home/me/"), FileName, LocalFileName, FileList)
    .Complete(Cmd, true);
}

static QString Error(std::function<void()> Function)
{
  try
  {
    Function();
  }
  catch (const ExtException & E)
  {
    return E.Message();
  }
  return QString();
}

static void TestRemotePatterns()
{
  // file name, escaped for the shell depending on the surrounding quotes
  CHECK_EQ(Remote(QStringLiteral("touch \"!\""), QStringLiteral("file.txt")), QStringLiteral("touch \"file.txt\""));
  CHECK_EQ(Remote(QStringLiteral("rm \"!\""), QStringLiteral("a \"b\" $c `d` \\e")),
    QStringLiteral("rm \"a \\\"b\\\" \\$c \\`d\\` \\\\e\""));
  CHECK_EQ(Remote(QStringLiteral("rm !"), QStringLiteral("a \"b\" $c")), QStringLiteral("rm a \"b\" \\$c"));
  CHECK_EQ(Remote(QStringLiteral("rm '!'"), QStringLiteral("a $c\\")), QStringLiteral("rm 'a $c\\'"));
  CHECK_EQ(Remote(QStringLiteral("rm \"!\""), QStringLiteral("-rf")), QStringLiteral("rm \"./-rf\""));
  // the "Execute" default command
  CHECK_EQ(Remote(QStringLiteral("\"./!\""), QStringLiteral("script.sh")), QStringLiteral("\"./script.sh\""));
  // !! and a file name with ! (escaped in the first pass)
  CHECK_EQ(Remote(QStringLiteral("echo !! \"!\""), QStringLiteral("wow!.txt")), QStringLiteral("echo ! \"wow!.txt\""));
  // trailing ! is the file name
  CHECK_EQ(Remote(QStringLiteral("ls -l !"), QStringLiteral("x")), QStringLiteral("ls -l x"));
  // current path
  CHECK_EQ(Remote(QStringLiteral("cd \"!/\" && ls"), QString()), QStringLiteral("cd \"/home/martin/\" && ls"));
  CHECK_EQ(RemoteCustomCommand(CustomCommandData(), QStringLiteral("/")).Complete(QStringLiteral("!/"), true), QStringLiteral("/"));
  // session patterns
  CHECK_EQ(Remote(QStringLiteral("!@ !U !# !N !K"), QString()),
    QStringLiteral("example.com martin 2222 My server /home/martin/.ssh/id_ed25519.ppk"));
  CHECK_EQ(Remote(QStringLiteral("echo !P"), QString()), QStringLiteral("echo pa\\$\\$!word"));
  CHECK_EQ(Remote(QStringLiteral("echo '!P'"), QString()), QStringLiteral("echo 'pa$$!word'"));
  CHECK_EQ(Remote(QStringLiteral("!u !n !k"), QString()),
    QStringLiteral("martin My server /home/martin/.ssh/id_ed25519.ppk"));
  CHECK_EQ(Remote(QStringLiteral("putty '!S'"), QString()),
    QStringLiteral("putty 'sftp://martin:pa%24%24%21word@example.com:2222/'"));
  // file list (already quoted)
  QString List = MakeShellFileList({QStringLiteral("a b"), QStringLiteral("c\"d"), QStringLiteral("-x")});
  CHECK_EQ(List, QStringLiteral("\"a b\" \"c\\\"d\" \"./-x\""));
  CHECK_EQ(Remote(QStringLiteral("grep foo !&"), QString(), List), QStringLiteral("grep foo \"a b\" \"c\\\"d\" \"./-x\""));
  // unknown pattern letters are a file name followed by text
  CHECK_EQ(Remote(QStringLiteral("cat !x"), QStringLiteral("f")), QStringLiteral("cat fx"));
  // no session: session patterns are empty
  CHECK_EQ(RemoteCustomCommand().Complete(QStringLiteral("[!@][!#][!S]"), true), QStringLiteral("[][][]"));
}

static void TestPrompts()
{
  // documented example: grep "!?Pattern:?!" !&
  QStringList SeenPrompts;
  QStringList SeenDefaults;
  int Calls = 0;
  auto Answer = [&](const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)
  {
    Calls++;
    SeenPrompts = Prompts;
    SeenDefaults = Defaults;
    Values = QStringList{QStringLiteral("a \"b\" $c!"), QStringLiteral("x y")};
    return true;
  };
  CHECK_EQ(Remote(QStringLiteral("grep \"!?Pattern:?!\" !& > \"!?Output:?out.txt!\""), QString(), QStringLiteral("\"f\""), Answer),
    QStringLiteral("grep \"a \\\"b\\\" \\$c!\" \"f\" > \"x y\""));
  CHECK(Calls == 1);
  CHECK(SeenPrompts == (QStringList{QStringLiteral("Pattern:"), QStringLiteral("Output:")}));
  CHECK(SeenDefaults == (QStringList{QString(), QStringLiteral("out.txt")}));

  // defaults without a prompt callback (default commands)
  CHECK_EQ(Remote(QStringLiteral("tar -cz  -f \"!?&Archive Name:?archive.tgz!\" !&"), QString(),
    MakeShellFileList({QStringLiteral("dir"), QStringLiteral("file")})),
    QStringLiteral("tar -cz  -f \"archive.tgz\" \"dir\" \"file\""));
  CHECK_EQ(Remote(QStringLiteral("tar -xz --directory=\"!?&Extract to directory:?.!\" -f \"!\""), QStringLiteral("a.tgz")),
    QStringLiteral("tar -xz --directory=\".\" -f \"a.tgz\""));

  // "\" before the second "?" avoids escaping
  auto Raw = [](const QStringList &, const QStringList &, QStringList & Values)
  {
    Values = QStringList{QStringLiteral("$HOME/*.txt")};
    return true;
  };
  CHECK_EQ(Remote(QStringLiteral("ls !?Mask:\\?*!"), QString(), QString(), Raw), QStringLiteral("ls $HOME/*.txt"));
  CHECK_EQ(Remote(QStringLiteral("ls !?Mask:?*!"), QString(), QString(), Raw), QStringLiteral("ls \\$HOME/*.txt"));

  // prompt without default
  QStringList Prompts2;
  auto Collect = [&](const QStringList & Prompts, const QStringList &, QStringList &)
  {
    Prompts2 = Prompts;
    return true;
  };
  Remote(QStringLiteral("echo !?Value!"), QString(), QString(), Collect);
  CHECK(Prompts2 == QStringList{QStringLiteral("Value")});

  // cancelled prompt aborts
  bool Aborted = false;
  try
  {
    Remote(QStringLiteral("echo !?X?y!"), QString(), QString(),
      [](const QStringList &, const QStringList &, QStringList &) { return false; });
  }
  catch (const AbortException &)
  {
    Aborted = true;
  }
  CHECK(Aborted);

  // local command output
  CHECK_EQ(Remote(QStringLiteral("echo !`printf 'hi there\\n\\n'`"), QString()), QStringLiteral("echo hi there"));
  CustomCommandData Data = Session();
  RemoteCustomCommand RemoteCommand(Data, QStringLiteral("/home/martin"));
  InteractiveCustomCommand Interactive(&RemoteCommand, InteractiveCustomCommand::PromptsCallback(),
    [](const QString & Command) { return QStringLiteral("<%1>").arg(Command); });
  // file patterns inside are expanded first (last pass, no escaping)
  CHECK_EQ(Interactive.Complete(QStringLiteral("x !`date -d !@` y"), false), QStringLiteral("x <date -d example.com> y"));
}

static void TestErrors()
{
  CHECK_EQ(ValidateCustomCommand(QStringLiteral("echo !?abc"), false), QStringLiteral("Unterminated pattern '?' starting at 6."));
  CHECK_EQ(ValidateCustomCommand(QStringLiteral("echo !`abc"), false), QStringLiteral("Unterminated pattern '`' starting at 6."));
  CHECK_EQ(ValidateCustomCommand(QStringLiteral("echo ! !&"), false),
    QStringLiteral("Cannot combine file name pattern (starting at 6) with file list pattern (starting at 8)."));
  CHECK_EQ(ValidateCustomCommand(QStringLiteral("cmp !^"), true), QStringLiteral("Unterminated pattern '^' starting at 5."));
  CHECK(ValidateCustomCommand(QStringLiteral("grep \"!?Pattern:?!\" !&"), false).isEmpty());
  CHECK(ValidateCustomCommand(QStringLiteral("echo !! !&"), false).isEmpty());
  CHECK(ValidateCustomCommand(QStringLiteral("touch \"!\""), false).isEmpty());
  CHECK(ValidateCustomCommand(QStringLiteral("meld \"!\" \"!^!\""), true).isEmpty());
  // without the interactive pass "!?" is just a file name pattern
  CHECK(Error([]() { RemoteCustomCommand().Complete(QStringLiteral("!?x"), true); }).isEmpty());
  CHECK_EQ(RemoteCustomCommand().Complete(QStringLiteral("!?x"), true), QStringLiteral("?x"));
}

static void TestLocalPatterns()
{
  // shell-escaped (run by /bin/sh); !^! and !\ only for local commands
  CHECK_EQ(Local(QStringLiteral("meld \"!\" \"!^!\""), QStringLiteral("/tmp/a $b.txt"), QStringLiteral("/home/me/a b.txt")),
    QStringLiteral("meld \"/tmp/a \\$b.txt\" \"/home/me/a b.txt\""));
  CHECK_EQ(Local(QStringLiteral("cd \"!\\\" && make"), QString()), QStringLiteral("cd \"/home/me\" && make"));
  CHECK_EQ(Local(QStringLiteral("xdg-open \"!/\""), QString()), QStringLiteral("xdg-open \"/var/www/\""));
  CHECK_EQ(Local(QStringLiteral("lp \"!\""), QStringLiteral("-file")), QStringLiteral("lp \"-file\""));
  QString List = MakeLocalFileList({QStringLiteral("/a b/c"), QStringLiteral("/d")});
  CHECK_EQ(List, QStringLiteral("'/a b/c' '/d'"));
  CHECK_EQ(Local(QStringLiteral("zip x.zip !&"), QString(), QString(), List), QStringLiteral("zip x.zip '/a b/c' '/d'"));
  CHECK_EQ(Local(QStringLiteral("putty -ssh !U@!@ -P !# -pw !P"), QString()),
    QStringLiteral("putty -ssh 'martin'@'example.com' -P '2222' -pw 'pa$$!word'"));
  CHECK_EQ(Local(QStringLiteral("cat '!'"), QStringLiteral("x'; rm -rf y; 'z")), QStringLiteral("cat 'x'\\''; rm -rf y; '\\''z'"));
  CHECK_EQ(Local(QStringLiteral("cat !"), QStringLiteral("x$(id)`id`")), QStringLiteral("cat 'x$(id)`id`'"));
}

static void TestAnalysis()
{
  CustomCommandInfo Info = AnalyzeCustomCommand(QStringLiteral("touch \"!\""), false);
  CHECK(Info.Valid && Info.FileCommand && !Info.FileListCommand && Info.RemoteFileCommand && !Info.SessionCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("ls -la"), false);
  CHECK(Info.Valid && !Info.FileCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("echo !!"), false);
  CHECK(!Info.FileCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("grep \"!?Pattern:?!\" !&"), false);
  CHECK(Info.FileCommand && Info.FileListCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("meld \"!\" \"!^!\""), true);
  CHECK(Info.FileCommand && Info.LocalFileName);
  Info = AnalyzeCustomCommand(QStringLiteral("meld \"!^!\""), true);
  CHECK(Info.FileCommand && Info.LocalFileName && !Info.RemoteFileCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("putty !@ -pw !P"), true);
  CHECK(!Info.FileCommand && Info.SessionCommand && Info.PasswordCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("cd !/"), false);
  CHECK(Info.SessionCommand && !Info.FileCommand);
  Info = AnalyzeCustomCommand(QStringLiteral("echo !?x"), false);
  CHECK(!Info.Valid);

  // menu states (TCustomScpExplorerForm::CustomCommandState)
  CustomCommandAvailability A;
  A.RemoteAllowed = true;
  A.SessionAvailable = true;
  CustomCommandType Touch(QStringLiteral("&Touch"), QStringLiteral("touch \"!\""), ccApplyToDirectories | ccRecursive);
  CHECK(CustomCommandState(Touch, A) == 0);
  A.AnySelectedCurrent = true;
  CHECK(CustomCommandState(Touch, A) == 1);
  CHECK(CustomCommandState(Touch, A, ccltNonFile) == -1);
  A.RemoteAllowed = false;
  CHECK(CustomCommandState(Touch, A) == 0);
  A.RemoteAllowed = true;
  A.LocalSideActive = true;
  CHECK(CustomCommandState(Touch, A, ccltFile) == -1);
  CHECK(CustomCommandState(Touch, A, ccltAll) == 1);
  A.LocalSideActive = false;
  CustomCommandType Ls(QStringLiteral("ls"), QStringLiteral("ls -la"), 0);
  CHECK(CustomCommandState(Ls, A, ccltFile) == -1);
  CHECK(CustomCommandState(Ls, A, ccltNonFile) == 1);
  CustomCommandType Diff(QStringLiteral("Diff"), QStringLiteral("meld \"!\" \"!^!\""), ccLocal);
  A.AnySelectedLocal = true;
  A.AnySelectedRemote = false;
  CHECK(CustomCommandState(Diff, A) == 0);
  A.AnySelectedRemote = true;
  CHECK(CustomCommandState(Diff, A) == 1);
  CHECK(CustomCommandState(Diff, A, ccltBoth) == 1);
  CHECK(CustomCommandState(Diff, A, ccltNonFile) == -1);
  CustomCommandType Putty(QStringLiteral("PuTTY"), QStringLiteral("putty !@"), ccLocal);
  CHECK(CustomCommandState(Putty, A) == 1);
  A.SessionAvailable = false;
  CHECK(CustomCommandState(Putty, A) == 0);
}

static void TestNames()
{
  CustomCommandType Command(QStringLiteral("Ta&r/GZip..."), QString(), 0);
  CHECK_EQ(Command.DisplayName(), QStringLiteral("Tar/GZip..."));
  CHECK_EQ(Command.PlainName(), QStringLiteral("Tar/GZip"));
  Command.Name = QStringLiteral("Save && &Quit");
  CHECK_EQ(Command.DisplayName(), QStringLiteral("Save & Quit"));
}

static void TestList(const QString & Dir)
{
  CustomCommandList Defaults = CustomCommandList::Defaults();
  CHECK(Defaults.Count() == 6);
  CHECK(Defaults.At(0) == CustomCommandType(QStringLiteral("&Execute"), QStringLiteral("\"./!\""), 0));
  CHECK(Defaults.At(1) == CustomCommandType(QStringLiteral("&Touch"), QStringLiteral("touch \"!\""), 3));
  CHECK(Defaults.At(2) == CustomCommandType(QStringLiteral("Ta&r/GZip..."), QStringLiteral("tar -cz  -f \"!?&Archive Name:?archive.tgz!\" !&"), 1));
  CHECK(Defaults.At(3) == CustomCommandType(QStringLiteral("&UnTar/GZip..."), QStringLiteral("tar -xz --directory=\"!?&Extract to directory:?.!\" -f \"!\""), 0));
  CHECK(Defaults.At(4) == CustomCommandType(QStringLiteral("&Grep..."), QStringLiteral("grep \"!?&Search for pattern:?!\" !&"), 512));
  CHECK(Defaults.At(5).Name == QStringLiteral("&Print"));
  CHECK(Defaults.At(5).Params == ccLocal);
  CHECK(!Defaults.Modified());

  // a file without custom commands gives the defaults
  IniFile Empty;
  CustomCommandList List;
  bool IsDefault = false;
  List.Load(Empty, &IsDefault);
  CHECK(IsDefault);
  CHECK(List == Defaults);

  // as written by WinSCP
  QString FileName = Dir + QStringLiteral("/commands.ini");
  {
    QFile F(FileName);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(
      "[Configuration\\Interface]\r\n"
      "ShowTips=0\r\n"
      "\r\n"
      "[Configuration\\CustomCommands]\r\n"
      "&Touch=touch%20\"!\"\r\n"
      "My grep=grep%20\"!?Pattern:?!\"%20!&\r\n"
      "\r\n"
      "[Configuration\\CustomCommandsParams]\r\n"
      "&Touch=3\r\n"
      "My grep=1536\r\n"
      "\r\n"
      "[Configuration\\CustomCommandsShortCuts]\r\n"
      "My grep=16455\r\n");
  }
  IniFile File;
  CHECK(File.Load(FileName));
  List.Load(File, &IsDefault);
  CHECK(!IsDefault);
  CHECK(List.Count() == 2);
  CHECK(List.At(0) == CustomCommandType(QStringLiteral("&Touch"), QStringLiteral("touch \"!\""), ccApplyToDirectories | ccRecursive));
  CHECK(List.At(1).Name == QStringLiteral("My grep"));
  CHECK(List.At(1).Command == QStringLiteral("grep \"!?Pattern:?!\" !&"));
  CHECK(List.At(1).Params == (ccShowResults | ccCopyResults));
  CHECK(List.At(1).ShortCut == 16455);   // Ctrl+G
  CHECK(List.Find(QStringLiteral("&Touch")) != nullptr);
  CHECK(List.Find(QStringLiteral("&touch")) == nullptr);
  CHECK(List.FindByShortCut(16455) == &List.At(1));

  // round trip
  CustomCommandList Changed = List;
  Changed.Add(CustomCommandType(QStringLiteral("Diff=x"), QStringLiteral("meld \"!\" \"!^!\""), ccLocal));
  Changed.Move(2, 0);
  Changed.Save(File);
  CHECK(File.Save(FileName));
  IniFile File2;
  CHECK(File2.Load(FileName));
  CustomCommandList Loaded;
  Loaded.Load(File2);
  CHECK(Loaded == Changed);
  CHECK(Loaded.At(0).Name == QStringLiteral("Diff=x"));
  CHECK(File2.ReadRaw(QStringLiteral("Configuration\\CustomCommands"), QStringLiteral("&Touch")) == QStringLiteral("touch%20\"!\""));
  CHECK(File2.ReadRaw(QStringLiteral("Configuration\\CustomCommandsParams"), QStringLiteral("Diff%3Dx")) == QStringLiteral("256"));
  CHECK(File2.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("ShowTips")) == QStringLiteral("0"));
  CHECK(File2.Keys(QStringLiteral("Configuration\\CustomCommandsShortCuts")).size() == 1);

  // emptied list
  CustomCommandList None;
  None.Save(File2);
  CHECK(File2.ReadRaw(QStringLiteral("Configuration"), QStringLiteral("CustomCommandsNone")) == QStringLiteral("1"));
  CustomCommandList LoadedNone;
  LoadedNone.Load(File2, &IsDefault);
  CHECK(!IsDefault);
  CHECK(LoadedNone.Count() == 0);
  Changed.Save(File2);
  CHECK(!File2.Keys(QStringLiteral("Configuration")).contains(QStringLiteral("CustomCommandsNone")));

  // list operations
  CustomCommandList Ops = Defaults;
  Ops.Reset();
  Ops.Change(0, Ops.At(0));
  CHECK(!Ops.Modified());
  CustomCommandType Edited = Ops.At(0);
  Edited.ShortCut = 0x4000 | 'E';
  Ops.Change(0, Edited);
  CHECK(Ops.Modified());
  CHECK(Ops.ShortCuts() == QList<int>{0x4000 | 'E'});
  Ops.Delete(0);
  CHECK(Ops.Count() == 5);

  // Configuration::Instance(): defaults are not written until changed
  Configuration & Config = Configuration::Instance();
  CHECK(CustomCommandList::Configured() == Defaults);
  CustomCommandList::SetConfigured(Defaults);
  CHECK(!Config.Ini().SectionExists(QStringLiteral("Configuration\\CustomCommands")));
  CustomCommandList::SetConfigured(Changed);
  CHECK(Config.Ini().SectionExists(QStringLiteral("Configuration\\CustomCommands")));
  CHECK(CustomCommandList::Configured() == Changed);
}

int main()
{
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Dir;
  CHECK(Dir.isValid());
  Configuration::Instance().SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));

  TestRemotePatterns();
  TestPrompts();
  TestErrors();
  TestLocalPatterns();
  TestAnalysis();
  TestNames();
  TestList(Dir.path());

  if (Failures == 0)
  {
    std::printf("All custom command tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
