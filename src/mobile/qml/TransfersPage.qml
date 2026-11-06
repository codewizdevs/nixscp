// The transfer queue of all tabs: waiting, running and finished downloads
// and uploads; Cancel, Retry, Clear finished.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "transfers"

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            Label { text: qsTr("Transfers"); font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
            ToolButton { text: qsTr("Clear finished"); enabled: transfers.finished > 0; onClicked: transfers.clearFinished() }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        model: transfers
        delegate: ItemDelegate {
            required property int itemId
            required property string site
            required property string title
            required property string direction
            required property string status
            required property double progress
            required property string detail
            required property string error
            required property string target
            required property bool canRetry
            width: ListView.view.width
            contentItem: RowLayout {
                spacing: 12
                Label {
                    text: direction === "down" ? "↓" : "↑"
                    font.pixelSize: 22
                    color: status === "failed" ? Material.color(Material.Red) : Material.accent
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: title; font.pixelSize: 15; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label {
                        // phone folders under the storage root: /Download
                        text: site + " → " + (direction === "down" && target.indexOf(phone.root) === 0 ? (target.substring(phone.root.length) || "/") : target)
                        font.pixelSize: 12
                        opacity: 0.6
                        elide: Text.ElideLeft
                        Layout.fillWidth: true
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: status === "running"
                        indeterminate: progress < 0
                        value: Math.max(0, progress)
                    }
                    Label {
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: status === "failed" ? Material.color(Material.Red) : Material.foreground
                        opacity: status === "failed" ? 1 : 0.7
                        text: status === "waiting" ? qsTr("Waiting")
                              : status === "running" ? (progress >= 0 ? Math.round(progress * 100) + "% · " : "") + detail
                              : status === "done" ? qsTr("Done")
                              : status === "cancelled" ? qsTr("Cancelled")
                              : qsTr("Failed: %1").arg(error)
                    }
                }
                ToolButton {
                    visible: status === "waiting" || status === "running"
                    icon.source: "icons/close.svg"
                    Accessible.name: qsTr("Cancel")
                    onClicked: transfers.cancel(itemId)
                }
                ToolButton {
                    // not after the tab closed: its connection is gone
                    visible: canRetry
                    icon.source: "icons/refresh.svg"
                    Accessible.name: qsTr("Retry")
                    onClicked: transfers.retry(itemId)
                }
            }
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0
        opacity: 0.6
        text: qsTr("No transfers")
    }
}
