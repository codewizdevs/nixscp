// A new site or the changes of one (original: its name), as a full page
// (fields stacked, Save in the top bar); the content scrolls above the
// on-screen keyboard. Editing keeps the site's other settings (key file,
// proxy...) and, with an empty password, the saved one. The site's note is
// shown under the settings and edited and saved on its own (NoteSection).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    // the site edited ("" = a new one)
    property string original: ""
    property bool hadPassword: false
    // a team's site the user only uses (member): changes are undone by the team
    readonly property bool teamReadOnly: siteTeam.uuid !== undefined && !siteTeam.canEdit
    // sharing started on this page; a shared copy leaves the site here, so the form stays
    property string teamAction: ""
    // the private key's path ("" = none)
    property string keyPath: ""
    // Android's keyboard height (Qt's on Android 9/10)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    Component.onCompleted: {
        if (original.length === 0)
            return
        var site = sites.siteData(original)
        name.text = site.name
        var index = protocol.find(site.protocol)
        protocol.currentIndex = index >= 0 ? index : 0
        host.text = site.host
        port.text = site.port > 0 ? site.port : ""
        user.text = site.user
        directory.text = site.directory
        hadPassword = site.hasPassword
        keyPath = site.keyFile
    }

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

    function save() {
        // a note being written is saved first (refused: it says why, the page stays)
        if (note.editing && !note.save())
            return
        if (host.text.trim().length === 0) {
            app.toast(qsTr("Enter the host name."))
            host.forceActiveFocus()
            return
        }
        Qt.inputMethod.hide()
        var failure = sites.saveSite(original, name.text, protocol.currentText, host.text, parseInt(port.text) || 0,
                                     user.text, password.text, hadPassword && !forget.checked, directory.text, keyPath)
        if (failure.length === 0)
            StackView.view.pop()
        else
            app.toast(failure)
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.leave() }
            Label { text: page.original ? qsTr("Edit site") : qsTr("New site"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
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

            Label {
                Layout.fillWidth: true
                visible: page.teamReadOnly
                wrapMode: Text.Wrap
                color: Material.color(Material.Orange)
                text: qsTr("This is a team site. Only the team's owners and admins change it; changes made here are undone at the next sync. Only the key you choose stays (it is this phone's).")
            }
            Label { text: qsTr("Protocol"); opacity: 0.7 }
            ComboBox {
                id: protocol
                Layout.fillWidth: true
                model: ["SFTP", "SCP", "FTP", "FTPS", "WebDAV", "WebDAVS", "S3"]
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
                placeholderText: page.hadPassword && !forget.checked ? qsTr("saved (empty keeps it)") : qsTr("ask when connecting")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: directory.forceActiveFocus()
            }
            CheckBox {
                id: forget
                visible: page.hadPassword
                text: qsTr("Forget the saved password")
            }
            Label { text: qsTr("Private key"); opacity: 0.7; visible: keyRow.visible }
            ItemDelegate {
                id: keyRow
                Layout.fillWidth: true
                visible: protocol.currentText === "SFTP" || protocol.currentText === "SCP"
                icon.source: "icons/key.svg"
                text: page.keyPath ? (keys.nameOf(page.keyPath) || page.keyPath) : qsTr("None (password)")
                onClicked: keyMenu.popup()
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: keyMenu
                    MenuItem { text: qsTr("None (password)"); onTriggered: page.keyPath = "" }
                    // inserted after "None" (Repeater items would go to the end)
                    Instantiator {
                        model: keys.keys
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: page.keyPath = modelData.path
                        }
                        onObjectAdded: function(index, object) { keyMenu.insertItem(index + 1, object) }
                        onObjectRemoved: function(index, object) { keyMenu.removeItem(object) }
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Add a key…")
                        onTriggered: {
                            var keysPage = page.StackView.view.push(Qt.resolvedUrl("KeysPage.qml"))
                            keysPage.added.connect(function(path) { page.keyPath = path })
                        }
                    }
                }
            }
            Label { text: qsTr("Remote folder"); opacity: 0.7 }
            TextField {
                id: directory
                Layout.fillWidth: true
                placeholderText: qsTr("home folder")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                onAccepted: name.forceActiveFocus()
            }
            Label { text: qsTr("Name"); opacity: 0.7 }
            TextField {
                id: name
                Layout.fillWidth: true
                placeholderText: qsTr("user@host")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onAccepted: page.save()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.6
                font.pixelSize: 12
                text: qsTr("Use / for folders, e.g. Clients/web1.")
            }

            // ---- the site's note (a stored site's) ----
            NoteSection {
                id: note
                Layout.fillWidth: true
                site: page.original
            }

            // ---- NixSCP Teams: share this site, or stop sharing it ----
            Label {
                visible: page.original.length > 0 && teams.available
                Layout.topMargin: 12
                text: qsTr("Team")
                font.bold: true
            }
            Label {
                visible: page.original.length > 0 && teams.available
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 13
                text: page.siteTeam.uuid ? qsTr("Shared with the team \"%1\".").arg(page.siteTeam.name)
                                         : qsTr("Share this site, with its stored password, with your colleagues in a team.")
            }
            Button {
                visible: page.original.length > 0 && teams.available && !page.siteTeam.uuid
                flat: true
                text: qsTr("Share with a team…")
                onClicked: {
                    var list = teams.teams.filter(function(t) { return t.canEdit && t.keyReady })
                    if (list.length === 0) { app.toast(qsTr("Create a team first (NixSCP Pro, Teams).")); return }
                    teamMenu.teamList = list
                    teamMenu.popup()
                }
                Menu {
                    id: teamMenu
                    property var teamList: []
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    Repeater {
                        model: teamMenu.teamList
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: { shareSiteDialog.team = modelData; shareSiteDialog.open() }
                        }
                    }
                }
            }
            Button {
                visible: page.original.length > 0 && teams.available && !!page.siteTeam.uuid && page.siteTeam.canEdit === true
                flat: true
                text: qsTr("Stop sharing…")
                onClicked: stopMenu.popup()
                Menu {
                    id: stopMenu
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    MenuItem { text: qsTr("Stop sharing, keep my copy"); onTriggered: { page.teamAction = "gone"; teams.unshare(page.siteTeam.uuid, [page.siteTeam.relative], true) } }
                    MenuItem { text: qsTr("Stop sharing and delete"); onTriggered: { page.teamAction = "gone"; teams.unshare(page.siteTeam.uuid, [page.siteTeam.relative], false) } }
                }
            }
        }
    }

    // the team of the site being edited ({} when it is not shared)
    readonly property var siteTeam: original.length > 0 && teams.teams ? teams.siteTeam(original) : ({})

    Dialog {
        id: shareSiteDialog
        property var team: ({})
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Share with \"%1\"?").arg(team.name || "")
        standardButtons: Dialog.Cancel | Dialog.Yes
        onOpened: shareKeep.checked = true
        ColumnLayout {
            width: shareSiteDialog.availableWidth
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Every member of the team gets this site with its stored password.")
            }
            CheckBox { id: shareKeep; text: qsTr("Keep my own copy as well") }
        }
        onAccepted: {
            page.teamAction = shareKeep.checked ? "stays" : "gone"
            teams.shareSite(team.uuid, page.original, "", !shareKeep.checked)
        }
    }

    Connections {
        target: teams
        function onFinished(op, ok, error, data) {
            // an operation of another page (the team's page) is not this one's
            if ((op !== "shareSite" && op !== "unshare") || page.teamAction.length === 0) return
            var action = page.teamAction
            page.teamAction = ""
            if (!ok) { if (error) app.toast(error); return }
            app.toast(op === "shareSite" ? qsTr("Shared with the team.") : qsTr("No longer shared."))
            // only when the site this page edits is gone, and only this page
            if (action === "gone" && page.StackView.view && page.StackView.view.currentItem === page) page.StackView.view.pop()
        }
    }

    // keep the focused field (in the note: the cursor) above the keyboard
    Connections {
        target: page.Window.window
        function onActiveFocusItemChanged() { page.ensureVisible() }
    }
    Connections {
        target: Qt.inputMethod
        function onCursorRectangleChanged() { if (note.editing) page.ensureVisible() }
    }
    function ensureVisible() {
        var item = page.Window.window ? page.Window.window.activeFocusItem : null
        if (!item || !form.contains(form.mapFromItem(item, 0, 0))) return
        // a field as a whole; the note's text area (taller than the screen) at its cursor
        var area = item.objectName === "noteText" ? item.cursorRectangle : Qt.rect(0, 0, item.width, item.height)
        var y = form.mapFromItem(item, 0, area.y).y + form.y
        var visible = flick.height - page.keyboardHeight
        if (y + area.height + 24 > flick.contentY + visible || y < flick.contentY)
            flick.contentY = Math.max(0, y - visible / 3)
    }
}
