// The phone's settings: connections (timeout, keepalive), the editor for
// remote files, files (sort, hidden files, archive format), confirmations,
// the theme, and the SSH keys.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "settings"

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            Label { text: qsTr("Settings"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
        }
    }

    component Heading: Label {
        Layout.topMargin: 20
        Layout.leftMargin: 16
        font.bold: true
        color: Material.accent
    }
    component Hint: Label {
        Layout.fillWidth: true
        Layout.leftMargin: 16
        Layout.rightMargin: 16
        wrapMode: Text.Wrap
        opacity: 0.65
        font.pixelSize: 12
    }
    // a setting: title and value on the left, a control on the right
    component Row: ItemDelegate {
        property string title
        property string value
        Layout.fillWidth: true
        contentItem: ColumnLayout {
            spacing: 2
            Label { text: title; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { text: value; opacity: 0.65; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight; visible: value.length > 0 }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 32
        clip: true
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 0

            // ---- NixSCP Pro ----
            Heading { text: qsTr("NixSCP Pro") }
            Row {
                title: pro.signedIn ? pro.email : qsTr("Sync, encryption and Teams")
                value: !pro.signedIn ? qsTr("Not signed in") : pro.statusText
                onClicked: page.StackView.view.push(Qt.resolvedUrl("ProPage.qml"))
            }

            // ---- connections ----
            Heading { text: qsTr("Connections") }
            Row {
                title: qsTr("Connection timeout")
                value: qsTr("%1 seconds").arg(settings.timeout)
                onClicked: numberDialog.ask(qsTr("Connection timeout (seconds)"), settings.timeout, 5, 600, function(v) { settings.timeout = v })
            }
            Row {
                title: qsTr("Keepalive")
                value: settings.keepalive > 0 ? qsTr("Every %1 seconds").arg(settings.keepalive) : qsTr("Off")
                onClicked: numberDialog.ask(qsTr("Keepalive every (seconds, 0 = off)"), settings.keepalive, 0, 3600, function(v) { settings.keepalive = v })
            }
            Hint { text: qsTr("For every site opened on this phone. Keepalives stop mobile networks and routers from dropping an idle connection.") }

            // ---- editing ----
            Heading { text: qsTr("Editing remote files") }
            Row {
                title: qsTr("Editor")
                value: settings.editor === "builtin" ? qsTr("Built-in editor")
                     : settings.editor === "ask" ? qsTr("Ask every time")
                     : settings.editorName || settings.editor
                onClicked: { editorMenu.apps = androidFiles.editors(); editorMenu.popup() }
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: editorMenu
                    property var apps: []
                    MenuItem { text: qsTr("Built-in editor"); onTriggered: { settings.editor = "builtin"; settings.editorName = "" } }
                    MenuItem { text: qsTr("Ask every time"); onTriggered: { settings.editor = "ask"; settings.editorName = "" } }
                    Repeater {
                        model: editorMenu.apps
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: { settings.editor = modelData.package; settings.editorName = modelData.name }
                        }
                    }
                }
            }
            Hint { text: qsTr("Edit opens the file in it; saved changes are uploaded back to the server.") }

            // ---- files ----
            Heading { text: qsTr("Files") }
            Row {
                title: qsTr("Sort by")
                value: [qsTr("Name"), qsTr("Date"), qsTr("Size")][settings.sortBy] + (settings.descending ? qsTr(", reversed") : "")
                onClicked: sortMenu.popup()
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: sortMenu
                    MenuItem { text: qsTr("Name"); checkable: true; checked: settings.sortBy === 0; onTriggered: settings.sortBy = 0 }
                    MenuItem { text: qsTr("Date"); checkable: true; checked: settings.sortBy === 1; onTriggered: settings.sortBy = 1 }
                    MenuItem { text: qsTr("Size"); checkable: true; checked: settings.sortBy === 2; onTriggered: settings.sortBy = 2 }
                    MenuItem { text: qsTr("Reverse order"); checkable: true; checked: settings.descending; onTriggered: settings.descending = !settings.descending }
                }
            }
            SwitchDelegate {
                Layout.fillWidth: true
                text: qsTr("Thumbnails of server images")
                checked: settings.thumbnails
                onToggled: settings.thumbnails = checked
            }
            Hint { text: qsTr("Images up to 3 MB are fetched on a second connection while you browse.") }
            SwitchDelegate {
                Layout.fillWidth: true
                text: qsTr("Show hidden files")
                checked: settings.showHidden
                onToggled: settings.showHidden = checked
            }
            Row {
                title: qsTr("Archive format")
                value: settings.archiveFormat === "zip" ? qsTr(".zip (opens anywhere)") : qsTr(".tar.gz (keeps permissions)")
                onClicked: archiveMenu.popup()
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: archiveMenu
                    MenuItem { text: ".tar.gz"; checkable: true; checked: settings.archiveFormat !== "zip"; onTriggered: settings.archiveFormat = "tar.gz" }
                    MenuItem { text: ".zip"; checkable: true; checked: settings.archiveFormat === "zip"; onTriggered: settings.archiveFormat = "zip" }
                }
            }
            Hint { text: qsTr("\"Download as archive\" packs the selection on the server (tar or zip) and saves it to Download. FTP, WebDAV and S3 servers cannot run tar: their files are packed on the phone as .zip.") }

            // ---- confirmations ----
            Heading { text: qsTr("Confirmations") }
            SwitchDelegate {
                Layout.fillWidth: true
                text: qsTr("Confirm before deleting")
                checked: settings.confirmDelete
                onToggled: settings.confirmDelete = checked
            }
            SwitchDelegate {
                Layout.fillWidth: true
                text: qsTr("Confirm before overwriting files")
                checked: settings.confirmOverwrite
                onToggled: settings.confirmOverwrite = checked
            }

            // ---- appearance ----
            Heading { text: qsTr("Appearance") }
            Row {
                title: qsTr("Theme")
                value: [qsTr("As the system"), qsTr("Light"), qsTr("Dark")][settings.theme]
                onClicked: themeMenu.popup()
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: themeMenu
                    MenuItem { text: qsTr("As the system"); checkable: true; checked: settings.theme === 0; onTriggered: settings.theme = 0 }
                    MenuItem { text: qsTr("Light"); checkable: true; checked: settings.theme === 1; onTriggered: settings.theme = 1 }
                    MenuItem { text: qsTr("Dark"); checkable: true; checked: settings.theme === 2; onTriggered: settings.theme = 2 }
                }
            }

            // ---- keys ----
            Heading { text: qsTr("SSH keys") }
            Row {
                title: qsTr("SSH keys")
                value: keys.keys.length === 1 ? qsTr("1 key") : qsTr("%1 keys").arg(keys.keys.length)
                onClicked: page.StackView.view.push(Qt.resolvedUrl("KeysPage.qml"))
            }
            Hint { text: qsTr("Generate a key on the phone or pick one from its files, then choose it for a site.") }
        }
    }

    Dialog {
        id: numberDialog
        property var apply
        property int minimum: 0
        property int maximum: 3600
        function ask(title, value, min, max, done) {
            numberDialog.title = title
            minimum = min
            maximum = max
            apply = done
            numberField.text = value
            open()
        }
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 420)
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { numberField.forceActiveFocus(); numberField.selectAll() }
        onAccepted: {
            var v = parseInt(numberField.text)
            if (!isNaN(v)) apply(Math.max(minimum, Math.min(maximum, v)))
        }
        TextField {
            id: numberField
            width: numberDialog.availableWidth
            inputMethodHints: Qt.ImhDigitsOnly
            onAccepted: numberDialog.accept()
        }
    }
}
