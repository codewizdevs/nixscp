// The phone's SSH keys: generate one (Ed25519 or RSA), pick one from the
// phone's files (.ppk or OpenSSH); tap a key for its public key (copy,
// share, install on the connected server) or to delete it.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

Page {
    id: page
    objectName: "keys"
    // a key was added (the site page chooses it)
    signal added(string path)

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            Label { text: qsTr("SSH keys"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        model: keys.keys
        bottomMargin: 96
        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            text: modelData.name
            icon.source: "icons/key.svg"
            onClicked: keyDialog.show(modelData.name)
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 64
        visible: list.count === 0
        spacing: 8
        Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 18; text: qsTr("No keys yet") }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("Generate a key with +, or pick a private key (.ppk or OpenSSH) from the phone's files.")
        }
    }

    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 64
        height: 64
        icon.source: "icons/add.svg"
        icon.width: 28
        icon.height: 28
        highlighted: true
        Accessible.name: qsTr("Add a key")
        onClicked: addMenu.open()
        Menu {
            // not under the status or navigation bar (popups cover the whole window)
            topMargin: insets.top + 4
            bottomMargin: insets.bottom + 4
            id: addMenu
            y: -height
            MenuItem { text: qsTr("Generate a key…"); onTriggered: generateDialog.open() }
            MenuItem { text: qsTr("Pick a key file…"); onTriggered: pickDialog.open() }
        }
    }

    FileDialog {
        id: pickDialog
        title: qsTr("Private key")
        onAccepted: {
            var name = keys.importKey(selectedFile.toString())
            if (name) { app.toast(qsTr("Key %1 added.").arg(name)); page.added(keys.path(name)) }
            else app.toast(keys.lastError())
        }
    }

    Dialog {
        id: generateDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Generate a key")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { keyName.text = ""; passphrase.text = ""; keyType.currentIndex = 0; keyName.forceActiveFocus() }
        onAccepted: {
            keys.generate(keyName.text, keyType.currentIndex === 0 ? "ed25519" : "rsa", passphrase.text)
            passphrase.text = ""
        }
        ColumnLayout {
            width: generateDialog.availableWidth
            Label { text: qsTr("Name"); opacity: 0.7 }
            TextField { id: keyName; Layout.fillWidth: true; placeholderText: "id_ed25519"; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText }
            Label { text: qsTr("Type"); opacity: 0.7 }
            ComboBox { id: keyType; Layout.fillWidth: true; model: ["Ed25519", "RSA 4096"] }
            Label { text: qsTr("Passphrase (optional)"); opacity: 0.7 }
            TextField { id: passphrase; Layout.fillWidth: true; echoMode: TextInput.Password; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText }
        }
    }

    Connections {
        target: keys
        function onGenerated(name, error) {
            if (name) { keyDialog.show(name); page.added(keys.path(name)) }
            else app.toast(error)
        }
    }

    // generating takes a while (RSA 4096)
    Rectangle {
        anchors.fill: parent
        color: "#66000000"
        visible: keys.generating
        MouseArea { anchors.fill: parent }
        RowLayout {
            anchors.centerIn: parent
            spacing: 12
            BusyIndicator { running: keys.generating; Layout.preferredWidth: 40; Layout.preferredHeight: 40 }
            Label { text: qsTr("Generating the key…"); color: "white" }
        }
    }

    // a key's public part, and what to do with it
    Dialog {
        id: keyDialog
        property string name
        property string publicKey
        function show(n) { name = n; publicKey = keys.publicKey(n); open() }
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 520)
        modal: true
        title: name
        ColumnLayout {
            width: keyDialog.availableWidth
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 13
                text: keyDialog.publicKey ? qsTr("Public key, for ~/.ssh/authorized_keys on the server:")
                                          : qsTr("The public key cannot be shown without the key's passphrase.")
            }
            TextArea {
                id: publicText
                Layout.fillWidth: true
                visible: keyDialog.publicKey.length > 0
                readOnly: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: monoFont
                font.pixelSize: 12
                text: keyDialog.publicKey
            }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    text: qsTr("Copy")
                    flat: true
                    visible: keyDialog.publicKey.length > 0
                    onClicked: { publicText.selectAll(); publicText.copy(); publicText.deselect(); app.toast(qsTr("Public key copied.")) }
                }
                Button {
                    text: qsTr("Share…")
                    flat: true
                    visible: keyDialog.publicKey.length > 0
                    onClicked: androidFiles.shareText(keyDialog.publicKey)
                }
                Button {
                    text: qsTr("Install on %1").arg(session.site)
                    flat: true
                    visible: keyDialog.publicKey.length > 0 && session.canRunCommands
                    onClicked: { session.installPublicKey(keyDialog.publicKey); keyDialog.close() }
                }
                Button {
                    text: qsTr("Delete…")
                    flat: true
                    Material.foreground: Material.color(Material.Red)
                    onClicked: deleteDialog.open()
                }
            }
        }
        standardButtons: Dialog.Close
    }

    Dialog {
        id: deleteDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Delete the key?")
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: deleteDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("%1 is removed from the phone; sites using it ask for a password instead.").arg(keyDialog.name)
        }
        onAccepted: { keys.remove(keyDialog.name); keyDialog.close() }
    }
}
