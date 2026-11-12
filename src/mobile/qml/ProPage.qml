// NixSCP Pro: sign in (in the browser), the NixSCP password that encrypts
// the synced settings (first device: created, with a recovery key; other
// devices: unlocked), sync status, the app lock (fingerprint, auto-lock),
// Teams, the password, sign out.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "pro"
    // needKey: the cloud has data (unlock) or not (create); -1 = not known yet
    property int cloudHasData: -1
    // the data is from NixSCP 1.2.3 or older: no password opens it, it can only be replaced
    property bool cloudOldFormat: false
    property string keyError: ""
    property bool keyBusy: false
    // The recovery key of a running setup is shown once, by this page: not left until it is
    function back() {
        if (page.keyBusy || page.keyChangeBusy) {
            app.toast(qsTr("Please wait: your recovery key is being created."))
            return true
        }
        return false
    }
    property bool recoveryMode: false
    property bool keyChangeBusy: false
    property var account: ({})
    readonly property var strengthNames: [qsTr("too weak"), qsTr("acceptable"), qsTr("good"), qsTr("strong")]

    Component.onCompleted: refresh()
    function refresh() {
        if (pro.state === "needKey") { cloudHasData = -1; pro.probe() }
        if (pro.signedIn) pro.fetchAccount()
    }

    Connections {
        target: pro
        function onSignInFinished(ok, error) {
            if (!ok && error) app.toast(error)
            else page.refresh()
        }
        function onProbed(ok, hasData, oldFormat, error) {
            page.cloudHasData = ok ? ((hasData && !oldFormat) ? 1 : 0) : -1
            page.cloudOldFormat = ok && oldFormat
            if (!ok) page.keyError = error
        }
        function onKeyFinished(ok, result) {
            page.keyBusy = false
            if (!ok) { page.keyError = result; return }
            page.keyError = ""
            password1.text = ""; password2.text = ""
            // a new key: its recovery key, once
            if (result.length > 0 && page.cloudHasData === 0) { recoveryDialog.key = result; recoveryDialog.open() }
            pro.fetchAccount()
        }
        function onPasswordChanged(ok, result) {
            page.keyChangeBusy = false
            if (!ok) { app.toast(result); return }
            app.toast(qsTr("The NixSCP password was changed. Turn fingerprint unlock on again with it."))
            if (lock.biometricEnabled) lock.disableBiometric()
            // a new data key comes with a new recovery key, shown once
            if (result.length > 0) { recoveryDialog.key = result; recoveryDialog.open() }
        }
        function onRecoveryKeyChanged(ok, result) {
            page.keyChangeBusy = false
            if (!ok) { app.toast(result); return }
            recoveryDialog.key = result
            recoveryDialog.open()
        }
        function onAccount(data) { page.account = data }
        function onChanged() { if (pro.state === "needKey" && page.cloudHasData < 0) pro.probe() }
    }
    Connections {
        target: lock
        function onBiometricFinished(ok, error) {
            if (ok) app.toast(qsTr("Fingerprint unlock is on."))
            else if (error) app.toast(error)
        }
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: if (!page.back()) page.StackView.view.pop() }
            Label { text: qsTr("NixSCP Pro"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
            ToolButton {
                visible: pro.hasKey
                icon.source: "icons/refresh.svg"
                icon.color: "white"
                Accessible.name: qsTr("Sync now")
                onClicked: pro.syncNow()
            }
        }
    }

    component Heading: Label {
        Layout.topMargin: 20
        font.bold: true
        color: Material.accent
    }
    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        font.pixelSize: 13
    }

    Flickable {
        anchors.fill: parent
        anchors.bottomMargin: app.keyboardHeight
        contentHeight: column.implicitHeight + 48
        clip: true
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: column
            x: 20
            y: 16
            width: parent.width - 40
            spacing: 8

            // ---- signed out ------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: !pro.signedIn
                spacing: 10
                Label { text: qsTr("Your sites on every device"); font.pixelSize: 20; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Hint {
                    text: qsTr("NixSCP Pro syncs your sites, passwords and settings between this phone and your computers, "
                               + "end-to-end encrypted with your NixSCP password: the server stores only data it cannot read. "
                               + "Teams share site folders with colleagues. Free during the beta.")
                }
                Button {
                    Layout.fillWidth: true
                    highlighted: true
                    text: pro.signingIn ? qsTr("Waiting for the browser…") : qsTr("Sign in")
                    enabled: !pro.signingIn
                    onClicked: pro.signIn()
                }
                Hint {
                    visible: pro.signingIn
                    text: qsTr("Sign in on the page that opened, then tap \"Back to NixSCP\".")
                }
                Button { Layout.fillWidth: true; flat: true; visible: pro.signingIn; text: qsTr("Cancel"); onClicked: pro.cancelSignIn() }
                // no browser opened: the link to copy into one
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    flat: true
                    visible: pro.signingIn && !linkText.visible
                    text: qsTr("No browser opened? Show the link")
                    onClicked: linkText.shown = true
                }
                TextArea {
                    id: linkText
                    property bool shown: false
                    Layout.fillWidth: true
                    visible: shown && pro.signingIn
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    font.pixelSize: 11
                    font.family: monoFont
                    text: pro.signInUrl
                }
                Button {
                    visible: linkText.visible && pro.signingIn
                    flat: true
                    text: qsTr("Copy the link")
                    onClicked: { linkText.selectAll(); linkText.copy(); linkText.deselect(); app.toast(qsTr("Copied.")) }
                }
                Button {
                    Layout.fillWidth: true
                    flat: true
                    text: qsTr("Create a free account")
                    onClicked: Qt.openUrlExternally(pro.registerUrl)
                }
            }

            // ---- signed in: the NixSCP password ----------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: pro.signedIn && pro.state === "needKey"
                spacing: 10
                Label { text: pro.email; opacity: 0.7 }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 18
                    font.bold: true
                    text: page.cloudHasData === 0 ? qsTr("Choose your NixSCP password")
                          : page.recoveryMode ? qsTr("Enter your recovery key")
                          : qsTr("Enter your NixSCP password")
                }
                BusyIndicator { visible: page.cloudHasData < 0 && page.keyError.length === 0; running: visible }
                Hint {
                    visible: page.cloudOldFormat
                    text: qsTr("Your cloud data was encrypted by NixSCP 1.2.3 or older. This version cannot read that format, so no "
                               + "password opens it here. You can start over: choose a new password, and this phone's settings "
                               + "replace the cloud data. Nothing is replaced until you confirm. Your other devices then need "
                               + "the new password.")
                }
                Hint {
                    visible: page.cloudHasData === 0
                    text: qsTr("It encrypts everything NixSCP syncs and the configuration on this phone. Nobody can reset it: "
                               + "without it (or the recovery key) the data cannot be decrypted. Whoever holds the encrypted data, "
                               + "the server included, can try guesses at it offline, so make it long and unique. "
                               + "Do not reuse your account password: the website sees that one when you sign in.")
                }
                Hint {
                    visible: page.cloudHasData === 1
                    text: page.recoveryMode ? qsTr("The key you saved when you set up NixSCP Pro on your first device.")
                                            : qsTr("The password you chose on your first device.")
                }
                TextField {
                    id: password1
                    Layout.fillWidth: true
                    visible: page.cloudHasData >= 0
                    echoMode: page.recoveryMode ? TextInput.Normal : TextInput.Password
                    placeholderText: page.recoveryMode ? qsTr("Recovery key") : qsTr("NixSCP password")
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                }
                Hint {
                    visible: page.cloudHasData === 0 && password1.text.length > 0
                    text: qsTr("Strength: %1").arg(page.strengthNames[pro.passwordScore(password1.text)])
                }
                TextField {
                    id: password2
                    Layout.fillWidth: true
                    visible: page.cloudHasData === 0
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Repeat the password")
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                }
                Button {
                    visible: page.cloudHasData === 0
                    flat: true
                    text: qsTr("Generate a passphrase")
                    onClicked: {
                        const passphrase = pro.generatePassphrase()
                        password1.echoMode = TextInput.Normal
                        password1.text = passphrase
                        password2.text = passphrase
                        page.keyError = qsTr("Save this passphrase in a password manager before you continue.")
                    }
                }
                CheckBox {
                    id: withRecovery
                    visible: page.cloudHasData === 0
                    checked: true
                    text: qsTr("Create a recovery key (recommended)")
                }
                Label {
                    Layout.fillWidth: true
                    visible: page.keyError.length > 0
                    wrapMode: Text.Wrap
                    color: Material.color(Material.Red)
                    text: page.keyError
                }
                Button {
                    Layout.fillWidth: true
                    highlighted: true
                    visible: page.cloudHasData >= 0
                    enabled: !page.keyBusy && password1.text.length > 0
                    text: page.keyBusy ? qsTr("Working…") : (page.cloudOldFormat ? qsTr("Replace the cloud data…")
                                                             : (page.cloudHasData === 0 ? qsTr("Set up") : qsTr("Unlock")))
                    onClicked: {
                        page.keyError = ""
                        if (page.cloudHasData === 0) {
                            const problem = pro.passwordProblem(password1.text)
                            if (problem.length > 0) { page.keyError = problem; return }
                            if (password1.text !== password2.text) { page.keyError = qsTr("The passwords do not match."); return }
                            if (page.cloudOldFormat) { replaceCloudDialog.open(); return }
                            page.keyBusy = true
                            pro.createKey(password1.text, withRecovery.checked)
                        } else {
                            page.keyBusy = true
                            pro.unlockKey(password1.text, page.recoveryMode)
                        }
                    }
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    flat: true
                    visible: page.cloudHasData === 1
                    text: page.recoveryMode ? qsTr("Use the password") : qsTr("Forgot it? Use the recovery key")
                    onClicked: { page.recoveryMode = !page.recoveryMode; password1.text = ""; page.keyError = "" }
                }
                Button { Layout.alignment: Qt.AlignHCenter; flat: true; text: qsTr("Sign out"); onClicked: pro.signOut() }
            }

            // ---- ready -------------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: pro.signedIn && pro.hasKey
                spacing: 6
                Label { text: pro.email; font.pixelSize: 17; font.bold: true }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: page.account.license
                          ? (page.account.license.active ? qsTr("NixSCP Pro: active") + (page.account.license.plan ? " (" + page.account.license.plan + ")" : "")
                                                         : qsTr("NixSCP Pro: not active"))
                          : ""
                    color: page.account.license && !page.account.license.active ? Material.color(Material.Red) : Material.foreground
                }
                Label {
                    objectName: "storageLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: !!page.account.storageText
                    text: qsTr("Cloud storage: %1").arg(page.account.storageText || "")
                          + (page.account.storageFull ? " · " + qsTr("remove what you no longer need: when it is full, nothing more is uploaded") : "")
                    color: page.account.storageFull ? Material.color(Material.Red) : Material.foreground
                    opacity: page.account.storageFull ? 1 : 0.75
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.75
                    text: pro.statusText + (pro.lastSync ? " · " + qsTr("last sync %1").arg(pro.lastSync) : "")
                }
                RowLayout {
                    Button { text: qsTr("Sync now"); onClicked: pro.syncNow() }
                    Button {
                        visible: pro.state === "conflict" || pro.state === "paused"
                        text: qsTr("Resolve conflict…")
                        onClicked: app.askConflict("", "")
                    }
                }

                Heading { text: qsTr("Lock") }
                Hint { text: qsTr("Your sites, passwords and SSH keys are encrypted on this phone. NixSCP asks for the password when it starts.") }
                SwitchDelegate {
                    Layout.fillWidth: true
                    text: qsTr("Unlock with fingerprint")
                    enabled: lock.biometricAvailable
                    checked: lock.biometricEnabled
                    onToggled: {
                        if (checked) { checked = false; biometricDialog.open() }
                        else lock.disableBiometric()
                    }
                }
                Hint {
                    visible: !lock.biometricAvailable
                    text: qsTr("No fingerprint is set up on this phone (Android settings, Security).")
                }
                ItemDelegate {
                    Layout.fillWidth: true
                    text: qsTr("Lock again after") + ": " + (lock.autoLockMinutes === 0 ? qsTr("only at start") : qsTr("%1 min in the background").arg(lock.autoLockMinutes))
                    onClicked: autoLockMenu.popup()
                    Menu {
                        id: autoLockMenu
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        Repeater {
                            model: [1, 5, 15, 60, 0]
                            delegate: MenuItem {
                                required property int modelData
                                text: modelData === 0 ? qsTr("Only at start") : (modelData === 1 ? qsTr("1 minute") : qsTr("%1 minutes").arg(modelData))
                                checkable: true
                                checked: lock.autoLockMinutes === modelData
                                onTriggered: lock.autoLockMinutes = modelData
                            }
                        }
                    }
                }
                Button { text: qsTr("Lock now"); flat: true; onClicked: lock.lockNow() }

                Heading { text: qsTr("Teams") }
                Hint { text: qsTr("Share site folders with colleagues, end-to-end encrypted. Team sites appear as \"Team: <name>\" folders.") }
                Button { text: qsTr("Teams…"); onClicked: page.StackView.view.push(Qt.resolvedUrl("TeamsPage.qml")) }

                Heading { text: qsTr("Account") }
                Button { text: qsTr("Change NixSCP password…"); flat: true; enabled: !page.keyChangeBusy; onClicked: changeDialog.open() }
                Button { text: qsTr("New recovery key…"); flat: true; enabled: !page.keyChangeBusy; onClicked: recoveryKeyDialog.open() }
                Button { text: qsTr("Manage account on nixscp.com"); flat: true; onClicked: Qt.openUrlExternally(pro.accountUrl) }
                Button { text: qsTr("Sign out"); flat: true; onClicked: signOutDialog.open() }
            }
        }
    }

    // ---- dialogs ----------------------------------------------------------
    Dialog {
        id: recoveryDialog
        property string key
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("Your recovery key")
        ColumnLayout {
            width: recoveryDialog.availableWidth
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Keep it somewhere safe, away from this phone (a password manager, or printed). It opens your data if you forget the NixSCP password. It is shown only now.")
            }
            TextArea {
                id: recoveryText
                Layout.fillWidth: true
                readOnly: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: monoFont
                font.pixelSize: 15
                text: recoveryDialog.key
            }
            RowLayout {
                Button { text: qsTr("Copy"); flat: true; onClicked: { recoveryText.selectAll(); recoveryText.copy(); recoveryText.deselect(); app.toast(qsTr("Copied.")) } }
                Button { text: qsTr("Share…"); flat: true; onClicked: androidFiles.shareText(recoveryDialog.key) }
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("I saved it"); flat: true; highlighted: true; onClicked: { recoveryDialog.key = ""; pro.recoveryKeySaved(); recoveryDialog.close() } }
        }
    }

    // only the user decides this: the server's word that the data is old is no reason
    Dialog {
        id: replaceCloudDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Replace the cloud data")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: { page.keyBusy = true; pro.replaceOldCloud(password1.text, withRecovery.checked) }
        Label {
            width: replaceCloudDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("Replace the data in the NixSCP cloud with this phone's settings? The old data was made by NixSCP 1.2.3 or older "
                       + "and will not be used again.")
        }
    }

    Dialog {
        id: biometricDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Unlock with fingerprint")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { bioPassword.text = ""; bioPassword.forceActiveFocus() }
        onAccepted: { lock.enableBiometric(bioPassword.text); bioPassword.text = "" }
        onRejected: bioPassword.text = ""
        ColumnLayout {
            width: biometricDialog.availableWidth
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Enter your NixSCP password once; then the fingerprint unlocks NixSCP instead.") }
            TextField {
                id: bioPassword
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("NixSCP password")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: biometricDialog.accept()
            }
        }
    }

    Dialog {
        id: changeDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Change NixSCP password")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { current1.text = ""; currentIsRecovery.checked = false; rotate1.checked = false; new1.text = ""; new2.text = ""; current1.forceActiveFocus() }
        onAccepted: {
            const problem = pro.passwordProblem(new1.text)
            if (problem.length > 0) app.toast(problem)
            else if (new1.text !== new2.text) app.toast(qsTr("The passwords do not match."))
            else { page.keyChangeBusy = true; pro.changePassword(current1.text, currentIsRecovery.checked, new1.text, rotate1.checked) }
            current1.text = ""; new1.text = ""; new2.text = ""
        }
        ColumnLayout {
            width: changeDialog.availableWidth
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; opacity: 0.7; text: qsTr("Your other devices keep working; they ask for the new password when they start next. The recovery key stays valid.") }
            TextField { id: current1; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: currentIsRecovery.checked ? qsTr("Recovery key") : qsTr("Current password"); inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText }
            CheckBox { id: currentIsRecovery; text: qsTr("This is my recovery key") }
            TextField { id: new1; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("New password"); inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText }
            Hint { visible: new1.text.length > 0; text: qsTr("Strength: %1").arg(page.strengthNames[pro.passwordScore(new1.text)]) }
            TextField { id: new2; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("Repeat it"); inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText }
            CheckBox { id: rotate1; text: qsTr("Also replace the data key and the recovery key") }
            Hint { text: qsTr("Do this if a password or recovery key may have leaked: the old one otherwise still opens the data key from any old copy of your encrypted data. Your other devices then ask for the new password once.") }
        }
    }

    Dialog {
        id: recoveryKeyDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("New recovery key")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { current2.text = ""; current2IsRecovery.checked = false; rotate2.checked = true; current2.forceActiveFocus() }
        onAccepted: {
            page.keyChangeBusy = true
            pro.newRecoveryKey(current2.text, current2IsRecovery.checked, rotate2.checked)
            current2.text = ""
        }
        ColumnLayout {
            width: recoveryKeyDialog.availableWidth
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; opacity: 0.7; text: qsTr("A new recovery key, shown once, replaces the old one. If the old key may have leaked, keep the data key replaced: a new recovery key alone leaves the old one able to open the data key from any old copy of your encrypted data.") }
            TextField { id: current2; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: current2IsRecovery.checked ? qsTr("Recovery key") : qsTr("Current password"); inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText }
            CheckBox { id: current2IsRecovery; text: qsTr("This is my recovery key") }
            CheckBox { id: rotate2; text: qsTr("Also replace the data key") }
        }
    }

    Dialog {
        id: signOutDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Sign out?")
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: signOutDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("Syncing stops on this phone. The sites stay here, still encrypted: NixSCP keeps asking for your NixSCP password (or the fingerprint) when it starts. Your data stays in the cloud.")
        }
        onAccepted: pro.signOut()
    }
}
