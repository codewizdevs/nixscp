// A text file for the built-in editor, byte for byte: the encodings a file
// can be in (without ICU Qt knows only UTF-8 and Latin-1 here, so the code
// pages are tables), and what becomes of the file when the text is saved.
//
// A TextEdit does not give its text back as it got it: CR LF and CR become
// LF, a no-break space a space, line and paragraph separators LF. So a save
// never writes the editor's text as it is: only the part that was really
// changed is replaced in the original, and that part gets the file's own
// line ending. What cannot be written in the file's encoding is refused,
// never replaced.
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QStringList>

namespace TextFile
{
  extern const char * const Utf8;

  // The encodings offered for a file that is not UTF-8
  QStringList LegacyEncodings();

  // The bytes as valid UTF-8 (with or without a BOM)
  bool IsUtf8(const QByteArray & Data);
  // Text of the bytes (a UTF-8 BOM left out); false when they are not in this
  // encoding or would not come back the same when written again
  bool Decode(const QByteArray & Data, const QString & Encoding, QString & Text);
  // false with the first character (in Bad) this encoding has no bytes for
  bool Encode(const QString & Text, const QString & Encoding, QByteArray & Data, QString * Bad = nullptr);

  // "LF", "CRLF", "CR", "Mixed", or "" for a single line
  QString LineEnding(const QByteArray & Data);
  // The text as a TextEdit gives it back
  QString Displayed(const QString & Text);
  // The first line with more than ASCII: what tells encodings apart
  QString Sample(const QString & Text);
  // Original (the bytes of the file) with the changes of Edited (the
  // editor's text, as Displayed) in it; false with the reason in Error
  bool Merge(const QByteArray & Original, const QString & Edited, const QString & Encoding, QByteArray & Result, QString & Error);

  // Copies Source to Target; false when reading or writing failed (a read
  // error is not the end of the file)
  bool Copy(QIODevice & Source, QIODevice & Target);
  // Name, or "name (2).ext" when the folder has a file of that name
  QString UniqueName(const QString & Folder, const QString & Name);
}
