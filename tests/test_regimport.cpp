// Import of WinSCP sites from a Windows registry export (.reg)
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"

#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

int main()
{
  QTemporaryDir Dir;
  QString Path = Dir.filePath(QStringLiteral("winscp.reg"));
  QString Password = QString::fromLatin1(Security::EncryptPassword(QStringLiteral("s3cret"), QStringLiteral("deployprod.example.com")));
  // As written by "reg export": UTF-16LE with BOM, CRLF, escaped backslashes and quotes
  QString Text = QStringLiteral(
    "Windows Registry Editor Version 5.00\r\n\r\n"
    "[HKEY_CURRENT_USER\\Software\\Martin Prikryl\\WinSCP 2]\r\n\r\n"
    "[HKEY_CURRENT_USER\\Software\\Martin Prikryl\\WinSCP 2\\Configuration\\Interface]\r\n"
    "\"ShowHiddenFiles\"=dword:00000001\r\n\r\n"
    "[HKEY_CURRENT_USER\\Software\\Martin Prikryl\\WinSCP 2\\Sessions\\Work/Prod%20server]\r\n"
    "\"HostName\"=\"prod.example.com\"\r\n"
    "\"UserName\"=\"deploy\"\r\n"
    "\"PortNumber\"=dword:00000016\r\n"
    "\"FSProtocol\"=dword:00000002\r\n"
    "\"Password\"=\"%1\"\r\n"
    "\"PublicKeyFile\"=\"C:\\\\Users\\\\me\\\\.ssh\\\\key.ppk\"\r\n"
    "\"Note\"=\"says \\\"hi\\\"\"\r\n"
    "\"TryAgent\"=dword:00000000\r\n\r\n"
    "[HKEY_CURRENT_USER\\Software\\Martin Prikryl\\WinSCP 2\\Sessions\\Home%20NAS]\r\n"
    "\"HostName\"=\"192.168.1.10\"\r\n"
    "\"PortNumber\"=dword:00000822\r\n").arg(Password);
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly));
  F.write("\xFF\xFE");
  F.write(reinterpret_cast<const char *>(Text.utf16()), Text.size() * 2);
  F.close();

  StoredSessionList List;
  QString Error;
  CHECK(List.ImportFromWinSCPIni(Path, &Error) == 2);
  CHECK(Error.isEmpty());
  auto Prod = List.FindByName(QStringLiteral("Work/Prod server"));
  CHECK(Prod != nullptr);
  if (Prod)
  {
    CHECK(Prod->HostName == QStringLiteral("prod.example.com"));
    CHECK(Prod->UserName == QStringLiteral("deploy"));
    CHECK(Prod->PortNumber == 22);
    CHECK(Prod->FSProtocol == fsSFTPonly);
    CHECK(Prod->Password == QStringLiteral("s3cret"));
    CHECK(Prod->PublicKeyFile == QStringLiteral("C:\\Users\\me\\.ssh\\key.ppk"));
    CHECK(Prod->Note == QStringLiteral("says \"hi\""));
    CHECK(!Prod->TryAgent);
  }
  auto Nas = List.FindByName(QStringLiteral("Home NAS"));
  CHECK(Nas && (Nas->PortNumber == 2082));

  IniFile Ini;
  CHECK(Ini.Load(Path));
  CHECK(Ini.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("ShowHiddenFiles")) == QStringLiteral("1"));

  std::printf("%s (%d failures)\n", Failures ? "FAILED" : "OK", Failures);
  return Failures ? 1 : 0;
}
