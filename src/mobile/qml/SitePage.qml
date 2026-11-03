// New site, as a full page (fields stacked, Save in the top bar); the content
// scrolls above the on-screen keyboard.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    readonly property real keyboardHeight: Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0

    function save() {
        if (host.text.trim().length === 0) {
            app.toast(qsTr("Enter the host name."))
            host.forceActiveFocus()
            return
        }
        Qt.inputMethod.hide()
        if (sites.addSite(name.text, protocol.currentText, host.text, parseInt(port.text) || 0, user.text, password.text))
            StackView.view.pop()
        else
            app.toast(qsTr("The site could not be saved."))
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: { Qt.inputMethod.hide(); page.StackView.view.pop() } }
            Label { text: qsTr("New site"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
            ToolButton { text: qsTr("Save"); font.bold: true; onClicked: page.save() }
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

            Label { text: qsTr("Protocol"); opacity: 0.7 }
            ComboBox {
                id: protocol
                Layout.fillWidth: true
                model: ["SFTP", "SCP", "FTP", "FTPS", "WebDAV", "WebDAVS"]
            }
            Label { text: qsTr("Host"); opacity: 0.7 }
            TextField {
                id: host
                Layout.fillWidth: true
                placeholderText: "example.com"
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onAccepted: port.forceActiveFocus()
            }
            Label { text: qsTr("Port"); opacity: 0.7 }
            TextField {
                id: port
                Layout.fillWidth: true
                placeholderText: qsTr("default")
                inputMethodHints: Qt.ImhDigitsOnly
                onAccepted: user.forceActiveFocus()
            }
            Label { text: qsTr("User"); opacity: 0.7 }
            TextField {
                id: user
                Layout.fillWidth: true
                placeholderText: qsTr("user name")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onAccepted: password.forceActiveFocus()
            }
            Label { text: qsTr("Password"); opacity: 0.7 }
            TextField {
                id: password
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("ask when connecting")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: name.forceActiveFocus()
            }
            Label { text: qsTr("Name"); opacity: 0.7 }
            TextField {
                id: name
                Layout.fillWidth: true
                placeholderText: qsTr("user@host")
                onAccepted: page.save()
            }
        }
    }

    // keep the focused field above the keyboard
    Connections {
        target: page.Window.window
        function onActiveFocusItemChanged() {
            var item = page.Window.window ? page.Window.window.activeFocusItem : null
            if (!item || !form.contains(form.mapFromItem(item, 0, 0))) return
            var y = form.mapFromItem(item, 0, 0).y + form.y
            var visible = flick.height - page.keyboardHeight
            if (y + item.height + 24 > flick.contentY + visible || y < flick.contentY)
                flick.contentY = Math.max(0, y - visible / 3)
        }
    }
}
