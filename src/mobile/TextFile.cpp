#include "mobile/TextFile.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringDecoder>
#include <QStringEncoder>

#include <array>

namespace
{
// 0x80..0xFF; undefined bytes are the C1 control of that value, so that any
// file comes back unchanged
const char16_t Windows1250[128] = {
  0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
  0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
  0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
  0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
  0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
  0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
  0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
  0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
  0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
  0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
  0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
  0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
  0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
  0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
  0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

const char16_t Windows1252[128] = {
  0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
  0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
  0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
  0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
  0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
  0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
  0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
  0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
  0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
  0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
  0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
  0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
  0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
  0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

const char16_t Iso88592[128] = {
  0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
  0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
  0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
  0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
  0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
  0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
  0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
  0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
  0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
  0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
  0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
  0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
  0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
  0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
  0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
  0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

const char * const Latin1Name = "Latin-1";
const char * const Windows1250Name = "Windows-1250";
const char * const Windows1252Name = "Windows-1252";
const char * const Iso88592Name = "ISO-8859-2";

const char16_t * Table(const QString & Encoding)
{
  if (Encoding == QLatin1String(Windows1250Name)) return Windows1250;
  if (Encoding == QLatin1String(Windows1252Name)) return Windows1252;
  if (Encoding == QLatin1String(Iso88592Name)) return Iso88592;
  return nullptr;
}

const QByteArray Utf8Bom("\xEF\xBB\xBF");

// The characters a TextEdit turns into LF or a space (QTextDocument::toPlainText)
bool IsBreak(QChar C)
{
  ushort U = C.unicode();
  return (U == u'\r') || (U == u'\n') || (U == 0x2028) || (U == 0x2029) || (U == 0xFDD0) || (U == 0xFDD1);
}

QChar Shown(QChar C)
{
  return IsBreak(C) ? QChar(u'\n') : ((C.unicode() == 0xA0) ? QChar(u' ') : C);
}

// The line ending of the file: the most common one (LF when there is none)
QString Ending(const QString & Text)
{
  std::array<int, 3> Count{0, 0, 0};   // LF, CR LF, CR
  for (qsizetype I = 0; I < Text.size(); I++)
  {
    if (Text.at(I) == u'\n')
    {
      Count[0]++;
    }
    else if (Text.at(I) == u'\r')
    {
      bool Pair = (I + 1 < Text.size()) && (Text.at(I + 1) == u'\n');
      Count[Pair ? 1 : 2]++;
      I += Pair ? 1 : 0;
    }
  }
  if ((Count[1] >= Count[0]) && (Count[1] >= Count[2]) && (Count[1] > 0))
  {
    return QStringLiteral("\r\n");
  }
  return (Count[2] > Count[0]) ? QStringLiteral("\r") : QStringLiteral("\n");
}
}

namespace TextFile
{
const char * const Utf8 = "UTF-8";

QStringList LegacyEncodings()
{
  return {QLatin1String(Windows1250Name), QLatin1String(Windows1252Name), QLatin1String(Iso88592Name), QLatin1String(Latin1Name)};
}

bool IsUtf8(const QByteArray & Data)
{
  QString Text;
  return Decode(Data, QLatin1String(Utf8), Text);
}

bool Decode(const QByteArray & Data, const QString & Encoding, QString & Text)
{
  if (Encoding == QLatin1String(Utf8))
  {
    QStringDecoder Decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    QByteArrayView Body(Data);
    if (Data.startsWith(Utf8Bom))
    {
      Body = Body.sliced(Utf8Bom.size());
    }
    Text = Decoder.decode(Body);
    return !Decoder.hasError();
  }
  const char16_t * Codes = Table(Encoding);
  if ((Codes == nullptr) && (Encoding != QLatin1String(Latin1Name)))
  {
    return false;
  }
  Text.resize(Data.size());
  for (qsizetype I = 0; I < Data.size(); I++)
  {
    uchar Byte = static_cast<uchar>(Data.at(I));
    Text[I] = QChar(((Byte < 0x80) || (Codes == nullptr)) ? char16_t(Byte) : Codes[Byte - 0x80]);
  }
  return true;
}

bool Encode(const QString & Text, const QString & Encoding, QByteArray & Data, QString * Bad)
{
  if (Encoding == QLatin1String(Utf8))
  {
    QStringEncoder Encoder(QStringEncoder::Utf8, QStringEncoder::Flag::Stateless);
    Data = Encoder.encode(Text);
    if (Encoder.hasError())
    {
      if (Bad != nullptr)
      {
        *Bad = QString(QChar(0xFFFD));
      }
      return false;
    }
    return true;
  }
  const char16_t * Codes = Table(Encoding);
  if ((Codes == nullptr) && (Encoding != QLatin1String(Latin1Name)))
  {
    return false;
  }
  QHash<char16_t, uchar> Bytes;
  for (int I = 0; (Codes != nullptr) && (I < 128); I++)
  {
    Bytes.insert(Codes[I], static_cast<uchar>(0x80 + I));
  }
  Data.resize(Text.size());
  for (qsizetype I = 0; I < Text.size(); I++)
  {
    char16_t Code = Text.at(I).unicode();
    if ((Code < 0x80) || ((Codes == nullptr) && (Code < 0x100)))
    {
      Data[I] = static_cast<char>(Code);
      continue;
    }
    auto It = Bytes.constFind(Code);
    if (It == Bytes.constEnd())
    {
      if (Bad != nullptr)
      {
        bool Pair = Text.at(I).isHighSurrogate() && (I + 1 < Text.size());
        *Bad = Text.mid(I, Pair ? 2 : 1);
      }
      return false;
    }
    Data[I] = static_cast<char>(It.value());
  }
  return true;
}

QString LineEnding(const QByteArray & Data)
{
  bool Lf = false;
  bool CrLf = false;
  bool Cr = false;
  for (qsizetype I = 0; I < Data.size(); I++)
  {
    if (Data.at(I) == '\n')
    {
      Lf = true;
    }
    else if (Data.at(I) == '\r')
    {
      bool Pair = (I + 1 < Data.size()) && (Data.at(I + 1) == '\n');
      (Pair ? CrLf : Cr) = true;
      I += Pair ? 1 : 0;
    }
  }
  int Kinds = int(Lf) + int(CrLf) + int(Cr);
  if (Kinds > 1)
  {
    return QStringLiteral("Mixed");
  }
  return CrLf ? QStringLiteral("CRLF") : (Cr ? QStringLiteral("CR") : (Lf ? QStringLiteral("LF") : QString()));
}

QString Displayed(const QString & Text)
{
  QString Result;
  Result.reserve(Text.size());
  for (qsizetype I = 0; I < Text.size(); I++)
  {
    if ((Text.at(I) == u'\r') && (I + 1 < Text.size()) && (Text.at(I + 1) == u'\n'))
    {
      I++;
    }
    Result.append(Shown(Text.at(I)));
  }
  return Result;
}

QString Sample(const QString & Text)
{
  const QStringList Lines = Text.split(QLatin1Char('\n'));
  for (const QString & Line : Lines)
  {
    if (std::any_of(Line.begin(), Line.end(), [](QChar C) { return C.unicode() >= 0x80; }))
    {
      return Line.trimmed().left(120);
    }
  }
  return QString();
}

bool Merge(const QByteArray & Original, const QString & Edited, const QString & Encoding, QByteArray & Result, QString & Error)
{
  QString Old;
  if (!Decode(Original, Encoding, Old))
  {
    Error = QStringLiteral("The file is not in %1.").arg(Encoding);
    return false;
  }
  // from the start: what did not change (a line ending is one character in the editor)
  qsizetype Head = 0;       // in Old
  qsizetype EditedHead = 0;
  while ((Head < Old.size()) && (EditedHead < Edited.size()))
  {
    qsizetype Length = ((Old.at(Head) == u'\r') && (Head + 1 < Old.size()) && (Old.at(Head + 1) == u'\n')) ? 2 : 1;
    if (Shown(Old.at(Head)) != Edited.at(EditedHead))
    {
      break;
    }
    Head += Length;
    EditedHead++;
  }
  // from the end, up to there
  qsizetype Tail = Old.size();
  qsizetype EditedTail = Edited.size();
  while ((Tail > Head) && (EditedTail > EditedHead))
  {
    qsizetype Length = ((Old.at(Tail - 1) == u'\n') && (Tail - 2 >= Head) && (Old.at(Tail - 2) == u'\r')) ? 2 : 1;
    if (Shown(Old.at(Tail - 1)) != Edited.at(EditedTail - 1))
    {
      break;
    }
    Tail -= Length;
    EditedTail--;
  }
  QString Changed = Edited.mid(EditedHead, EditedTail - EditedHead);
  Changed.replace(QLatin1Char('\n'), Ending(Old));
  QString Text = Old.left(Head) + Changed + Old.mid(Tail);
  QString Bad;
  if (!Encode(Text, Encoding, Result, &Bad))
  {
    Error = QStringLiteral("The character \"%1\" cannot be saved in %2. Remove it to save the file.").arg(Bad, Encoding);
    return false;
  }
  if ((Encoding == QLatin1String(Utf8)) && Original.startsWith(Utf8Bom))
  {
    Result.prepend(Utf8Bom);
  }
  return true;
}

QString UniqueName(const QString & Folder, const QString & Name)
{
  QFileInfo Info(Name);
  QString Result = Name;
  for (int Copy = 2; QFile::exists(Folder + QLatin1Char('/') + Result); Copy++)
  {
    Result = Info.completeBaseName() + QStringLiteral(" (%1)").arg(Copy) +
      (Info.suffix().isEmpty() ? QString() : (QLatin1Char('.') + Info.suffix()));
  }
  return Result;
}

bool Copy(QIODevice & Source, QIODevice & Target)
{
  QByteArray Buffer(1024 * 1024, Qt::Uninitialized);
  for (;;)
  {
    qint64 Read = Source.read(Buffer.data(), Buffer.size());
    if (Read < 0)
    {
      return false;
    }
    if (Read == 0)
    {
      // the end, unless the source only says so (a sequential device that is not at its end)
      return Source.atEnd();
    }
    if (Target.write(Buffer.constData(), Read) != Read)
    {
      return false;
    }
  }
}
}
