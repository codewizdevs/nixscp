// The remote files of the open session: tap a folder to enter it, tap a file
// for its actions (download, rename, delete); + uploads or creates a folder.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

Page {
    id: page
    objectName: "files"
    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/close.svg"; icon.color: "white"; Accessible.name: qsTr("Close"); onClicked: session.close() }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { text: session.site; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                Label { text: session.directory; font.pixelSize: 12; opacity: 0.85; elide: Text.ElideLeft; Layout.fillWidth: true }
            }
            ToolButton { icon.source: "icons/up.svg"; icon.color: "white"; Accessible.name: qsTr("Parent folder"); enabled: session.directory !== "/" && !session.busy; onClicked: session.enter("..") }
            ToolButton { icon.source: "icons/refresh.svg"; icon.color: "white"; Accessible.name: qsTr("Refresh"); enabled: !session.busy; onClicked: session.refresh() }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        model: session.files
        clip: true
        delegate: ItemDelegate {
            required property string name
            required property bool isDir
            required property bool isLink
            required property string size
            required property string date
            required property string rights
            width: ListView.view.width
            enabled: !session.busy
            contentItem: RowLayout {
                spacing: 12
                Image { source: isDir ? "icons/folder.svg" : "icons/file.svg"; sourceSize: Qt.size(28, 28) }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: name + (isLink ? " ↗" : ""); font.pixelSize: 15; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label {
                        text: (isDir ? "" : size + " · ") + date + " · " + rights
                        font.pixelSize: 12
                        opacity: 0.6
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
            onClicked: isDir ? session.enter(name) : actions.show(name, false)
            onPressAndHold: actions.show(name, isDir)
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    Label {
        anchors.centerIn: parent
        text: qsTr("Empty folder")
        opacity: 0.6
        visible: list.count === 0 && !session.busy
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: busyRow.implicitHeight + 16
        color: Material.backgroundColor
        visible: session.busy
        RowLayout {
            id: busyRow
            anchors.centerIn: parent
            BusyIndicator { running: session.busy; Layout.preferredWidth: 32; Layout.preferredHeight: 32 }
            Label { text: session.status }
        }
    }

    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 64
        height: 64
        icon.source: "icons/add.svg"
        Accessible.name: qsTr("Add")
        icon.width: 28
        icon.height: 28
        highlighted: true
        visible: !session.busy
        onClicked: addMenu.open()
        Menu {
            id: addMenu
            y: -height
            MenuItem { text: qsTr("Upload files…"); onTriggered: uploadDialog.open() }
            MenuItem { text: qsTr("New folder…"); onTriggered: { nameDialog.mode = "folder"; nameDialog.open() } }
        }
    }

    Menu {
        id: actions
        property string file
        property bool isDir
        function show(name, dir) { file = name; isDir = dir; popup() }
        MenuItem { text: actions.file; enabled: false }
        MenuItem { text: qsTr("Open"); visible: !actions.isDir; height: visible ? implicitHeight : 0; onTriggered: session.openFile(actions.file) }
        MenuItem { text: qsTr("Share…"); visible: !actions.isDir; height: visible ? implicitHeight : 0; onTriggered: session.openFile(actions.file, true) }
        MenuItem { text: qsTr("Download"); visible: !actions.isDir; height: visible ? implicitHeight : 0; onTriggered: session.download([actions.file]) }
        MenuItem { text: qsTr("Rename…"); onTriggered: { nameDialog.mode = "rename"; nameDialog.original = actions.file; nameDialog.open() } }
        MenuItem { text: qsTr("Delete…"); onTriggered: { deleteDialog.file = actions.file; deleteDialog.open() } }
    }

    Dialog {
        id: nameDialog
        property string mode: "folder"
        property string original
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: mode === "rename" ? qsTr("Rename") : qsTr("New folder")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { field.text = mode === "rename" ? original : ""; field.forceActiveFocus(); field.selectAll() }
        onAccepted: mode === "rename" ? session.rename(original, field.text) : session.createFolder(field.text)
        TextField {
            id: field
            width: parent.width
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            onAccepted: nameDialog.accept()
        }
    }

    Dialog {
        id: deleteDialog
        property string file
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Delete?")
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label { width: parent.width; wrapMode: Text.Wrap; text: deleteDialog.file }
        onAccepted: session.remove([file])
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Files to upload")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++)
                paths.push(selectedFiles[i].toString())
            session.upload(paths)
        }
    }
}
