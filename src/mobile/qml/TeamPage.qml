// One team: what it shares (share single sites or folders, stop sharing),
// its members (role, remove) and invites (create, share the code, revoke);
// rename, delete or leave. Owners and admins manage it; members use its sites.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
// IconImage: icons in the theme's text colour
import QtQuick.Controls.impl
import QtQuick.Layouts

Page {
    id: page
    objectName: "team"
    property var team: ({})
    property var memberList: []
    property var inviteList: []
    property var sharedList: []
    readonly property bool canEdit: team.canEdit === true
    readonly property bool owner: team.role === "owner"

    Component.onCompleted: load()
    function load() {
        page.sharedList = teams.sharedSites(team.uuid)
        teams.members(team.uuid)
        if (canEdit) teams.invites(team.uuid)
    }

    Connections {
        target: teams
        function onFinished(op, ok, error, data) {
            if (!ok) { if (error) app.toast(error); return }
            // a late reply of another team's page
            if (op === "members") { if (data.uuid === page.team.uuid) page.memberList = data.list }
            else if (op === "invites") { if (data.uuid === page.team.uuid) page.inviteList = data.list }
            else if (op === "history") { if (data.uuid === page.team.uuid) { historyDialog.versions = data.list; historyDialog.open() } }
            else if (op === "invite") { codeDialog.code = data; codeDialog.open(); teams.invites(page.team.uuid) }
            else if (op === "revoke" || op === "role" || op === "removeMember") page.load()
            else if (op === "restore") { app.toast(qsTr("The version was restored.")); page.load() }
            else if (op === "resolve") page.load()
            else if (op === "share") { app.toast(qsTr("The folder is now shared with the team.")); page.sharedList = teams.sharedSites(page.team.uuid) }
            else if (op === "shareSite") { app.toast(qsTr("The site is now shared with the team.")); page.sharedList = teams.sharedSites(page.team.uuid) }
            else if (op === "unshare") { app.toast(qsTr("No longer shared.")); page.sharedList = teams.sharedSites(page.team.uuid) }
            else if (op === "rename") app.toast(qsTr("Renamed."))
            else if (op === "delete" || op === "leave" || op === "forget") page.StackView.view.pop()
        }
        function onChanged() {
            // the team's new name or role
            var list = teams.teams
            for (var i = 0; i < list.length; i++)
                if (list[i].uuid === page.team.uuid) page.team = list[i]
            page.load()
        }
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { text: page.team.name || ""; font.pixelSize: 17; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                Label { text: qsTr("You are %1").arg(page.team.role || ""); font.pixelSize: 12; opacity: 0.85 }
            }
            BusyIndicator { running: teams.busy; visible: running; Layout.preferredWidth: 28; Layout.preferredHeight: 28 }
            ToolButton {
                icon.source: "icons/more.svg"
                icon.color: "white"
                Accessible.name: qsTr("Menu")
                onClicked: teamMenu.popup()
                Menu {
                    id: teamMenu
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    MenuItem { text: qsTr("Rename…"); visible: page.canEdit; height: visible ? implicitHeight : 0; onTriggered: { nameDialog.open() } }
                    MenuItem { text: qsTr("Restore from history…"); visible: page.canEdit; height: visible ? implicitHeight : 0; onTriggered: teams.history(page.team.uuid) }
                    MenuItem { text: qsTr("Delete team…"); visible: page.owner; height: visible ? implicitHeight : 0; onTriggered: { confirmDialog.action = "delete"; confirmDialog.open() } }
                    MenuItem { text: qsTr("Leave team…"); visible: !page.owner; height: visible ? implicitHeight : 0; onTriggered: { confirmDialog.action = "leave"; confirmDialog.open() } }
                }
            }
        }
    }

    component Heading: Label {
        Layout.topMargin: 16
        Layout.leftMargin: 16
        font.bold: true
        color: Material.accent
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

            Label {
                Layout.fillWidth: true
                Layout.margins: 16
                visible: page.team.needsRotation === true
                wrapMode: Text.Wrap
                color: Material.color(Material.Orange)
                text: qsTr("A member was removed: the team key is being renewed by the next sync of an owner or admin.")
            }

            Label {
                Layout.fillWidth: true
                Layout.margins: 16
                visible: !!page.team.error
                wrapMode: Text.Wrap
                color: Material.color(Material.Red)
                text: page.team.error || ""
            }
            RowLayout {
                Layout.leftMargin: 8
                visible: !!page.team.error
                Button { flat: true; text: qsTr("Remove, keep the sites"); onClicked: { confirmDialog.action = "forgetKeep"; confirmDialog.open() } }
                Button { flat: true; text: qsTr("Remove with the sites"); onClicked: { confirmDialog.action = "forget"; confirmDialog.open() } }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                visible: !page.team.error && !!page.team.conflict
                wrapMode: Text.Wrap
                color: Material.color(Material.Orange)
                text: page.team.conflict || ""
            }
            RowLayout {
                Layout.leftMargin: 8
                visible: !page.team.error && !!page.team.conflict
                Button { flat: true; text: qsTr("Use the team's version"); onClicked: { confirmDialog.action = "useTeam"; confirmDialog.open() } }
                Button { flat: true; text: qsTr("Keep mine for everyone"); onClicked: { confirmDialog.action = "keepMine"; confirmDialog.open() } }
            }

            // ---- shared sites ----
            Heading { text: qsTr("Shared sites (%1)").arg(page.sharedList.length) }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 13
                text: (page.sharedList.length === 0
                       ? (page.canEdit ? qsTr("Nothing is shared yet. Share single sites or a whole folder; every member gets them.")
                                       : qsTr("Nothing is shared with this team yet."))
                       : qsTr("In the folder \"%1\" of the site list.").arg(page.team.folder || ""))
                      + (page.canEdit ? "" : " " + qsTr("Only owners and admins change them."))
            }
            Repeater {
                model: page.sharedList
                delegate: ItemDelegate {
                    required property string modelData
                    Layout.fillWidth: true
                    contentItem: RowLayout {
                        spacing: 12
                        // a shared note item: a note, not a server
                        IconImage {
                            source: sites.isNote((page.team.folder || "") + "/" + modelData) ? "icons/note.svg" : "icons/server.svg"
                            sourceSize: Qt.size(22, 22); color: Material.foreground; opacity: 0.7
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: modelData.split("/").pop(); elide: Text.ElideRight; Layout.fillWidth: true }
                            Label {
                                visible: modelData.indexOf("/") > 0
                                text: modelData.substring(0, modelData.lastIndexOf("/"))
                                font.pixelSize: 12; opacity: 0.6; elide: Text.ElideRight; Layout.fillWidth: true
                            }
                        }
                        ToolButton {
                            visible: page.canEdit
                            icon.source: "icons/more.svg"
                            Accessible.name: qsTr("More")
                            onClicked: { unshareMenu.site = modelData; unshareMenu.popup() }
                        }
                    }
                    onPressAndHold: if (page.canEdit) { unshareMenu.site = modelData; unshareMenu.popup() }
                }
            }
            Menu {
                id: unshareMenu
                property string site
                topMargin: insets.top + 4
                bottomMargin: insets.bottom + 4
                MenuItem { text: qsTr("Stop sharing, keep my copy"); onTriggered: { unshareDialog.site = unshareMenu.site; unshareDialog.keep = true; unshareDialog.open() } }
                MenuItem { text: qsTr("Stop sharing and delete"); onTriggered: { unshareDialog.site = unshareMenu.site; unshareDialog.keep = false; unshareDialog.open() } }
            }
            RowLayout {
                Layout.leftMargin: 8
                visible: page.canEdit
                Button {
                    flat: true
                    text: qsTr("Share a site…")
                    onClicked: { sitePicker.sites = teams.localSites(); sitePicker.open() }
                }
                Button {
                    flat: true
                    text: qsTr("Share a folder…")
                    onClicked: { folderMenu.folders = teams.localFolders(); folderMenu.popup() }
                    Menu {
                        id: folderMenu
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        property var folders: []
                        Repeater {
                            model: folderMenu.folders
                            delegate: MenuItem {
                                required property string modelData
                                text: modelData
                                onTriggered: { shareDialog.folder = modelData; shareDialog.open() }
                            }
                        }
                    }
                }
            }

            // ---- members ----
            Heading { text: qsTr("Members (%1)").arg(page.memberList.length) }
            Repeater {
                model: page.memberList
                delegate: ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    contentItem: ColumnLayout {
                        spacing: 0
                        Label { text: (modelData.email || modelData.user_uuid) + (modelData.email === pro.email ? " " + qsTr("(you)") : ""); elide: Text.ElideRight; Layout.fillWidth: true }
                        Label { text: modelData.role + (modelData.gone ? " · " + qsTr("account deleted") : "") + (modelData.unusable ? " · " + qsTr("unusable key") : ""); opacity: 0.65; font.pixelSize: 13 }
                        Label { text: modelData.fingerprint || ""; opacity: 0.65; font.pixelSize: 11; font.family: monoFont; elide: Text.ElideRight; Layout.fillWidth: true }
                    }
                    // the owner manages everyone but themselves, admins the plain members
                    onClicked: if ((page.owner || (page.canEdit && modelData.role === "member")) && modelData.role !== "owner" && modelData.email !== pro.email) { memberMenu.member = modelData; memberMenu.popup() }
                }
            }
            Menu {
                id: memberMenu
                topMargin: insets.top + 4
                bottomMargin: insets.bottom + 4
                property var member: ({})
                MenuItem { text: memberMenu.member.email || ""; enabled: false }
                MenuItem { text: qsTr("Make admin"); visible: page.owner && memberMenu.member.role === "member"; height: visible ? implicitHeight : 0; onTriggered: teams.setRole(page.team.uuid, memberMenu.member.user_uuid, "admin") }
                MenuItem { text: qsTr("Make member"); visible: page.owner && memberMenu.member.role === "admin"; height: visible ? implicitHeight : 0; onTriggered: teams.setRole(page.team.uuid, memberMenu.member.user_uuid, "member") }
                MenuItem { text: qsTr("Remove from the team…"); onTriggered: { confirmDialog.action = "removeMember"; confirmDialog.member = memberMenu.member; confirmDialog.open() } }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 13
                text: qsTr("Your key fingerprint: %1. Compare fingerprints with your colleagues over another channel: a match proves the key is theirs.").arg(teams.fingerprint())
            }

            // ---- invites ----
            Heading { text: qsTr("Invites"); visible: page.canEdit }
            Repeater {
                model: page.canEdit ? page.inviteList : []
                delegate: ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    contentItem: RowLayout {
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: qsTr("Invite as %1").arg(modelData.role); Layout.fillWidth: true }
                            Label { text: modelData.expires_at ? qsTr("valid until %1").arg(new Date(modelData.expires_at).toLocaleDateString()) : ""; opacity: 0.65; font.pixelSize: 13 }
                        }
                        ToolButton { icon.source: "icons/delete.svg"; Accessible.name: qsTr("Revoke"); onClicked: teams.revokeInvite(page.team.uuid, modelData.id) }
                    }
                }
            }
            Button {
                Layout.leftMargin: 16
                visible: page.canEdit
                flat: true
                text: qsTr("Invite someone…")
                onClicked: inviteDialog.open()
            }
        }
    }

    // ---- dialogs ----
    Dialog {
        id: inviteDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Invite someone")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: teams.invite(page.team.uuid, inviteRole.currentIndex === 1 ? "admin" : "member", [1, 7, 30][inviteDays.currentIndex])
        ColumnLayout {
            width: inviteDialog.availableWidth
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; opacity: 0.7; text: qsTr("You get a one-time code to send them; they enter it in NixSCP (Teams, Join).") }
            Label { text: qsTr("Role"); opacity: 0.7 }
            // only the owner makes admins
            ComboBox { id: inviteRole; Layout.fillWidth: true; model: page.owner ? [qsTr("Member (uses the sites)"), qsTr("Admin (also changes them, invites)")] : [qsTr("Member (uses the sites)")] }
            Label { text: qsTr("Valid for"); opacity: 0.7 }
            ComboBox { id: inviteDays; Layout.fillWidth: true; currentIndex: 1; model: [qsTr("1 day"), qsTr("7 days"), qsTr("30 days")] }
        }
    }

    Dialog {
        id: codeDialog
        property string code
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: qsTr("Invite code")
        standardButtons: Dialog.Close
        ColumnLayout {
            width: codeDialog.availableWidth
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Send this code to your colleague; it works once.") }
            TextArea { id: codeText; Layout.fillWidth: true; readOnly: true; wrapMode: TextEdit.WrapAnywhere; font.family: monoFont; text: codeDialog.code }
            RowLayout {
                Button { text: qsTr("Copy"); flat: true; onClicked: { codeText.selectAll(); codeText.copy(); codeText.deselect(); app.toast(qsTr("Copied.")) } }
                Button { text: qsTr("Share…"); flat: true; onClicked: androidFiles.shareText(qsTr("Join my NixSCP team \"%1\": open NixSCP, Teams, Join with an invite code, and enter %2").arg(page.team.name).arg(codeDialog.code)) }
            }
        }
    }

    Dialog {
        id: nameDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Rename team")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { teamName.text = page.team.name; teamName.forceActiveFocus(); teamName.selectAll() }
        onAccepted: if (teamName.text.trim().length > 0) teams.rename(page.team.uuid, teamName.text)
        TextField { id: teamName; width: nameDialog.availableWidth; onAccepted: nameDialog.accept() }
    }

    Dialog {
        id: shareDialog
        property string folder
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Share \"%1\"?").arg(folder)
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: shareDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("Its sites (with their passwords) move into \"%1\" and every member gets them. Proxy, tunnel and command settings stay on this phone.").arg(page.team.folder || "")
        }
        onAccepted: teams.shareFolder(page.team.uuid, folder)
    }

    // pick one of your own sites to share
    Dialog {
        id: sitePicker
        property var sites: []
        property string filter: ""
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        height: Math.min(parent.height - insets.top - insets.bottom - 32, 560)
        modal: true
        title: qsTr("Share a site")
        standardButtons: Dialog.Cancel
        onOpened: { siteFilter.text = ""; keepCopy.checked = true }
        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            TextField {
                id: siteFilter
                Layout.fillWidth: true
                placeholderText: qsTr("Search your sites")
                inputMethodHints: Qt.ImhNoPredictiveText
                onTextChanged: sitePicker.filter = text.toLowerCase()
            }
            CheckBox { id: keepCopy; text: qsTr("Keep my own copy as well"); checked: true }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: sitePicker.sites.filter(function(s) { return sitePicker.filter.length === 0 || s.toLowerCase().indexOf(sitePicker.filter) >= 0 })
                delegate: ItemDelegate {
                    required property string modelData
                    width: ListView.view.width
                    text: modelData
                    onClicked: {
                        teams.shareSite(page.team.uuid, modelData, "", !keepCopy.checked)
                        sitePicker.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: unshareDialog
        property string site
        property bool keep: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Stop sharing \"%1\"?").arg(site.split("/").pop())
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: unshareDialog.availableWidth
            wrapMode: Text.Wrap
            text: (unshareDialog.keep ? qsTr("It moves back to your own sites.") : qsTr("It is deleted."))
                  + " " + qsTr("The members lose it at their next sync. They could have copied its password while it was shared.")
        }
        onAccepted: teams.unshare(page.team.uuid, [site], keep)
    }

    Dialog {
        id: confirmDialog
        property string action
        property var member: ({})
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: action === "delete" ? qsTr("Delete the team?") : (action === "leave" ? qsTr("Leave the team?")
               : (action === "useTeam" ? qsTr("Use the team's version?") : (action === "keepMine" ? qsTr("Keep your version?")
               : (action === "forget" || action === "forgetKeep" ? qsTr("Remove the team from this phone?") : qsTr("Remove %1?").arg(member.email || "")))))
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: confirmDialog.availableWidth
            wrapMode: Text.Wrap
            text: confirmDialog.action === "delete" ? qsTr("Every member loses the team's sites. This cannot be undone.")
                  : confirmDialog.action === "leave" ? qsTr("The team's sites are removed from your devices.")
                  : confirmDialog.action === "useTeam" ? qsTr("Your changes to the team's sites on this phone are lost.")
                  : confirmDialog.action === "keepMine" ? qsTr("Your sites replace the team's for everyone. The team's version stays in the history.")
                  : confirmDialog.action === "forgetKeep" ? qsTr("It leaves this phone until someone invites you again; nothing changes for the others. Its sites stay here as your own.")
                  : confirmDialog.action === "forget" ? qsTr("It leaves this phone until someone invites you again; nothing changes for the others. Its sites are removed from here.")
                  : qsTr("They lose the team's sites; the team key is renewed.")
        }
        onAccepted: {
            if (action === "delete") teams.remove(page.team.uuid)
            else if (action === "leave") teams.leave(page.team.uuid)
            else if (action === "useTeam") teams.resolveConflict(page.team.uuid, false)
            else if (action === "keepMine") teams.resolveConflict(page.team.uuid, true)
            else if (action === "forget") teams.forget(page.team.uuid, false)
            else if (action === "forgetKeep") teams.forget(page.team.uuid, true)
            else teams.removeMember(page.team.uuid, member.user_uuid)
        }
    }

    // the versions the server keeps; one becomes the content again
    Dialog {
        id: historyDialog
        property var versions: []
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        height: Math.min(parent.height - insets.top - insets.bottom - 32, 560)
        modal: true
        title: qsTr("Restore from history")
        standardButtons: Dialog.Cancel
        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; opacity: 0.7; text: qsTr("The chosen version becomes the team's content for everyone, as a new version.") }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: historyDialog.versions
                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    text: qsTr("Version %1").arg(modelData.version) + (modelData.created_at ? " · " + new Date(modelData.created_at).toLocaleString() : "")
                    onClicked: {
                        teams.restore(page.team.uuid, modelData.version)
                        historyDialog.close()
                    }
                }
            }
        }
    }
}
