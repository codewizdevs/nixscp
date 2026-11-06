// NixSCP Teams: the teams of the account; create one, join one with an
// invite code. Tap a team for its members, invites and sites.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "teams"

    Component.onCompleted: if (teams.available) teams.refresh()
    Connections {
        target: teams
        function onFinished(op, ok, error, data) {
            if (!ok && error) app.toast(error)
            else if (ok && op === "create") app.toast(qsTr("Team created."))
            else if (ok && op === "join") app.toast(qsTr("You joined the team; its sites are being downloaded."))
        }
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            Label { text: qsTr("Teams"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
            BusyIndicator { running: teams.busy; visible: running; Layout.preferredWidth: 28; Layout.preferredHeight: 28 }
            ToolButton { icon.source: "icons/refresh.svg"; icon.color: "white"; Accessible.name: qsTr("Refresh"); enabled: teams.available && !teams.busy; onClicked: teams.refresh() }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        visible: teams.available
        model: teams.teams
        bottomMargin: 96
        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            contentItem: ColumnLayout {
                spacing: 2
                Label { text: modelData.name; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                Label {
                    text: (modelData.members === 1 ? qsTr("%1 · 1 member").arg(modelData.role)
                                                   : qsTr("%1 · %2 members").arg(modelData.role).arg(modelData.members))
                          + (modelData.error ? " · " + qsTr("failed verification")
                             : (modelData.conflict ? " · " + qsTr("needs a decision")
                                : (modelData.keyReady ? "" : " · " + qsTr("no team key"))))
                    opacity: 0.65
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
            onClicked: page.StackView.view.push(Qt.resolvedUrl("TeamPage.qml"), { team: modelData })
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 64
        visible: !teams.available || list.count === 0
        spacing: 10
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: 18
            text: teams.available ? qsTr("No teams yet") : qsTr("Teams need NixSCP Pro")
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.7
            text: teams.available ? qsTr("Create a team with +, or join one with an invite code from a colleague.")
                                  : qsTr("Sign in to NixSCP Pro and enter your NixSCP password first.")
        }
    }

    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 64
        height: 64
        visible: teams.available
        icon.source: "icons/add.svg"
        icon.width: 28
        icon.height: 28
        highlighted: true
        Accessible.name: qsTr("Add a team")
        onClicked: addMenu.open()
        Menu {
            id: addMenu
            topMargin: insets.top + 4
            bottomMargin: insets.bottom + 4
            y: -height
            MenuItem { text: qsTr("Create a team…"); onTriggered: { textDialog.mode = "create"; textDialog.open() } }
            MenuItem { text: qsTr("Join with an invite code…"); onTriggered: { textDialog.mode = "join"; textDialog.open() } }
        }
    }

    Dialog {
        id: textDialog
        property string mode: "create"
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: mode === "create" ? qsTr("Create a team") : qsTr("Join a team")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { textField.text = ""; textField.forceActiveFocus() }
        onAccepted: {
            if (textField.text.trim().length === 0) return
            if (mode === "create") teams.create(textField.text)
            else teams.join(textField.text)
        }
        ColumnLayout {
            width: textDialog.availableWidth
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: textDialog.mode === "create" ? qsTr("You become its owner; invite colleagues from the team's page.")
                                                   : qsTr("The code a team owner or admin gave you.")
            }
            TextField {
                id: textField
                Layout.fillWidth: true
                placeholderText: textDialog.mode === "create" ? qsTr("Team name") : qsTr("Invite code")
                font.family: textDialog.mode === "join" ? monoFont : Qt.application.font.family
                inputMethodHints: textDialog.mode === "join" ? (Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText) : Qt.ImhNone
                onAccepted: textDialog.accept()
            }
        }
    }
}
