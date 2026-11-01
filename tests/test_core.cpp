// Core unit tests (no GUI)
#include "core/Security.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/RemoteFile.h"

#include <QTemporaryDir>
#include <QFile>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

static void TestSecurity()
{
  for (const QString & Password : {QString(), QStringLiteral("secret"), QStringLiteral("pässwörd"), QString(300, QLatin1Char('x'))})
  {
    QByteArray Encrypted = Security::EncryptPassword(Password, QStringLiteral("userhost"));
    CHECK(Encrypted.size() >= Security::PWALG_SIMPLE_MAXLEN * 2);
    CHECK(Security::DecryptPassword(Encrypted, QStringLiteral("userhost")) == Password);
    // Wrong key yields empty password, as in WinSCP
    if (!Password.isEmpty())
    {
      CHECK(Security::DecryptPassword(Encrypted, QStringLiteral("other")).isEmpty());
    }
  }
  // Hand computed: flag FF, internal 00, length 05, shift 00, "abcde"
  // SimpleEncryptChar(c) = hex(~c ^ 0xA3)
  auto Enc = [](unsigned char C) { return QByteArray::number(static_cast<unsigned char>(~C ^ 0xA3), 16).rightJustified(2, '0').toUpper(); };
  QByteArray Manual = Enc(0xFF) + Enc(0x00) + Enc(5) + Enc(0) + Enc('k') + Enc('e') + Enc('y') + Enc('p') + Enc('w');
  CHECK(Security::DecryptPassword(Manual, QStringLiteral("key")) == QStringLiteral("pw"));
}

static void TestMunge()
{
  CHECK(MungeStr(QStringLiteral("My Site"), false) == QStringLiteral("My%20Site"));
  CHECK(MungeStr(QStringLiteral("folder/site"), false) == QStringLiteral("folder/site"));
  CHECK(MungeStr(QStringLiteral(".hidden"), false) == QStringLiteral("%2Ehidden"));
  CHECK(UnMungeStr(QStringLiteral("My%20Site")) == QStringLiteral("My Site"));
  CHECK(UnMungeStr(MungeStr(QStringLiteral("Šite ü"), false)) == QStringLiteral("Šite ü"));
  CHECK(MungeStr(QStringLiteral("*.txt"), true) == QStringLiteral("*.txt"));
}

static void TestSessions()
{
  QTemporaryDir Dir;
  QString IniPath = Dir.filePath(QStringLiteral("WinSCP.ini"));
  {
    // Typical WinSCP.ini fragment, including a key NixSCP does not know
    QFile F(IniPath);
    CHECK(F.open(QIODevice::WriteOnly));
    QByteArray Password = Security::EncryptPassword(QStringLiteral("topsecret"), QStringLiteral("rootexample.com"));
    F.write("[Sessions\\Work/My%20Server]\r\nHostName=example.com\r\nUserName=root\r\nPortNumber=2222\r\n"
            "FSProtocol=2\r\nPassword=" + Password + "\r\nProxyMethod=2\r\n\r\n"
            "[Configuration\\Interface]\r\nRandomSeedFile=x\r\n");
  }
  StoredSessionList List;
  QString Error;
  CHECK(List.ImportFromWinSCPIni(IniPath, &Error) == 1);
  auto Data = List.FindByName(QStringLiteral("Work/My Server"));
  CHECK(Data != nullptr);
  if (Data)
  {
    CHECK(Data->HostName == QStringLiteral("example.com"));
    CHECK(Data->UserName == QStringLiteral("root"));
    CHECK(Data->PortNumber == 2222);
    CHECK(Data->FSProtocol == fsSFTPonly);
    CHECK(Data->Password == QStringLiteral("topsecret"));
    CHECK(Data->FolderName() == QStringLiteral("Work"));
    CHECK(Data->LocalName() == QStringLiteral("My Server"));
  }

  // Save and reload; unknown keys must survive
  IniFile Out;
  List.Save(Out);
  QString OutPath = Dir.filePath(QStringLiteral("nixscp.ini"));
  CHECK(Out.Save(OutPath));
  IniFile In;
  CHECK(In.Load(OutPath));
  CHECK(In.ReadRaw(QStringLiteral("Sessions\\Work/My%20Server"), QStringLiteral("ProxyMethod")) == QStringLiteral("2"));
  StoredSessionList Reloaded;
  Reloaded.Load(In);
  auto Data2 = Reloaded.FindByName(QStringLiteral("Work/My Server"));
  CHECK(Data2 && (Data2->Password == QStringLiteral("topsecret")));

  // Delete
  Reloaded.Remove(QStringLiteral("Work/My Server"));
  Reloaded.Save(In);
  CHECK(!In.SectionExists(QStringLiteral("Sessions\\Work/My%20Server")));
}

static void TestRights()
{
  CHECK(Rights::ToText(0755) == QStringLiteral("rwxr-xr-x"));
  CHECK(Rights::ToText(04755) == QStringLiteral("rwsr-xr-x"));
  CHECK(Rights::ToText(01777) == QStringLiteral("rwxrwxrwt"));
  CHECK(Rights::ToOctal(0644) == QStringLiteral("0644"));
  unsigned int M = 0;
  CHECK(Rights::FromOctal(QStringLiteral("750"), M) && (M == 0750));
  CHECK(!Rights::FromOctal(QStringLiteral("9"), M));
  CHECK(UnixPath::Parent(QStringLiteral("/home/user")) == QStringLiteral("/home"));
  CHECK(UnixPath::Parent(QStringLiteral("/home")) == QStringLiteral("/"));
  CHECK(UnixPath::Parent(QStringLiteral("/")) == QStringLiteral("/"));
  CHECK(UnixPath::Combine(QStringLiteral("/"), QStringLiteral("etc")) == QStringLiteral("/etc"));
}

int main()
{
  TestSecurity();
  TestMunge();
  TestSessions();
  TestRights();
  std::printf("%s (%d failures)\n", Failures ? "FAILED" : "OK", Failures);
  return Failures ? 1 : 0;
}
