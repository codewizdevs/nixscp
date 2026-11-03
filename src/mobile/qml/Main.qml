// NixSCP for Android: app frame, the pages (Sites, Files) and the sheets for
// the core's questions (host key, password / passphrase, queries).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: app
    visible: true
    width: 412
    height: 915
    title: "NixSCP"
    Material.theme: Material.System
    Material.accent: "#f59e0b"
    Material.primary: "#1d4ed8"
    // edge to edge (Android 15): the pages between the status and navigation bars
    onHeightChanged: insets.update()
    background: Rectangle {
        color: Material.backgroundColor
        Rectangle { width: parent.width; height: insets.top; color: Material.primary }
    }

    function toast(text) {
        toastLabel.text = text
        toastPopup.open()
        toastTimer.restart()
    }

    // Android's Back: up a folder, then back to the sites; on the sites it leaves the app
    onClosing: function(close) {
        if (stack.depth <= 1)
            return
        close.accepted = false
        if (stack.currentItem && stack.currentItem.objectName === "files" && session.connected) {
            if (session.directory !== "/" && !session.busy)
                session.enter("..")
            else if (session.directory === "/")
                session.close()
        } else {
            Qt.inputMethod.hide()
            stack.pop()
        }
    }

    StackView {
        id: stack
        anchors.fill: parent
        anchors.topMargin: insets.top
        anchors.bottomMargin: insets.bottom
        initialItem: SitesPage {}
    }

    // the session opened: show its files; closed: back to the sites
    Connections {
        target: session
        function onStateChanged() {
            if (session.connected && stack.depth === 1)
                stack.push(Qt.resolvedUrl("FilesPage.qml"))
            else if (!session.connected && !session.busy && stack.depth > 1)
                stack.pop(null)
        }
        function onError(message, details) {
            errorDialog.text = message
            errorDialog.details = details
            errorDialog.open()
        }
        function onOpened(path, share) {
            if (share ? !androidFiles.share([path]) : !androidFiles.open(path))
                app.toast(share ? qsTr("This file cannot be shared.") : qsTr("No app on this phone opens this kind of file."))
        }
        function onStatusChanged() {
            // transfers go on in the background (foreground service)
            var transfer = /^(Downloading|Uploading|Opening)/.test(session.status)
            androidFiles.keepAlive(session.busy && transfer, session.status)
        }
        function onDownloaded(paths) {
            app.toast(paths.length === 1 ? qsTr("Saved to %1").arg(paths[0])
                                         : qsTr("%1 files saved to %2").arg(paths.length).arg(session.downloadFolder))
        }
    }

    // Android cuts the connections of apps in the background (e.g. while the
    // system file picker is open): a reconnect question soon after coming back
    // is answered with Retry at once, once per return
    property double resumedAt: 0
    property bool autoRetried: false
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive) {
                app.resumedAt = Date.now()
                app.autoRetried = false
            }
        }
    }
    function autoRetry(buttons) {
        if (app.autoRetried || Date.now() - app.resumedAt > 15000)
            return false
        for (var i = 0; i < buttons.length; i++) {
            if (buttons[i].caption === "Retry") {
                app.autoRetried = true
                ui.answerQuery(buttons[i].answer)
                return true
            }
        }
        return false
    }

    // ---- the core's questions -------------------------------------------
    Connections {
        target: ui
        function onHostKeyRequested(host, port, keyType, fingerprint, state) {
            hostKeyDialog.host = host + (port === 22 ? "" : ":" + port)
            hostKeyDialog.keyType = keyType
            hostKeyDialog.fingerprint = fingerprint
            hostKeyDialog.changed = state === 1
            hostKeyDialog.open()
        }
        function onPromptRequested(kind, name, instructions, prompts) {
            promptDialog.kind = kind
            promptDialog.instructions = instructions || name
            promptDialog.prompts = prompts
            promptDialog.open()
        }
        function onQueryRequested(query, moreMessages, buttons) {
            if (app.autoRetry(buttons))
                return
            queryDialog.text = query.replace(/\*\*/g, "")
            queryDialog.details = moreMessages
            queryDialog.buttons = buttons
            queryDialog.open()
        }
        function onBannerShown(sessionName, banner) {
            errorDialog.title = qsTr("Message from %1").arg(sessionName)
            errorDialog.text = banner
            errorDialog.details = ""
            errorDialog.open()
        }
    }

    Dialog {
        id: hostKeyDialog
        property string host
        property string keyType
        property string fingerprint
        property bool changed: false
        property bool answered: false
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: changed ? qsTr("Host key changed!") : qsTr("Unknown server")
        onOpened: answered = false
        onClosed: if (!answered) ui.answerHostKey(2)
        ColumnLayout {
            width: parent.width
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: hostKeyDialog.changed
                      ? qsTr("The key of %1 is not the one you accepted before. Someone may be intercepting the connection. Continue only if you know the key was changed.").arg(hostKeyDialog.host)
                      : qsTr("First connection to %1. Check that the key fingerprint matches the server.").arg(hostKeyDialog.host)
                color: hostKeyDialog.changed ? Material.color(Material.Red) : Material.foreground
            }
            Label { text: hostKeyDialog.keyType; font.bold: true }
            Label {
                Layout.fillWidth: true
                text: hostKeyDialog.fingerprint
                wrapMode: Text.WrapAnywhere
                font.family: "monospace"
                font.pixelSize: 13
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: { hostKeyDialog.answered = true; ui.answerHostKey(2); hostKeyDialog.close() } }
            Button { text: qsTr("Once"); flat: true; onClicked: { hostKeyDialog.answered = true; ui.answerHostKey(1); hostKeyDialog.close() } }
            Button { text: qsTr("Accept"); flat: true; highlighted: true; onClicked: { hostKeyDialog.answered = true; ui.answerHostKey(0); hostKeyDialog.close() } }
        }
    }

    Dialog {
        id: promptDialog
        property int kind: 0
        property string instructions
        property var prompts: []
        property bool answered: false
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        y: 80
        modal: true
        closePolicy: Popup.NoAutoClose
        title: kind === 1 ? qsTr("Key passphrase") : (kind === 3 ? qsTr("User name") : qsTr("Sign in"))
        onOpened: {
            answered = false
            // never show what was typed for an earlier prompt
            for (var i = 0; i < fields.count; i++)
                fields.itemAt(i).children[1].text = ""
            if (fields.count > 0) fields.itemAt(0).children[1].forceActiveFocus()
        }
        onClosed: {
            for (var i = 0; i < fields.count; i++)
                fields.itemAt(i).children[1].text = ""
            if (!answered) ui.answerPrompt(false, [])
        }
        function accept() {
            var values = []
            for (var i = 0; i < fields.count; i++)
                values.push(fields.itemAt(i).children[1].text)
            answered = true
            ui.answerPrompt(true, values)
            close()
        }
        ColumnLayout {
            width: parent.width
            spacing: 4
            Label {
                Layout.fillWidth: true
                text: promptDialog.instructions
                wrapMode: Text.Wrap
                visible: text.length > 0
            }
            Repeater {
                id: fields
                model: promptDialog.prompts
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: modelData.text.replace(/&(?=\S)/g, "").replace(/:\s*$/, ""); font.pixelSize: 13; opacity: 0.8 }
                    TextField {
                        Layout.fillWidth: true
                        echoMode: modelData.echo ? TextInput.Normal : TextInput.Password
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | (modelData.echo ? 0 : Qt.ImhSensitiveData)
                        onAccepted: promptDialog.accept()
                    }
                }
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: promptDialog.close() }
            Button { text: qsTr("OK"); flat: true; highlighted: true; onClicked: promptDialog.accept() }
        }
    }

    Dialog {
        id: queryDialog
        property string text
        property string details
        property var buttons: []
        property bool answered: false
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: "NixSCP"
        onOpened: answered = false
        onClosed: if (!answered) ui.answerQuery(0)
        ColumnLayout {
            width: parent.width
            Label { Layout.fillWidth: true; text: queryDialog.text; wrapMode: Text.Wrap }
            Label {
                Layout.fillWidth: true
                text: queryDialog.details
                wrapMode: Text.Wrap
                visible: text.length > 0
                opacity: 0.7
                font.pixelSize: 13
            }
        }
        footer: Flow {
            padding: 8
            spacing: 4
            layoutDirection: Qt.RightToLeft
            Repeater {
                model: queryDialog.buttons
                delegate: Button {
                    required property var modelData
                    required property int index
                    text: modelData.caption
                    flat: true
                    highlighted: index === 0
                    onClicked: { queryDialog.answered = true; ui.answerQuery(modelData.answer); queryDialog.close() }
                }
            }
        }
    }

    Dialog {
        id: errorDialog
        property string text
        property string details
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: qsTr("Error")
        onClosed: title = qsTr("Error")
        standardButtons: Dialog.Ok
        ColumnLayout {
            width: parent.width
            Label { Layout.fillWidth: true; text: errorDialog.text; wrapMode: Text.Wrap }
            Label {
                Layout.fillWidth: true
                text: errorDialog.details
                wrapMode: Text.Wrap
                visible: text.length > 0
                opacity: 0.7
                font.pixelSize: 13
            }
        }
    }

    Popup {
        id: toastPopup
        x: (parent.width - width) / 2
        y: parent.height - height - 48
        width: Math.min(parent.width - 32, 480)
        // a message only: taps elsewhere reach the page (not swallowed to close it)
        closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 8; color: "#323232" }
        Label { id: toastLabel; width: parent.width; wrapMode: Text.Wrap; color: "white" }
        Timer { id: toastTimer; interval: 3500; onTriggered: toastPopup.close() }
    }
}
