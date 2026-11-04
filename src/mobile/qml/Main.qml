// NixSCP for Android: app frame, the pages (Sites, Site, Browser) and the
// sheets for the core's questions (host key, password / passphrase, queries).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: app
    visible: true
    width: 412
    height: 915
    title: "NixSCP"
    Material.theme: settings.theme === 1 ? Material.Light : (settings.theme === 2 ? Material.Dark : Material.System)
    Material.accent: "#f59e0b"
    Material.primary: "#1d4ed8"
    // edge to edge (Android 15): the pages between the status and navigation bars
    // folding, rotating: the bars may change
    onHeightChanged: insets.update()
    onWidthChanged: insets.update()
    background: Rectangle {
        color: Material.backgroundColor
        Rectangle { width: parent.width; height: insets.top; color: Material.primary }
    }

    // the browser is open (possibly under the console, editor or settings)
    function inBrowser() {
        for (var i = 0; i < stack.depth; i++) {
            var item = stack.get(i, StackView.DontLoad)
            if (item && item.objectName === "browser") return true
        }
        return false
    }

    // the keyboard's height (dialogs with a field stay above it)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    function toast(text) {
        toastLabel.text = text
        toastPopup.open()
        toastTimer.restart()
    }

    // Android's Back: the page first (selection, filter, up a folder, the
    // search), then back a page; on the sites it leaves the app
    onClosing: function(close) {
        var page = stack.currentItem
        if (page && page.back && page.back()) {
            close.accepted = false
            return
        }
        if (stack.depth <= 1)
            return
        close.accepted = false
        Qt.inputMethod.hide()
        stack.pop()
    }

    // Qt's idea of the keyboard kept in step with Android's (SystemInsets)
    Timer {
        interval: 200
        repeat: true
        running: Qt.inputMethod.visible
        onTriggered: insets.checkKeyboard()
        // closed: once more, for keyboard = 0
        onRunningChanged: if (!running) insets.checkKeyboard()
    }

    StackView {
        id: stack
        anchors.fill: parent
        anchors.topMargin: insets.top
        anchors.bottomMargin: insets.bottom
        initialItem: SitesPage {}
    }

    // a tab connected: its files (from a site list picked over the browser:
    // back to the browser); the last tab closed: back to the sites
    Connections {
        target: sessions
        function onEstablished(site) {
            sites.addRecent(site)
            if (!app.inBrowser()) {
                stack.pop(null)
                stack.push(Qt.resolvedUrl("BrowserPage.qml"))
            } else if (stack.currentItem.objectName === "sites") {
                stack.pop()
            }
        }
        function onSessionsChanged() {
            if (sessions.count === 0 && app.inBrowser())
                stack.pop(null)
        }
        function onError(site, message, details) {
            errorDialog.title = site && site !== session.site ? site : qsTr("Error")
            errorDialog.text = message
            errorDialog.details = details
            errorDialog.open()
        }
        // editing: the built-in editor, or the app chosen in Settings
        function onEditReady(owner, localPath, remotePath) {
            if (settings.editor === "builtin" || !androidFiles.edit(localPath, settings.editor))
                stack.push(Qt.resolvedUrl("EditorPage.qml"), { owner: owner, localPath: localPath, remotePath: remotePath })
        }
        function onArchived(path) {
            phone.refresh()
            app.toast(qsTr("Saved to %1").arg(path.substring(phone.root.length) || path))
        }
        function onNotice(message) { app.toast(message) }
        function onShared(paths) {
            if (!androidFiles.share(paths))
                app.toast(qsTr("These files cannot be shared."))
        }
        function onCopied(targetDir, count) {
            phone.refresh()
            // shown under the storage root: /Download, /DCIM/Camera...
            var place = targetDir.indexOf(phone.root) === 0 ? (targetDir.substring(phone.root.length) || "/") : targetDir
            app.toast(count === 1 ? qsTr("Downloaded to %1").arg(place)
                                  : qsTr("%1 files downloaded to %2").arg(count).arg(place))
        }
        function onUploaded(directory, count) {
            app.toast(count === 1 ? qsTr("Uploaded to %1").arg(directory)
                                  : qsTr("%1 files uploaded to %2").arg(count).arg(directory))
        }
        function onOpened(path, share) {
            if (share ? !androidFiles.share([path]) : !androidFiles.open(path))
                app.toast(share ? qsTr("This file cannot be shared.") : qsTr("No app on this phone opens this kind of file."))
        }
        function onTransferChanged() { app.updateKeepAlive() }
        function onPreviewReady(owner, localPath, name) {
            stack.push(Qt.resolvedUrl("PreviewPage.qml"), { localPath: localPath, name: name, kind: owner.files.previewKind(name) || "text" })
        }
        function onDownloaded(paths) {
            // into the public Downloads folder, where the Files app shows them
            var places = androidFiles.publish(paths)
            var saved = places.filter(function(p) { return p.length > 0 })
            if (saved.length === 0)
                app.toast(qsTr("Saved in NixSCP's own folder: %1").arg(paths[0]))
            else
                app.toast(saved.length === 1 ? qsTr("Saved to %1").arg(saved[0]) : qsTr("%1 files saved to Downloads/NixSCP").arg(saved.length))
        }
    }

    // the transfer queue's results; it keeps the transfer service running
    Connections {
        target: transfers
        function onChanged() { app.updateKeepAlive() }
        function onDownloaded(paths) {
            var places = androidFiles.publish(paths)
            var saved = places.filter(function(p) { return p.length > 0 })
            if (saved.length > 0)
                app.toast(saved.length === 1 ? qsTr("Saved to %1").arg(saved[0]) : qsTr("%1 files saved to Downloads/NixSCP").arg(saved.length))
        }
        function onCopied(targetDir, count) {
            phone.refresh()
            var place = targetDir.indexOf(phone.root) === 0 ? (targetDir.substring(phone.root.length) || "/") : targetDir
            app.toast(count === 1 ? qsTr("Downloaded to %1").arg(place) : qsTr("%1 files downloaded to %2").arg(count).arg(place))
        }
        function onUploaded(directory, count) {
            app.toast(count === 1 ? qsTr("Uploaded to %1").arg(directory) : qsTr("%1 files uploaded to %2").arg(count).arg(directory))
        }
        function onFailed(site, message, details) {
            errorDialog.title = qsTr("Transfer failed: %1").arg(site)
            errorDialog.text = message
            errorDialog.details = details
            errorDialog.open()
        }
    }
    // the queue's progress in the notification; a busy tab keeps it too
    function updateKeepAlive() {
        if (transfers.active > 0)
            androidFiles.keepAlive(true, transfers.summary, transfers.progress >= 0 ? Math.round(transfers.progress * 100) : -1)
        else
            androidFiles.keepAlive(sessions.transferring, sessions.transferStatus, -1)
    }

    // a site's home-screen shortcut: connect to it (after the unlock)
    property string pendingSite: ""
    Connections {
        target: androidFiles
        function onOpenSiteRequested(site) {
            if (lock.started && !lock.locked) sessions.open(site)
            else app.pendingSite = site
        }
    }
    Connections {
        target: lock
        function onChanged() {
            if (lock.started && !lock.locked && app.pendingSite.length > 0) {
                sessions.open(app.pendingSite)
                app.pendingSite = ""
            }
        }
    }

    // ---- NixSCP Pro: messages, conflicts, the license ----
    Connections {
        target: pro
        function onNotice(message) { app.toast(message) }
        function onConflict(device, time) { app.askConflict(device, time) }
        function onLicenseNotice(betaEnded) {
            licenseDialog.betaEnded = betaEnded
            licenseDialog.open()
        }
    }
    function askConflict(device, time) {
        conflictDialog.device = device
        conflictDialog.time = time
        conflictDialog.open()
    }
    Dialog {
        id: conflictDialog
        property string device
        property string time
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("Settings changed on two devices")
        Label {
            width: conflictDialog.availableWidth
            wrapMode: Text.Wrap
            text: (conflictDialog.device ? qsTr("%1 changed the settings in the cloud (%2), and they were changed on this phone too.").arg(conflictDialog.device).arg(conflictDialog.time)
                                         : qsTr("The settings were changed in the cloud and on this phone."))
                  + " " + qsTr("Which ones do you keep? The others are kept as a backup.")
        }
        footer: Flow {
            padding: 8
            spacing: 4
            layoutDirection: Qt.RightToLeft
            Button { text: qsTr("Use the cloud's"); flat: true; highlighted: true; onClicked: { pro.resolveConflict(0); conflictDialog.close() } }
            Button { text: qsTr("Keep this phone's"); flat: true; onClicked: { pro.resolveConflict(1); conflictDialog.close() } }
            Button { text: qsTr("Decide later"); flat: true; onClicked: { pro.resolveConflict(2); conflictDialog.close() } }
        }
    }
    Dialog {
        id: licenseDialog
        property bool betaEnded: false
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: "NixSCP Pro"
        Label {
            width: licenseDialog.availableWidth
            wrapMode: Text.Wrap
            text: licenseDialog.betaEnded
                  ? qsTr("The NixSCP Pro beta has ended. Your synced settings stay in the cloud and still download; changes on this phone upload again with NixSCP Pro.")
                  : qsTr("Your NixSCP Pro license is not active, so changes on this phone are no longer uploaded. Nothing is lost: they upload as soon as it is renewed.")
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Later"); flat: true; onClicked: licenseDialog.close() }
            Button { text: licenseDialog.betaEnded ? qsTr("Get NixSCP Pro") : qsTr("Renew"); flat: true; highlighted: true; onClicked: { Qt.openUrlExternally(pro.accountUrl); licenseDialog.close() } }
        }
    }

    // ---- the lock of the encrypted configuration, over everything ----
    LockPage {
        anchors.fill: parent
        z: 1000
        visible: lock.locked
    }

    // the connection of the current tab was lost: the tab closes
    Connections {
        target: session
        function onStateChanged() {
            if (!session.connected && !session.busy && session.site.length > 0)
                sessions.closeCurrent()
        }
    }

    // Android cuts the connections of apps in the background (the system
    // file picker, the settings, another app): the first reconnect question
    // after the app was away is answered with Retry at once (the desktop
    // reconnects by itself too); later ones are asked
    property bool wasAway: false
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationSuspended || Qt.application.state === Qt.ApplicationHidden) {
                app.wasAway = true
                lock.background()
            }
            if (Qt.application.state === Qt.ApplicationActive) {
                // the system bars may have changed meanwhile (or settled after start)
                insets.update()
                // back from Android's "All files access" setting
                phone.checkAccess()
                // back from an editor: its changes go to the server
                sessions.checkEdits()
                // locked again after a while away; what changed on other devices
                lock.foreground()
                if (lock.started) pro.foreground()
            }
        }
    }
    function autoRetry(buttons) {
        if (!app.wasAway)
            return false
        for (var i = 0; i < buttons.length; i++) {
            if (buttons[i].caption === "Retry") {
                app.wasAway = false
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
                font.family: monoFont
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
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 480)
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
        y: parent.height - height - 48 - insets.bottom
        width: Math.min(parent.width - 32, 480)
        // a message only: taps elsewhere reach the page (not swallowed to close it)
        closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 8; color: "#323232" }
        Label { id: toastLabel; width: parent.width; wrapMode: Text.Wrap; color: "white" }
        Timer { id: toastTimer; interval: 3500; onTriggered: toastPopup.close() }
    }
}
