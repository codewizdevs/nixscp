// One team: its members (role, remove), invites (create, share the code,
// revoke) and the site folders shared into it; rename, delete or leave.
// Owners and admins manage it; members use its sites.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "team"
    property var team: ({})
    property var memberList: []
    property var inviteList: []
    readonly property bool canEdit: team.canEdit === true
    readonly property bool owner: team.role === "owner"

    Component.onCompleted: load()
    function load() {
        teams.members(team.uuid)
        if (canEdit) teams.invites(team.uuid)
    }

    Connections {
        target: teams
        function onFinished(op, ok, error, data) {
            if (!ok) { if (error) app.toast(error); return }
            if (op === "members") page.memberList = data
            else if (op === "invites") page.inviteList = data
            else if (op === "invite") { codeDialog.code = data; codeDialog.open(); teams.invites(page.team.uuid) }
            else if (op === "revoke" || op === "role" || op === "removeMember") page.load()
            else if (op === "share") app.toast(qsTr("The folder is now shared with the team."))
            else if (op === "rename") app.toast(qsTr("Renamed."))
            else if (op === "delete" || op === "leave") page.StackView.view.pop()
        }
        function onChanged() {
            // the team's new name or role
            var list = teams.teams
            for (var i = 0; i < list.length; i++)
                if (list[i].uuid === page.team.uuid) page.team = list[i]
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

            // ---- sites ----
            Heading { text: qsTr("Sites") }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 13
                text: qsTr("The team's sites are in the folder \"Team: %1\" of the site list.").arg(page.team.name || "")
                      + (page.canEdit ? "" : " " + qsTr("Only owners and admins change them."))
            }
            Button {
                Layout.leftMargin: 16
                visible: page.canEdit
                flat: true
                text: qsTr("Share a folder of your sites…")
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
                        Label { text: modelData.role; opacity: 0.65; font.pixelSize: 13 }
                    }
                    // owners and admins manage the others (not the owner)
                    onClicked: if (page.canEdit && modelData.role !== "owner" && modelData.email !== pro.email) { memberMenu.member = modelData; memberMenu.popup() }
                }
            }
            Menu {
                id: memberMenu
                topMargin: insets.top + 4
                bottomMargin: insets.bottom + 4
                property var member: ({})
                MenuItem { text: memberMenu.member.email || ""; enabled: false }
                MenuItem { text: qsTr("Make admin"); visible: memberMenu.member.role === "member"; height: visible ? implicitHeight : 0; onTriggered: teams.setRole(page.team.uuid, memberMenu.member.user_uuid, "admin") }
                MenuItem { text: qsTr("Make member"); visible: memberMenu.member.role === "admin"; height: visible ? implicitHeight : 0; onTriggered: teams.setRole(page.team.uuid, memberMenu.member.user_uuid, "member") }
                MenuItem { text: qsTr("Remove from the team…"); onTriggered: { confirmDialog.action = "removeMember"; confirmDialog.member = memberMenu.member; confirmDialog.open() } }
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
            ComboBox { id: inviteRole; Layout.fillWidth: true; model: [qsTr("Member (uses the sites)"), qsTr("Admin (also changes them, invites)")] }
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
            text: qsTr("Its sites (with their passwords) move into \"Team: %1\" and every member gets them.").arg(page.team.name)
        }
        onAccepted: teams.shareFolder(page.team.uuid, folder)
    }

    Dialog {
        id: confirmDialog
        property string action
        property var member: ({})
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: action === "delete" ? qsTr("Delete the team?") : (action === "leave" ? qsTr("Leave the team?") : qsTr("Remove %1?").arg(member.email || ""))
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: confirmDialog.availableWidth
            wrapMode: Text.Wrap
            text: confirmDialog.action === "delete" ? qsTr("Every member loses the team's sites. This cannot be undone.")
                  : confirmDialog.action === "leave" ? qsTr("The team's sites are removed from your devices.")
                  : qsTr("They lose the team's sites; the team key is renewed.")
        }
        onAccepted: {
            if (action === "delete") teams.remove(page.team.uuid)
            else if (action === "leave") teams.leave(page.team.uuid)
            else teams.removeMember(page.team.uuid, member.user_uuid)
        }
    }
}
