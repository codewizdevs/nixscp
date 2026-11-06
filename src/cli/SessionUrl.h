// Port of WinSCP source/core/SessionData.cpp: TSessionData::ParseUrl,
// TStoredSessionList::ParseUrl, ApplyRawSettings and the session switches
// (-hostkey, -certificate, -privatekey, -clientcert, -passphrase, -passive,
// -implicit, -explicit, -explicitssl, -timeout, -username, -password,
// -newpassword, -passwordsfromfiles, -rawsettings), DecodeUrlChars and
// host key fingerprint matching (TSecureShell::VerifyFingerprint).
#pragma once

#include "core/SessionData.h"

#include <QString>
#include <QStringList>

class Options;

// ParsedInfo flags
const int piProtocolDefined = 0x01;
const int piDefaultsOnly = 0x02;

// Session settings NixSCP's SessionData does not carry (yet).
// -hostkey, -certificate and ;fingerprint= go to SessionData::HostKey
// (WinSCP.ini "SshHostKey"), which the core verifies (SSH and TLS);
// the raw setting "HostKey" is the host key algorithm list (HostKeyList).
// "*" and "acceptnew" count from -hostkey, -certificate and -rawsettings,
// which a script author has written, never from a URL.
struct SessionExtras
{
  QString Passphrase;     // -passphrase / Passphrase raw setting
  // "user:@host" or -password= given explicitly empty (DenormalizeString):
  // the first password prompt is answered with an empty password
  bool EmptyPassword = false;
  QString NewPassword;    // -newpassword (TSessionData::ChangePassword/NewPassword)
};

// TSessionData::ReadPasswordsFromFiles (-passwordsfromfiles): each password
// value is a file name whose first line is the password
void ReadPasswordsFromFiles(SessionData & Data, SessionExtras & Extras);

QString DecodeUrlChars(QString S);

// Data is reset to the stored site, or to Default Settings for an ad-hoc URL.
// Stored may be null. Switches recognized are consumed from Opts (may be null).
// Throws ExtException on invalid switch values.
// Unsafe (pufUnsafe): the URL is from somebody else, a link for one, so of its
// raw settings only SessionData::IsUrlSetting ones apply, without a word.
void ParseSessionUrl(const QString & Url, Options * Opts, const StoredSessionList * Stored,
  SessionData & Data, SessionExtras & Extras, int & ParsedInfo, QString * MaskedUrl = nullptr, bool Unsafe = false);

// Applies "Key=Value" settings in WinSCP.ini format (TSessionData::ApplyRawSettings)
void ApplySessionRawSettings(SessionData & Data, SessionExtras & Extras, const QStringList & Settings);

// Normalizes a host key fingerprint for comparison: drops the key type and
// size prefix, "SHA256:"/"MD5:" prefixes and base64 padding
QString NormalizeFingerprint(const QString & Fingerprint);
// Does Expected (one entry of -hostkey, ';'-separated list allowed) match?
bool VerifyFingerprint(const QString & Expected, const QString & FingerprintSHA256, const QString & FingerprintMD5);
