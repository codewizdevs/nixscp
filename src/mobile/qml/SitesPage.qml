// The saved sites, grouped by folder; tap to connect, long-press to delete,
// + to add a site, the menu to import a WinSCP.ini.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

Page {
    id: page
    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            Image {
                source: "nixscp-128.png"
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                Layout.leftMargin: 12
            }
            Label { text: "NixSCP"; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true }
            ToolButton {
                icon.source: "icons/more.svg"
                icon.color: "white"
                Accessible.name: qsTr("Menu")
                onClicked: menu.open()
                Menu {
                    id: menu
                    MenuItem { text: qsTr("Import WinSCP.ini…"); onTriggered: importDialog.open() }
                    MenuItem { text: qsTr("About NixSCP"); onTriggered: aboutDialog.open() }
                }
            }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        model: sites
        section.property: "folder"
        section.delegate: Label {
            required property string section
            width: ListView.view.width
            text: section
            visible: section.length > 0
            height: visible ? implicitHeight + 16 : 0
            leftPadding: 16
            topPadding: 12
            font.bold: true
            color: Material.accent
        }
        delegate: ItemDelegate {
            required property string name
            required property string title
            required property string host
            required property string user
            required property string protocol
            width: ListView.view.width
            enabled: !session.busy
            contentItem: ColumnLayout {
                spacing: 2
                Label { text: title; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                Label {
                    text: protocol + " · " + (user ? user + "@" : "") + host
                    opacity: 0.65
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            onClicked: session.open(name)
            onPressAndHold: { removeDialog.site = name; removeDialog.open() }
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 64
        visible: list.count === 0
        spacing: 12
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: 18
            text: qsTr("No sites yet")
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("Add an SFTP, SCP, FTP or WebDAV site with +, or import your WinSCP.ini from the menu.")
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: session.busy
        visible: running
    }
    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 96
        text: session.status
        visible: session.busy
    }

    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 64
        height: 64
        icon.source: "icons/add.svg"
        Accessible.name: qsTr("Add")
        icon.width: 28
        icon.height: 28
        highlighted: true
        onClicked: page.StackView.view.push(Qt.resolvedUrl("SitePage.qml"))
    }

    Dialog {
        id: removeDialog
        property string site
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete site?")
        Label { text: removeDialog.site; wrapMode: Text.Wrap; width: parent.width }
        standardButtons: Dialog.Cancel | Dialog.Yes
        onAccepted: sites.removeSite(site)
    }

    FileDialog {
        id: importDialog
        title: qsTr("WinSCP.ini to import")
        nameFilters: [qsTr("WinSCP configuration (*.ini)"), qsTr("All files (*)")]
        onAccepted: {
            var count = sites.importWinScpIni(selectedFile.toString())
            app.toast(count < 0 ? qsTr("This file could not be imported.") : qsTr("%1 sites imported.").arg(count))
        }
    }

    Dialog {
        id: aboutDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: "NixSCP " + appVersion
        standardButtons: Dialog.Ok
        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("WinSCP for Linux, macOS, Windows and Android. Free and open source (GPL-3.0), based on WinSCP. https://nixscp.com")
        }
    }
}
