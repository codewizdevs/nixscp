// Port of WinSCP source/core/CopyParam.{h,cpp} (TCopyParamType), plus the
// TGUICopyParamType additions (windows/GUIConfiguration.cpp: Queue*) and the
// transfer related confirmations of TConfiguration (ConfirmOverwriting,
// ConfirmResume), which NixSCP carries along with the transfer settings.
//
// Also: TTerminal::UsableCopyParamAttrs (per protocol capabilities),
// FormatMultiFilesToOneConfirmation (core/RemoteFiles.cpp), the text mode line
// ending conversion of TFileBuffer::Convert (streaming) and the timestamp
// precision helpers used by "New and updated files only" (core/Common.cpp
// ReduceDateTimePrecision / LessDateTimePrecision).
//
// Windows policy: PreserveReadOnly is adapted (download: the local file loses
// its write bits when the remote file has no owner write permission).
// Removed (no Linux equivalent): ClearArchive, InvalidCharsReplacement /
// LocalInvalidChars; their WinSCP.ini values are left untouched by Save()
// (and carried over verbatim by CopyParamList for presets).
// Not ported: EncryptNewFiles (needs the site file encryption key, which
// NixSCP sites do not have; the ini value is preserved), TransferSkipList /
// TransferResumeFile, PartOffset/PartSize, OnceDoneOperation.
#pragma once

#include "core/FileMasks.h"

#include <functional>

#include <QByteArray>
#include <QDateTime>
#include <QString>

class IniStorage;
class Terminal;

// TUsableCopyParamAttrs flags (GetInfoStr / dialog control enabling)
const int cpaIncludeMaskOnly = 0x01;
const int cpaNoTransferMode = 0x02;
const int cpaNoIncludeMask = 0x04;
const int cpaNoClearArchive = 0x08;
const int cpaNoPreserveTime = 0x10;
const int cpaNoRights = 0x20;
const int cpaNoPreserveReadOnly = 0x40;
const int cpaNoIgnorePermErrors = 0x80;
const int cpaNoNewerOnly = 0x100;
const int cpaNoRemoveCtrlZ = 0x200;
const int cpaNoRemoveBOM = 0x400;
const int cpaNoPreserveTimeDirs = 0x800;
const int cpaNoResumeSupport = 0x1000;
const int cpaNoEncryptNewFiles = 0x2000;
const int cpaNoCalculateSize = 0x4000;

// TCopyParamType::FileMask default (AnyMask of WinSCP)
inline const QString DefaultCopyFileMask = QStringLiteral("*.*");

// Extension of temporary files of resumable transfers (PartialExt)
inline const QString PartialExt = QStringLiteral(".filepart");

struct CopyParam
{
  enum TransferMode { tmBinary, tmAscii, tmAutomatic };
  enum FileNameCaseType { ncNoChange, ncUpperCase, ncLowerCase, ncFirstUpperCase, ncLowerCaseShort };
  enum ResumeSupportType { rsOn, rsSmart, rsOff };

  static const QString DefaultAsciiFileMask;

  TransferMode Mode = tmBinary;                  // TransferMode
  FileMasks AsciiFileMask{DefaultAsciiFileMask};
  FileNameCaseType FileNameCase = ncNoChange;
  bool PreserveReadOnly = false;                 // download: keep the remote file read-only locally
  bool PreserveTime = true;                      // "Preserve timestamp", default on
  bool PreserveTimeDirs = false;
  bool PreserveRights = false;                   // "Set permissions", default off
  unsigned int Rights = 0644;                    // rw-r--r-- (TRights::rfDefault)
  bool AddXToDirectories = true;
  bool IgnorePermErrors = false;
  ResumeSupportType ResumeSupport = rsSmart;
  qint64 ResumeThreshold = 100 * 1024;           // 100 KB
  bool CalculateSize = true;                     // kept for WinSCP.ini compatibility only
  FileMasks IncludeFileMask;
  bool RemoveCtrlZ = false;                      // text mode uploads
  bool RemoveBOM = false;
  unsigned long CPSLimit = 0;                    // bytes/s, 0 = unlimited
  bool NewerOnly = false;
  bool ExcludeHiddenFiles = false;
  bool ExcludeEmptyDirectories = false;
  // Name part of the Copy dialog target ("*.*", "*.bak" or a new name of a
  // single file); applied to the top-level files only. Not saved.
  QString FileMask = DefaultCopyFileMask;

  // TConfiguration (Preferences > Confirmations), not saved by Save()
  bool ConfirmOverwriting = true;
  bool ConfirmResume = true;
  // cpAppend (scripting): an existing target gets the file added at its end. Not saved.
  bool Append = false;
  // TTerminal::GetBaseFileName: the site's TrimVMSVersions, for ChangeFileName. Not saved.
  bool TrimVMSVersions = false;

  // TGUICopyParamType
  bool Queue = false;                            // "Transfer in background" default
  bool QueueNoConfirmation = true;
  bool QueueParallel = true;

  bool operator==(const CopyParam & Other) const;
  bool operator!=(const CopyParam & Other) const { return !(*this == Other); }

  // Usable attributes of SFTP transfers in given direction (TTerminal::UsableCopyParamAttrs)
  static int UsableAttrs(bool ToRemote);
  // TTerminal::UsableCopyParamAttrs for the protocol of the (opened) terminal;
  // Move adds cpaNoClearArchive (cpDelete). A null terminal means SFTP.
  static int UsableAttrs(bool ToRemote, const Terminal * ATerminal, bool Move = false);
  // The same from a static capability answer (e.g. Terminal::ProtocolCapability);
  // Capability is an FSCapability value
  static int UsableAttrs(bool ToRemote, const std::function<bool(int Capability)> & IsCapable, bool Move = false);

  QString GetInfoStr(const QString & Separator, int Attrs) const;
  bool AnyUsableCopyParam(int Attrs) const;

  // FileName is the full path of the source file
  bool UseAsciiTransfer(const QString & FileName, bool Local, const FileMasks::Params & Params) const;
  bool AllowResume(qint64 Size, const QString & FileName) const;
  bool AllowAnyTransfer() const;
  bool AllowTransfer(const QString & FileName, bool Local, bool Directory, const FileMasks::Params & Params, bool Hidden) const;
  // File name case modification (TCopyParamType::ChangeFileName, nested level)
  QString ChangeFileName(const QString & FileName) const;
  // TCopyParamType::ChangeFileName: FirstLevel (files selected for the
  // transfer, not their contents) also applies FileMask (MaskFileName)
  QString ChangeFileName(const QString & FileName, bool FirstLevel) const;
  // Windows (TCopyParamType::ValidLocalFileName with the default token
  // replacement): characters a Windows file name cannot have become %XX, a
  // trailing space/dot is encoded and reserved names (CON, ...) get "%00";
  // RestoreChars reverses it for uploads. Both return the name as is on Linux.
  static QString ValidLocalFileName(const QString & FileName);
  static QString RestoreChars(const QString & FileName);
  // The same for Windows, on any platform (what the two above do there)
  static QString WindowsLocalFileName(const QString & FileName);
  static QString WindowsRestoreChars(const QString & FileName);
  // Rights for newly uploaded file/directory (TCopyParamType::RemoteFileRights)
  unsigned int RemoteFileRights(bool Directory) const;

  // Settings for temporary transfers of edited/opened files
  // (TCustomScpExplorerForm::TemporaryFileCopyParam): no masks, no renaming, no newer-only
  CopyParam TemporaryFileCopyParam(bool ForceText = false) const;

  // Storage must be opened at the key (e.g. Configuration\Interface\CopyParam)
  void Load(const IniStorage & Storage);
  void Save(IniStorage & Storage) const;

private:
  void DoGetInfoStr(const QString & Separator, int Options, QString & Result, bool & SomeAttrIncluded) const;
};

// TRights::Text <-> mode ("rw-r--r--", s/S/t/T for special bits)
QString RightsToText(unsigned int Mode);
bool RightsFromText(const QString & Text, unsigned int & Mode);
// TRights::AddExecute: x where r
unsigned int AddExecute(unsigned int Mode);

// Speed limit combo texts (KB/s or "Unlimited")
extern const unsigned long MinSpeed;
extern const unsigned long MaxSpeed;
bool TryGetSpeedLimit(const QString & Text, unsigned long & Speed);
unsigned long GetSpeedLimit(const QString & Text);   // throws ExtException
QString SetSpeedLimit(unsigned long Limit);

// MULTI_FILES_TO_ONE confirmation for a target path whose name part is not a
// mask, while multiple files are transferred (FormatMultiFilesToOneConfirmation)
QString FormatMultiFilesToOneConfirmation(const QString & Target);

// Local permissions of a downloaded file with PreserveReadOnly: the write bits
// are cleared when the remote file has no owner write permission
// (Windows: faReadOnly attribute). Returns LocalMode unchanged otherwise.
unsigned int PreserveReadOnlyMode(unsigned int LocalMode, unsigned int RemoteMode, bool PreserveReadOnly);

// Line endings (TEOLType values of core/SessionData.h: 0 LF, 1 CRLF, 2 CR)
QByteArray EOLToStr(int EOLType);
// End of line of local text files on Linux
inline const QByteArray LocalEOL = QByteArrayLiteral("\n");

// Text mode line ending conversion (TFileBuffer::Convert), for a stream of
// chunks: a two byte EOL split between chunks is handled correctly.
// Download: TextEOLConverter(EOLToStr(Data.EOLType), LocalEOL);
// upload: TextEOLConverter(LocalEOL, EOLToStr(Data.EOLType)).
// CRLF sequences pass unmodified when converting from a one byte EOL to CRLF.
class TextEOLConverter
{
public:
  TextEOLConverter(const QByteArray & Source, const QByteArray & Dest);
  bool Identity() const { return FSource == FDest; }
  // Converted data of the chunk; may keep the last byte back (Finish returns it)
  QByteArray Convert(const QByteArray & Chunk);
  QByteArray Finish();

private:
  QByteArray FSource;
  QByteArray FDest;
  bool FPending = false;   // a byte (FSource[0] or FDest[0]) kept back from the last chunk
  char FPendingChar = 0;
  void Emit(QByteArray & Out, char C);
};

QDateTime ReduceDateTimePrecision(const QDateTime & DateTime, TModificationFmt Precision);
TModificationFmt LessDateTimePrecision(TModificationFmt Precision1, TModificationFmt Precision2);
// "New and updated files only": Source is newer than Dest when compared with
// the lower of both precisions (whole seconds at best), so a minute precision
// FTP listing does not make every local file look newer
bool IsNewerModification(const QDateTime & Source, TModificationFmt SourcePrecision,
  const QDateTime & Dest, TModificationFmt DestPrecision);
