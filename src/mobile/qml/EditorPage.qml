// The built-in editor for a remote text file (downloaded first): Save
// writes it and uploads it back; Back asks about unsaved changes.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    objectName: "editor"
    // the session the file belongs to (any tab)
    property var owner: session
    property string localPath
    property string remotePath
    property bool modified: false
    property bool wrap: true
    // Android's keyboard height (Qt's on Android 9/10)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    Component.onCompleted: {
        var file = owner.readText(localPath)
        if (file.error) {
            app.toast(file.error)
            Qt.callLater(function() { page.StackView.view.pop() })
            return
        }
        area.text = file.text
        modified = false
    }

    function save() {
        owner.saveText(localPath, area.text)
        modified = false
    }
    // Back: asks first when there are unsaved changes
    function back() {
        if (!modified)
            return false
        unsavedDialog.open()
        return true
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton {
                icon.source: "icons/back.svg"
                icon.color: "white"
                Accessible.name: qsTr("Back")
                onClicked: if (!page.back()) page.StackView.view.pop()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: (page.modified ? "• " : "") + page.remotePath.substring(page.remotePath.lastIndexOf("/") + 1)
                    font.pixelSize: 16
                    font.bold: true
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
                Label { text: page.owner.site; font.pixelSize: 12; opacity: 0.85; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            ToolButton {
                icon.source: "icons/wrap.svg"
                icon.color: "white"
                checkable: true
                checked: page.wrap
                Accessible.name: qsTr("Wrap lines")
                onClicked: page.wrap = !page.wrap
            }
            ToolButton {
                icon.source: "icons/save.svg"
                icon.color: "white"
                Accessible.name: qsTr("Save")
                enabled: page.modified && !page.owner.busy
                onClicked: page.save()
            }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        bottomMargin: page.keyboardHeight
        clip: true
        flickableDirection: page.wrap ? Flickable.VerticalFlick : Flickable.HorizontalAndVerticalFlick
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar { visible: !page.wrap }

        TextArea.flickable: TextArea {
            id: area
            font.family: monoFont
            font.pixelSize: 14
            wrapMode: page.wrap ? TextEdit.Wrap : TextEdit.NoWrap
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            selectByMouse: true
            persistentSelection: true
            background: null
            leftPadding: 12
            rightPadding: 12
            onTextChanged: page.modified = true
        }
    }

    // the upload of a save
    footer: Pane {
        visible: page.owner.busy
        padding: 8
        RowLayout {
            anchors.fill: parent
            BusyIndicator { running: page.owner.busy; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
            Label { text: page.owner.status; Layout.fillWidth: true; elide: Text.ElideRight }
        }
    }

    Dialog {
        id: unsavedDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Save the changes?")
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: unsavedDialog.close() }
            Button { text: qsTr("Discard"); flat: true; onClicked: { unsavedDialog.close(); page.modified = false; page.StackView.view.pop() } }
            Button { text: qsTr("Save"); flat: true; highlighted: true; onClicked: { unsavedDialog.close(); page.save(); page.StackView.view.pop() } }
        }
    }
}
