// Find files below the server's current folder (WinSCP's Find files): a
// part of the name or a mask such as *.log. Tap a result to go to its
// folder.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "search"
    property string base: session.directory
    property bool searching: false
    property bool done: false

    function start() {
        results.clear()
        done = false
        Qt.inputMethod.hide()
        searching = session.findFiles(field.text)
    }
    // Back stops a running search first
    function back() {
        if (!searching) return false
        session.stopSearch()
        return true
    }

    Connections {
        target: session
        function onFound(files) {
            for (var i = 0; i < files.length; i++)
                results.append(files[i])
        }
        function onSearchFinished(aborted, error) {
            page.searching = false
            page.done = true
            if (error) app.toast(error)
        }
    }
    ListModel { id: results }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: { session.stopSearch(); page.StackView.view.pop() } }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { text: qsTr("Find files"); font.pixelSize: 16; font.bold: true }
                Label { text: qsTr("in %1").arg(page.base); font.pixelSize: 12; opacity: 0.85; elide: Text.ElideLeft; Layout.fillWidth: true }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.bottomMargin: app.keyboardHeight
        spacing: 0
        Pane {
            Layout.fillWidth: true
            padding: 8
            Material.elevation: 2
            RowLayout {
                anchors.fill: parent
                TextField {
                    id: field
                    Layout.fillWidth: true
                    placeholderText: qsTr("name, or a mask like *.log")
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onAccepted: page.start()
                    Component.onCompleted: forceActiveFocus()
                }
                Button {
                    text: page.searching ? qsTr("Stop") : qsTr("Find")
                    highlighted: !page.searching
                    enabled: page.searching || field.text.trim().length > 0
                    onClicked: page.searching ? session.stopSearch() : page.start()
                }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 12
            visible: page.searching || page.done
            text: page.searching ? session.status : qsTr("%1 found").arg(results.count)
            elide: Text.ElideMiddle
            opacity: 0.7
            font.pixelSize: 12
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: results
            delegate: ItemDelegate {
                required property string name
                required property string directory
                required property bool isDir
                required property string size
                width: ListView.view.width
                contentItem: RowLayout {
                    spacing: 12
                    Image { source: isDir ? "icons/folder.svg" : "icons/file.svg"; sourceSize: Qt.size(24, 24) }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label { text: name; Layout.fillWidth: true; elide: Text.ElideMiddle }
                        Label { text: directory + (size ? " · " + size : ""); opacity: 0.6; font.pixelSize: 12; font.family: monoFont; Layout.fillWidth: true; elide: Text.ElideLeft }
                    }
                }
                onClicked: {
                    session.stopSearch()
                    session.cd(isDir ? directory + (directory === "/" ? "" : "/") + name : directory)
                    page.StackView.view.pop()
                }
            }
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
