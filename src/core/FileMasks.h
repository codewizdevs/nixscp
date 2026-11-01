// Port of WinSCP source/core/FileMasks.{h,cpp} (TFileMasks, MaskFileName & co.)
// and of the mask matching of Delphi's Masks unit (Masks::TMask), which
// WinSCP uses for individual masks.
//
// Mask syntax (https://winscp.net/eng/docs/file_mask):
//   masks are separated by ';' or ',' (a doubled delimiter is a literal one);
//   '|' separates include masks from exclude masks;
//   '*' any number of characters, '?' one character, '[a-z]' / '[!a-z]' sets;
//   matching is case insensitive;
//   "*." matches files without extension, "*" and "*.*" any file;
//   a trailing '/' makes a directory mask ("*/", "|.git/");
//   a mask with a path ("/home/martin/*.txt") is matched against the full path;
//   size and time conditions: "*.txt>10K", "<1M", ">=2024-01-01", "<2024-01-01 12:00",
//   ">1D" (modified in the last day; S, N, H, D, Y units, "today", "yesterday",
//   "1DS"-like start-of-period forms).
// Not ported: relative "./" path masks with SetRoots (used by synchronization).
#pragma once

#include "core/FileSystem.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

class FileMasksException : public ExtException
{
public:
  // ErrorStart is 0-based index into the mask string
  FileMasksException(const QString & Message, int AErrorStart, int AErrorLen) :
    ExtException(Message), ErrorStart(AErrorStart), ErrorLen(AErrorLen) {}
  int ErrorStart;
  int ErrorLen;
};

// Single wildcard mask (Masks::TMask): '*', '?', '[set]', '[!set]', case insensitive
class WildcardMask
{
public:
  // Throws std::invalid_argument on malformed set
  explicit WildcardMask(const QString & Mask);
  bool Matches(const QString & Str) const;

private:
  struct Element
  {
    enum Kind { Literal, AnyChar, AnyText, Set };
    Kind ElementKind = Literal;
    QChar Char;
    bool Negate = false;
    std::vector<std::pair<QChar, QChar>> Ranges;
  };
  std::vector<Element> FElements;
  bool MatchesElement(const Element & E, QChar C) const;
};

class FileMasks
{
public:
  struct Params
  {
    qint64 Size = 0;
    QDateTime Modification;
  };

  static const QChar IncludeExcludeFileMasksDelimiter;   // '|'
  static const QString FileMasksDelimiters;              // ";,"

  static bool IsMask(const QString & Mask);
  static QString EscapeMask(const QString & S);
  static QString NormalizeMask(const QString & Mask, const QString & AnyMask = QString());

  FileMasks() = default;
  FileMasks(const QString & AMasks);   // throws FileMasksException
  FileMasks & operator=(const QString & AMasks) { SetMasks(AMasks); return *this; }
  bool operator==(const FileMasks & Other) const { return FStr == Other.FStr; }
  bool operator!=(const FileMasks & Other) const { return FStr != Other.FStr; }
  bool operator==(const QString & Other) const { return FStr == Other; }

  QString Masks() const { return FStr; }
  void SetMasks(const QString & Value);   // throws FileMasksException, keeps previous masks then
  void SetMask(const QString & Mask);     // single mask, delimiters are not interpreted
  bool IsEmpty() const { return FStr.trimmed().isEmpty(); }

  // FileName is a full path ("/dir/file"); Local selects the local side
  // (on Linux both sides use the same path syntax, the flag is kept for parity).
  bool Matches(const QString & FileName, bool Local, bool Directory, const Params * AParams = nullptr) const;
  bool Matches(const QString & FileName, bool Local, bool Directory, const Params * AParams,
    bool RecurseInclude, bool & ImplicitMatch) const;
  // Name of the file and path of the directory containing it ("/dir"; empty when unknown)
  bool Matches(const QString & FileName, bool Directory, const QString & Path, const Params * AParams) const;
  // Name only; masks with paths do not match
  bool MatchesFileName(const QString & FileName, bool Directory = false, const Params * AParams = nullptr) const;

  bool NoImplicitMatchWithDirExcludeMask = false;
  bool AllDirsAreImplicitlyIncluded = false;

  // User strings of individual masks (for a mask editor)
  QStringList IncludeFileMasksStr() const { return MasksStr(false, true); }
  QStringList ExcludeFileMasksStr() const { return MasksStr(false, false); }
  QStringList IncludeDirectoryMasksStr() const { return MasksStr(true, true); }
  QStringList ExcludeDirectoryMasksStr() const { return MasksStr(true, false); }

private:
  struct Mask
  {
    enum Kind { Any, NoExt, Regular };
    enum Boundary { None, Open, Close };

    Kind FileNameMaskKind = Any;
    std::shared_ptr<WildcardMask> FileNameMask;
    Kind DirectoryMaskKind = Any;
    std::shared_ptr<WildcardMask> DirectoryMask;

    Boundary HighSizeMask = None;
    qint64 HighSize = 0;
    Boundary LowSizeMask = None;
    qint64 LowSize = 0;

    Boundary HighModificationMask = None;
    QDateTime HighModification;
    Boundary LowModificationMask = None;
    QDateTime LowModification;

    QString MaskStr;
    QString UserStr;
  };
  using MaskList = std::vector<Mask>;

  QString FStr;
  MaskList FMasks[4];

  static int MaskIndex(bool Directory, bool Include) { return (Directory ? 2 : 0) + (Include ? 0 : 1); }
  QStringList MasksStr(bool Directory, bool Include) const;
  void SetStr(const QString & Str, bool SingleMask);
  void CreateMask(const QString & MaskStr, int MaskStart, bool Include);
  void CreateMaskMask(const QString & AMask, int Start, int End, bool Ex,
    Mask::Kind & MaskKind, std::shared_ptr<WildcardMask> & MaskMask);
  [[noreturn]] void ThrowError(int Start, int End) const;
  static bool MatchesMasks(const QString & FileName, bool Directory, const QString & Path,
    const Params * AParams, const MaskList & Masks, bool Recurse);
  static bool MatchesMaskMask(Mask::Kind MaskKind, const WildcardMask * MaskMask, const QString & Str);
  bool DoMatches(const QString & FileName, bool Directory, const QString & Path, const Params * AParams,
    bool RecurseInclude, bool & ImplicitMatch) const;
};

// MaskFileName("readme.txt", "*.bak") == "readme.bak" (target file name masks of the Copy dialog)
QString MaskFileName(QString FileName, const QString & Mask);
bool IsFileNameMask(const QString & Mask);
bool IsEffectiveFileNameMask(const QString & Mask);
QString DelimitFileNameMask(QString Mask);

// Common.cpp helpers used by masks
bool TryStrToSize(QString SizeStr, qint64 & Size);
bool TryRelativeStrToDateTime(QString S, QDateTime & DateTime, bool Add);
bool TryStrToDateTimeStandard(const QString & S, QDateTime & Value);
