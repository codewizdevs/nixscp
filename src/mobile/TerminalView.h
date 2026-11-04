// Draws a TerminalSession's screen (and its scrollback, scrolled with
// scrollOffset) in a fixed-width font; its size in characters follows the
// item's size.
#pragma once

#include "mobile/TerminalSession.h"

#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class TerminalView : public QQuickPaintedItem
{
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(TerminalSession * terminal READ terminal WRITE setTerminal NOTIFY terminalChanged)
  Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
  Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged)
  // lines scrolled back (0 = the live screen)
  Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollChanged)
  Q_PROPERTY(int scrollbackCount READ scrollbackCount NOTIFY scrollChanged)
  Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY fontChanged)

public:
  explicit TerminalView(QQuickItem * Parent = nullptr);

  TerminalSession * terminal() const { return FTerminal; }
  void setTerminal(TerminalSession * Terminal);
  QString fontFamily() const { return FFont.family(); }
  void setFontFamily(const QString & Family);
  int fontSize() const { return FFont.pixelSize(); }
  void setFontSize(int Size);
  int scrollOffset() const { return FScrollOffset; }
  void setScrollOffset(int Offset);
  int scrollbackCount() const { return FTerminal ? FTerminal->ScrollbackCount() : 0; }
  qreal cellHeight() const { return FCellHeight; }

  // the size in characters for the item's size
  Q_INVOKABLE int columns() const;
  Q_INVOKABLE int rows() const;

  void paint(QPainter * Painter) override;

signals:
  void terminalChanged();
  void fontChanged();
  void scrollChanged();

protected:
  void geometryChange(const QRectF & NewGeometry, const QRectF & OldGeometry) override;

private:
  QPointer<TerminalSession> FTerminal;
  QFont FFont;
  qreal FCellWidth = 8;
  qreal FCellHeight = 16;
  qreal FAscent = 12;
  int FScrollOffset = 0;

  void UpdateMetrics();
  void ApplySize();
};
