// A note item (SessionData::NoteOnly): a note in the site list with no
// server behind it, so it never connects. Read-only until Edit, then Save or
// Cancel; the menu renames or deletes it.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
// IconImage: icons in the theme's colours
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    objectName: "note"
    // the note item's stored name ("folder/name")
    property string name: ""
    // a new note: written at once
    property bool startEditing: false
    readonly property string folder: name.lastIndexOf("/") > 0 ? name.substring(0, name.lastIndexOf("/")) : ""
    // Android's keyboard height (Qt's on Android 9/10)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    Component.onCompleted: if (startEditing) Qt.callLater(note.startEdit)

    // Back (Android's or the arrow): a changed note is not lost unasked
    function back() {
        if (!note.changed)
            return false
        note.confirmLeave(function() { page.StackView.view.pop() })
        return true
    }
    function leave() {
        if (!back()) {
            Qt.inputMethod.hide()
            page.StackView.view.pop()
        }
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.leave() }
            Label { text: page.name.split("/").pop(); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
            ToolButton {
                icon.source: "icons/more.svg"
                icon.color: "white"
                Accessible.name: qsTr("Menu")
                onClicked: noteMenu.open()
                Menu {
                    id: noteMenu
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    MenuItem { text: qsTr("Rename…"); onTriggered: renameDialog.open() }
                    MenuItem { text: qsTr("Delete…"); onTriggered: removeDialog.open() }
                }
            }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: form.implicitHeight + 32 + page.keyboardHeight
        clip: true
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: form
            x: 20
            y: 16
            width: flick.width - 40
            spacing: 6

            RowLayout {
                spacing: 6
                IconImage { source: "icons/note.svg"; sourceSize: Qt.size(16, 16); color: Material.accent }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.7
                    font.pixelSize: 13
                    text: page.folder ? qsTr("Note in %1").arg(page.folder) : qsTr("Note")
                }
            }
            NoteSection {
                id: note
                Layout.fillWidth: true
                site: page.name
                noteItem: true
            }
        }
    }

    Dialog {
        id: renameDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Rename note")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { nameField.text = page.name; nameField.forceActiveFocus(); nameField.select(page.folder ? page.folder.length + 1 : 0, nameField.text.length) }
        onAccepted: {
            var name = sites.cleanName(nameField.text)
            var failure = sites.renameSite(page.name, name)
            if (failure.length > 0)
                app.toast(failure)
            else
                page.name = name
        }
        ColumnLayout {
            width: renameDialog.availableWidth
            TextField {
                id: nameField
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhNoPredictiveText
                onAccepted: renameDialog.accept()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.6
                font.pixelSize: 12
                text: qsTr("Use / for folders, e.g. Office/Network.")
            }
        }
    }

    Dialog {
        id: removeDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Delete note?")
        Label { text: page.name; wrapMode: Text.Wrap; width: removeDialog.availableWidth }
        standardButtons: Dialog.Cancel | Dialog.Yes
        onAccepted: {
            sites.removeSite(page.name)
            page.StackView.view.pop()
        }
    }

    // keep the cursor above the keyboard
    Connections {
        target: page.Window.window
        function onActiveFocusItemChanged() { page.ensureVisible() }
    }
    Connections {
        target: Qt.inputMethod
        function onCursorRectangleChanged() { page.ensureVisible() }
    }
    function ensureVisible() {
        var item = page.Window.window ? page.Window.window.activeFocusItem : null
        if (!item || !form.contains(form.mapFromItem(item, 0, 0)))
            return
        var cursor = item.objectName === "noteText" ? item.cursorRectangle : Qt.rect(0, 0, item.width, item.height)
        var y = form.mapFromItem(item, 0, cursor.y).y + form.y
        var visible = flick.height - page.keyboardHeight
        if (y + cursor.height + 24 > flick.contentY + visible || y < flick.contentY)
            flick.contentY = Math.max(0, y - visible / 3)
    }
}
