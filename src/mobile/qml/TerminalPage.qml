// The session's terminal: an interactive shell (top, htop, vi, less...)
// starting in the folder being browsed. Tap to type; the extra keys give
// Esc, Tab, Ctrl, Alt, arrows and the symbols a phone keyboard hides; drag
// to scroll back. Leaving the page keeps the shell; the session's close
// ends it.
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "terminal"
    property bool ctrl: false
    property bool alt: false
    readonly property var term: session.terminal
    property string ended: ""

    Settings {
        category: "Terminal"
        property alias fontSize: view.fontSize
    }

    function start() {
        ended = ""
        session.openTerminal(view.columns(), view.rows())
        typeHere()
    }
    function typeHere() {
        sink.forceActiveFocus()
        Qt.inputMethod.show()
    }
    // a key of the extra row or the keyboard, with the sticky modifiers
    function key(name) {
        if (!term) return
        term.sendKey(name, ctrl, alt)
        ctrl = alt = false
        view.scrollOffset = 0
    }
    function text(t) {
        if (!term || t.length === 0) return
        term.sendText(t, ctrl, alt)
        ctrl = alt = false
        view.scrollOffset = 0
    }

    Component.onCompleted: Qt.callLater(start)

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { text: qsTr("Terminal"); font.pixelSize: 16; font.bold: true }
                Label { text: session.site; font.pixelSize: 12; opacity: 0.85; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            ToolButton { text: "A−"; font.bold: true; Accessible.name: qsTr("Smaller text"); onClicked: view.fontSize = view.fontSize - 1 }
            ToolButton { text: "A+"; font.bold: true; Accessible.name: qsTr("Larger text"); onClicked: view.fontSize = view.fontSize + 1 }
            ToolButton {
                text: qsTr("Paste")
                enabled: page.term && page.term.running
                onClicked: { page.term.paste(); page.typeHere() }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.bottomMargin: app.keyboardHeight
        spacing: 0

        TerminalView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            terminal: page.term
            fontFamily: monoFont

            // tap: the keyboard; drag: the scrollback
            MouseArea {
                anchors.fill: parent
                property real startY
                property int startOffset
                property bool dragged
                onPressed: function(mouse) { startY = mouse.y; startOffset = view.scrollOffset; dragged = false }
                onPositionChanged: function(mouse) {
                    var lines = Math.round((mouse.y - startY) / view.cellHeight)
                    if (Math.abs(lines) > 0) dragged = true
                    view.scrollOffset = startOffset + lines
                }
                onReleased: if (!dragged) page.typeHere()
            }

            // connecting, or the shell ended
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 420)
                height: status.implicitHeight + 32
                radius: 8
                color: "#cc303030"
                visible: !page.term || page.term.connecting || !page.term.running
                Label {
                    id: status
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: "white"
                    text: page.term && page.term.connecting ? qsTr("Connecting…")
                          : (page.ended.length > 0 ? page.ended + "\n\n" : qsTr("The shell ended.") + "\n\n") + qsTr("Tap to start a new one.")
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: page.term ? !page.term.running && !page.term.connecting : true
                    onClicked: page.start()
                }
            }

            // back to the live screen
            RoundButton {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                visible: view.scrollOffset > 0
                text: "↓"
                onClicked: view.scrollOffset = 0
            }
        }

        // the keys a phone keyboard lacks
        Pane {
            Layout.fillWidth: true
            padding: 2
            Material.elevation: 4
            Flickable {
                anchors.fill: parent
                implicitHeight: keys.implicitHeight
                contentWidth: keys.implicitWidth
                flickableDirection: Flickable.HorizontalFlick
                clip: true
                Row {
                    id: keys
                    spacing: 0
                    component Key: ToolButton {
                        property string send: ""
                        property string symbol: ""
                        font.family: monoFont
                        font.pixelSize: 15
                        implicitWidth: Math.max(44, implicitContentWidth + 16)
                        focusPolicy: Qt.NoFocus
                        onClicked: { if (send) page.key(send); else page.text(symbol); page.typeHere() }
                    }
                    Key { text: "Esc"; send: "Escape" }
                    Key { text: "Tab"; send: "Tab" }
                    ToolButton {
                        text: "Ctrl"
                        checkable: true
                        checked: page.ctrl
                        focusPolicy: Qt.NoFocus
                        onClicked: { page.ctrl = !page.ctrl; page.typeHere() }
                    }
                    ToolButton {
                        text: "Alt"
                        checkable: true
                        checked: page.alt
                        focusPolicy: Qt.NoFocus
                        onClicked: { page.alt = !page.alt; page.typeHere() }
                    }
                    Key { text: "←"; send: "Left" }
                    Key { text: "↑"; send: "Up" }
                    Key { text: "↓"; send: "Down" }
                    Key { text: "→"; send: "Right" }
                    Key { text: "Home"; send: "Home" }
                    Key { text: "End"; send: "End" }
                    Key { text: "PgUp"; send: "PageUp" }
                    Key { text: "PgDn"; send: "PageDown" }
                    Key { text: "|"; symbol: "|" }
                    Key { text: "/"; symbol: "/" }
                    Key { text: "-"; symbol: "-" }
                    Key { text: "~"; symbol: "~" }
                    Key { text: "*"; symbol: "*" }
                    Key { text: "&"; symbol: "&" }
                    Key { text: ";"; symbol: ";" }
                    Key { text: "$"; symbol: "$" }
                    Key { text: "F1"; send: "F1" }
                    Key { text: "F2"; send: "F2" }
                    Key { text: "F10"; send: "F10" }
                }
            }
        }
    }

    // the keyboard's input: what was typed into a field kept at two spaces
    // goes to the shell (fewer characters: Backspace)
    TextInput {
        id: sink
        readonly property string rest: "  "
        width: 1
        height: 1
        opacity: 0
        text: rest
        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhPreferLatin
        onTextEdited: {
            var t = text
            if (t.length < rest.length) {
                for (var i = t.length; i < rest.length; i++) page.key("Backspace")
            } else if (t.indexOf(rest) === 0) {
                page.text(t.substring(rest.length))
            } else {
                // replaced in the middle: the difference
                var common = 0
                while (common < t.length && common < rest.length && t[common] === rest[common]) common++
                for (var j = common; j < rest.length; j++) page.key("Backspace")
                page.text(t.substring(common))
            }
            text = rest
            cursorPosition = rest.length
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { page.key("Enter"); event.accepted = true }
            else if (event.key === Qt.Key_Backspace) { page.key("Backspace"); event.accepted = true }
            else if (event.key === Qt.Key_Tab) { page.key("Tab"); event.accepted = true }
            else if (event.key === Qt.Key_Up) { page.key("Up"); event.accepted = true }
            else if (event.key === Qt.Key_Down) { page.key("Down"); event.accepted = true }
            else if (event.key === Qt.Key_Left) { page.key("Left"); event.accepted = true }
            else if (event.key === Qt.Key_Right) { page.key("Right"); event.accepted = true }
            else if (event.key === Qt.Key_Escape) { page.key("Escape"); event.accepted = true }
        }
    }

    Connections {
        target: page.term
        function onFinished(message) { page.ended = message }
    }
}
