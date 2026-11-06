// The open session: the phone's folder and the server's, side by side on
// wide screens (unfolded Fold, tablets; WinSCP's Commander: phone left,
// server right), as two tabs on phones. Selected files go to the other side
// with Download / Upload. The progress of a transfer, with Cancel, at the
// bottom.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "browser"
    readonly property bool wide: width >= 600
    // the pane Back and Refresh act on (narrow: the one shown)
    property bool remoteActive: true

    function back() {
        var pane = remoteActive ? remotePane : localPane
        if (pane.back())
            return true
        if (!remoteActive) {
            // at the top of the phone's folders: to the server
            remoteActive = true
            return true
        }
        return closeSession()
    }
    // closing a tab stops what runs in it: only when the user says so
    function closeSession() {
        if (session.busy || session.hasTransfers()) closeDialog.open()
        else sessions.closeCurrent()
        return true
    }

    header: Column {
        ToolBar {
            width: parent.width
            Material.background: Material.primary
            Material.foreground: "white"
            RowLayout {
                anchors.fill: parent
                ToolButton { icon.source: "icons/close.svg"; icon.color: "white"; Accessible.name: qsTr("Disconnect"); onClicked: page.closeSession() }
                Label {
                    text: session.site
                    font.pixelSize: 17
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                ToolButton {
                    icon.source: "icons/add.svg"
                    icon.color: "white"
                    Accessible.name: qsTr("New tab")
                    onClicked: page.StackView.view.push(Qt.resolvedUrl("SitesPage.qml"))
                }
                ToolButton {
                    icon.source: "icons/refresh.svg"
                    icon.color: "white"
                    Accessible.name: qsTr("Refresh")
                    enabled: !session.busy
                    onClicked: (page.remoteActive ? remotePane : localPane).refresh()
                }
                ToolButton {
                    icon.source: "icons/terminal.svg"
                    icon.color: "white"
                    Accessible.name: qsTr("Terminal")
                    visible: session.canRunCommands
                    onClicked: page.StackView.view.push(Qt.resolvedUrl("TerminalPage.qml"))
                }
                ToolButton {
                    icon.source: "icons/more.svg"
                    icon.color: "white"
                    Accessible.name: qsTr("Menu")
                    onClicked: sessionMenu.popup()
                    Menu {
                        // not under the status or navigation bar (popups cover the whole window)
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        id: sessionMenu
                        MenuItem {
                            text: qsTr("Terminal")
                            enabled: session.canRunCommands
                            onTriggered: page.StackView.view.push(Qt.resolvedUrl("TerminalPage.qml"))
                        }
                        MenuItem { text: qsTr("Transfers"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("TransfersPage.qml")) }
                        MenuItem { text: qsTr("Install a public key…"); enabled: session.canRunCommands; onTriggered: page.StackView.view.push(Qt.resolvedUrl("KeysPage.qml")) }
                        MenuItem { text: qsTr("Settings"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml")) }
                    }
                }
            }
        }
        // the open sessions (shown with two or more)
        TabBar {
            id: tabs
            width: parent.width
            visible: sessions.count > 1
            Material.background: Qt.darker(Material.primary, 1.15)
            Material.foreground: "white"
            Material.accent: "white"
            currentIndex: sessions.currentIndex
            Repeater {
                model: sessions.sessions
                delegate: TabButton {
                    required property var modelData
                    required property int index
                    width: Math.max(implicitWidth, 120)
                    // the site and its folder (two tabs of one site differ)
                    text: (modelData.busy ? "• " : "") + modelData.site
                          + (modelData.directory && modelData.directory !== "/" ? " · " + modelData.directory.substring(modelData.directory.lastIndexOf("/") + 1) : "")
                    onClicked: sessions.currentIndex = index
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        FilePane {
            id: localPane
            remote: false
            active: !page.remoteActive
            visible: page.wide || !page.remoteActive
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: page.width / 2
            onActivated: page.remoteActive = false
        }
        Rectangle { visible: page.wide; Layout.fillHeight: true; Layout.preferredWidth: 1; color: Material.dividerColor }
        FilePane {
            id: remotePane
            remote: true
            active: page.remoteActive
            visible: page.wide || page.remoteActive
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: page.width / 2
            onActivated: page.remoteActive = true
        }
    }

    Dialog {
        id: closeDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Close %1?").arg(session.site)
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: closeDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("Something is still running on this connection. Closing it stops its transfers.")
        }
        onAccepted: sessions.closeCurrent()
    }

    footer: ColumnLayout {
        spacing: 0
        // ---- the transfer queue (tap: its list) ----
        ItemDelegate {
            Layout.fillWidth: true
            visible: transfers.active > 0
            padding: 8
            leftPadding: 16
            onClicked: page.StackView.view.push(Qt.resolvedUrl("TransfersPage.qml"))
            contentItem: ColumnLayout {
                spacing: 2
                Label { text: transfers.summary; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: 13 }
                ProgressBar {
                    Layout.fillWidth: true
                    indeterminate: transfers.progress < 0
                    value: Math.max(0, transfers.progress)
                }
            }
            background: Rectangle { color: Qt.tint(Material.dialogColor, Qt.rgba(Material.accent.r, Material.accent.g, Material.accent.b, 0.12)) }
        }
        // ---- what runs: transfer progress, or the status, with Cancel ----
        Pane {
            Layout.fillWidth: true
            visible: session.busy
            padding: 8
            leftPadding: 16
            Material.elevation: 6
            RowLayout {
                anchors.fill: parent
                spacing: 12
                BusyIndicator {
                    visible: !session.transferring
                    running: visible
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        text: session.transferring && session.progressFile ? session.progressFile : session.status
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: session.transferring
                        indeterminate: session.progress < 0
                        value: Math.max(0, session.progress)
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: session.transferring && text.length > 0
                        text: (session.progress >= 0 ? Math.round(session.progress * 100) + "% · " : "") + session.progressText
                        font.pixelSize: 12
                        opacity: 0.7
                        elide: Text.ElideRight
                    }
                }
                Button { text: qsTr("Cancel"); flat: true; onClicked: session.cancel() }
            }
        }
        // ---- phone / server (narrow screens) ----
        TabBar {
            Layout.fillWidth: true
            visible: !page.wide
            currentIndex: page.remoteActive ? 1 : 0
            TabButton {
                text: qsTr("Phone")
                icon.source: "icons/phone.svg"
                display: AbstractButton.TextBesideIcon
                onClicked: page.remoteActive = false
            }
            TabButton {
                text: qsTr("Server")
                icon.source: "icons/server.svg"
                display: AbstractButton.TextBesideIcon
                onClicked: page.remoteActive = true
            }
        }
    }
}
