// Port of WinSCP source/forms/NonVisual.{cpp,dfm} (TNonVisualDataModule: the
// action list with captions, 'Short|Long' hints and the Commander keyboard
// shortcuts of CommanderShortcuts/CloneShortcuts) and the menu/popup layouts
// of forms/ScpCommander.dfm and forms/NonVisual.dfm.
//
// Actions are named as in WinSCP (e.g. "RemoteCopyAction"), so the menu
// layouts below read like the .dfm files and Icons::ForAction finds the
// WinSCP glyph of each action.
//
// Keyboard shortcuts: WinSCP has Local*/Remote* pairs with the same shortcut
// (Ctrl+R refreshes the active panel) and only executes the one of the
// active side (TScpCommanderForm::AllowedAction). Each action has a scope:
// window-wide, active only while its side (left = Local, right = Remote) is
// the current panel, or displayed only (the menu shows the key, another
// action handles it), so a key never matches two enabled actions.
#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QToolBar;
class QWidget;

class CommanderActions : public QObject
{
  Q_OBJECT

public:
  enum ShortCutScope
  {
    ssWindow,     // always active (in the main window)
    ssLocal,      // active while the left (local) panel is current
    ssRemote,     // active while the right (remote / other local) panel is current
    ssDisplay,    // shown in menus only
  };

  explicit CommanderActions(QWidget * Window);
  ~CommanderActions() override;

  // The action by its WinSCP name; a missing name is a programming error
  // (a warning and an inert placeholder action are returned)
  QAction * Get(const QString & Name) const;
  QAction * operator()(const char * Name) const { return Get(QString::fromLatin1(Name)); }
  bool Has(const QString & Name) const;
  QStringList Names() const;
  // Connects the action's triggered signal
  void On(const char * Name, const std::function<void()> & Func);
  // Adds an action not in the WinSCP table (e.g. editor or queue actions)
  void Register(const QString & Name, QAction * Action, ShortCutScope Scope = ssWindow);

  ShortCutScope Scope(QAction * Action) const;
  void SetScope(QAction * Action, ShortCutScope Scope);
  // Re-registers the shortcuts for the current side (ssLocal/ssRemote actions)
  void SetCurrentSide(bool Remote);
  bool CurrentSideRemote() const { return FCurrentRemote; }
  // TNonVisualDataModule::CommanderShortcuts with ExplorerKeyboardShortcuts
  void ApplyCommanderShortcuts(bool ExplorerKeyboardShortcuts);

  // ---- Proxy items (an item of a menu/toolbar whose action changes, e.g.
  // CurrentCopyItem of the Files menu: Upload or Download by the active side)
  // The proxy mirrors caption, icon, hints, enabled, checked and the shortcut
  // text of its target and triggers it; it never owns the shortcut.
  QAction * CreateProxy(QObject * Parent, QAction * Target);
  static void SetProxyTarget(QAction * Proxy, QAction * Target);
  static QAction * ProxyTarget(QAction * Proxy);

  // ---- Menus
  struct MenuItem
  {
    QString Action;              // action name; empty with Caption = submenu; "-" = separator
    QString Caption;             // submenu caption
    QList<MenuItem> Items;       // submenu items
    QMenu ** Created = nullptr;  // receives the created submenu
    QAction * Direct = nullptr;  // an action object instead of a name
    QMenu * DirectMenu = nullptr; // an existing submenu (added as is)
  };
  static MenuItem Item(const QString & Action) { MenuItem R; R.Action = Action; return R; }
  static MenuItem Separator() { return Item(QStringLiteral("-")); }
  static MenuItem Submenu(const QString & Caption, const QList<MenuItem> & Items, QMenu ** Created = nullptr)
  {
    MenuItem R;
    R.Caption = Caption;
    R.Items = Items;
    R.Created = Created;
    return R;
  }
  // A submenu titled and iconed after an action (TTBXSubmenuItem with an
  // Action: e.g. the Sites, Workspaces or New Tab submenus)
  static MenuItem ActionSubmenu(const QString & Action, const QList<MenuItem> & Items, QMenu ** Created = nullptr)
  {
    MenuItem R = Submenu(QString(), Items, Created);
    R.Action = Action;
    return R;
  }
  static MenuItem Object(QAction * Action) { MenuItem R; R.Direct = Action; return R; }
  static MenuItem ExistingMenu(QMenu * Menu) { MenuItem R; R.DirectMenu = Menu; return R; }
  void Populate(QMenu * Menu, const QList<MenuItem> & Items) const;

  // A toolbar button with a drop-down menu (TTBXSubmenuItem with DropdownCombo)
  static void AddDropDownButton(QToolBar * Bar, QAction * Action, QMenu * Menu);

  // 'Short|Long' hint (NonVisual.dfm Hint): tool tip = short part, status tip = long part
  static void SetHint(QAction * Action, const QString & Hint);
  // Caption without '&' and trailing '...' (TTBXCustomItem, StripHotkey/StripEllipsis)
  static QString PlainCaption(const QString & Caption);

  // Shortcut helpers for the Num keys (VK_ADD, VK_SUBTRACT, VK_MULTIPLY)
  static QKeySequence Num(Qt::Key Key, Qt::KeyboardModifiers Modifiers = Qt::NoModifier);

private:
  QWidget * FWindow;
  QHash<QString, QAction *> FActions;
  QHash<QAction *, ShortCutScope> FScopes;
  QList<QString> FOrder;
  bool FCurrentRemote = false;
  mutable QAction * FMissing = nullptr;

  void CreateActions();
  QAction * Add(const char * Name, const char * Caption, const char * Hint, ShortCutScope Scope,
    const QList<QKeySequence> & ShortCuts = {}, bool Checkable = false);
  void UpdateShortCutContext(QAction * Action);
};
