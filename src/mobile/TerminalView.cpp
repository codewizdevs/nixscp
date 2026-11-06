#include "mobile/TerminalView.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

namespace
{
QColor ToColor(VTermState * State, VTermColor Color)
{
  vterm_state_convert_color_to_rgb(State, &Color);
  return QColor(Color.rgb.red, Color.rgb.green, Color.rgb.blue);
}
}

TerminalView::TerminalView(QQuickItem * Parent) : QQuickPaintedItem(Parent)
{
  FFont.setStyleHint(QFont::Monospace);
  FFont.setFixedPitch(true);
  FFont.setPixelSize(14);
  setFillColor(QColor(0x1e, 0x1e, 0x1e));
  setOpaquePainting(true);
  UpdateMetrics();
}

void TerminalView::setTerminal(TerminalSession * Terminal)
{
  if (FTerminal == Terminal)
  {
    return;
  }
  if (FTerminal != nullptr)
  {
    disconnect(FTerminal, nullptr, this, nullptr);
  }
  FTerminal = Terminal;
  if (FTerminal != nullptr)
  {
    connect(FTerminal, &TerminalSession::updated, this, [this]()
    {
      // a taller screen takes lines back from the scrollback (the keyboard
      // hidden, the phone rotated): the offset must not point before it
      FScrollOffset = TerminalSession::ValidOffset(FScrollOffset, FTerminal->ScrollbackCount());
      emit scrollChanged();
      update();
    });
    ApplySize();
  }
  emit terminalChanged();
  update();
}

void TerminalView::setFontFamily(const QString & Family)
{
  if (Family != FFont.family())
  {
    FFont.setFamily(Family);
    UpdateMetrics();
  }
}

void TerminalView::setFontSize(int Size)
{
  Size = qBound(8, Size, 40);
  if (Size != FFont.pixelSize())
  {
    FFont.setPixelSize(Size);
    UpdateMetrics();
  }
}

void TerminalView::setScrollOffset(int Offset)
{
  Offset = TerminalSession::ValidOffset(Offset, scrollbackCount());
  if (Offset != FScrollOffset)
  {
    FScrollOffset = Offset;
    emit scrollChanged();
    update();
  }
}

void TerminalView::UpdateMetrics()
{
  QFontMetricsF Metrics(FFont);
  FCellWidth = Metrics.horizontalAdvance(QLatin1Char('M'));
  FCellHeight = qCeil(Metrics.height());
  FAscent = Metrics.ascent();
  emit fontChanged();
  ApplySize();
  update();
}

int TerminalView::columns() const
{
  return qMax(10, static_cast<int>(width() / FCellWidth));
}

int TerminalView::rows() const
{
  return qMax(4, static_cast<int>(height() / FCellHeight));
}

void TerminalView::ApplySize()
{
  if ((FTerminal != nullptr) && (width() > 0) && (height() > 0))
  {
    FTerminal->Resize(columns(), rows());
  }
}

void TerminalView::geometryChange(const QRectF & NewGeometry, const QRectF & OldGeometry)
{
  QQuickPaintedItem::geometryChange(NewGeometry, OldGeometry);
  ApplySize();
}

void TerminalView::paint(QPainter * Painter)
{
  if (FTerminal == nullptr)
  {
    return;
  }
  VTermScreen * Screen = FTerminal->Screen();
  VTermState * State = FTerminal->State();
  int Rows = FTerminal->Rows();
  int Cols = FTerminal->Cols();
  int Back = FTerminal->ScrollbackCount();
  int Offset = TerminalSession::ValidOffset(FScrollOffset, Back);
  QColor DefaultFg(0xd4, 0xd4, 0xd4);
  QColor DefaultBg(0x1e, 0x1e, 0x1e);
  QFont Bold = FFont;
  Bold.setBold(true);
  Painter->setFont(FFont);

  for (int Row = 0; Row < Rows; Row++)
  {
    // with scrollOffset, the top rows come from the scrollback
    int Line = Row - Offset;
    const QVector<VTermScreenCell> * Saved = (Line < 0) ? &FTerminal->ScrollbackLine(Back + Line) : nullptr;
    qreal Y = Row * FCellHeight;
    for (int Col = 0; Col < Cols; Col++)
    {
      VTermScreenCell Cell{};
      if (Saved != nullptr)
      {
        if (Col >= Saved->size())
        {
          break;
        }
        Cell = Saved->at(Col);
      }
      else
      {
        vterm_screen_get_cell(Screen, VTermPos{Line, Col}, &Cell);
      }
      QColor Fg = VTERM_COLOR_IS_DEFAULT_FG(&Cell.fg) ? DefaultFg : ToColor(State, Cell.fg);
      QColor Bg = VTERM_COLOR_IS_DEFAULT_BG(&Cell.bg) ? DefaultBg : ToColor(State, Cell.bg);
      if (Cell.attrs.reverse)
      {
        std::swap(Fg, Bg);
      }
      bool Cursor = (Saved == nullptr) && (Offset == 0) && FTerminal->CursorVisible() &&
        (FTerminal->Cursor().row == Line) && (FTerminal->Cursor().col == Col);
      if (Cursor)
      {
        std::swap(Fg, Bg);
        if (Bg == DefaultBg) Bg = DefaultFg;
      }
      int Width = qMax(1, static_cast<int>(Cell.width));
      QRectF Rect(Col * FCellWidth, Y, Width * FCellWidth, FCellHeight);
      if (Bg != DefaultBg)
      {
        Painter->fillRect(Rect, Bg);
      }
      if (Cell.chars[0] != 0)
      {
        QString Text;
        for (int I = 0; (I < VTERM_MAX_CHARS_PER_CELL) && (Cell.chars[I] != 0); I++)
        {
          Text.append(QString::fromUcs4(reinterpret_cast<const char32_t *>(&Cell.chars[I]), 1));
        }
        if (Cell.attrs.bold != 0)
        {
          Painter->setFont(Bold);
        }
        Painter->setPen(Fg);
        Painter->drawText(QPointF(Rect.left(), Y + FAscent), Text);
        if (Cell.attrs.underline != 0)
        {
          Painter->drawLine(QPointF(Rect.left(), Y + FAscent + 2), QPointF(Rect.right(), Y + FAscent + 2));
        }
        if (Cell.attrs.bold != 0)
        {
          Painter->setFont(FFont);
        }
      }
      Col += Width - 1;
    }
  }
}
