// The built-in editor for a remote text file (downloaded first): Save
// uploads it, in its own encoding and line endings; a file that is not UTF-8
// is opened in the encoding the user chooses. Back asks about unsaved
// changes. The page stays when its session ends: the text exists only here,
// so it can be saved over a new connection or as a copy.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    objectName: "editor"
    // the session the file belongs to (any tab); null when it ended
    property var owner: session
    property string site: ""
    property string localPath
    property string remotePath
    property string encoding: ""
    property string lineEnding: ""
    property bool modified: false
    property bool wrap: true
    // the session ended with changes here
    property bool orphaned: false
    property bool reconnecting: false
    // a save on its way: the text it took, and whether to leave when it arrives
    property int revision: 0
    property int savedRevision: 0
    property bool saving: false
    property bool leaving: false
    readonly property bool busy: owner ? owner.busy : false
    readonly property string fileName: remotePath.substring(remotePath.lastIndexOf("/") + 1)
    // Android's keyboard height (Qt's on Android 9/10)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    function leave() { page.StackView.view.pop() }

    function load(chosen) {
        var file = owner.readEdit(localPath, chosen)
        if (file.error) {
            app.toast(file.error)
            Qt.callLater(leave)
            return
        }
        if (file.needsEncoding) {
            encodingDialog.show(file)
            return
        }
        encoding = file.encoding
        lineEnding = file.lineEnding
        area.text = file.text
        modified = false
    }

    Component.onCompleted: {
        site = owner.site
        load("")
    }
    // nothing of the file stays on the phone
    StackView.onRemoved: {
        if (owner) owner.closeEdit(localPath)
        Qt.callLater(app.leaveEmptyBrowser)
    }

    function save() {
        savedRevision = revision
        saving = true
        owner.saveText(localPath, area.text, encoding)
    }
    // Back: asks first when there are unsaved changes
    function back() {
        if (!modified)
            return false
        unsavedDialog.open()
        return true
    }

    Connections {
        target: page.owner
        ignoreUnknownSignals: true
        function onEditSaved(path, ok) {
            if (path !== page.localPath)
                return
            page.saving = false
            if (ok && page.revision === page.savedRevision)
                page.modified = false
            if (page.leaving) {
                page.leaving = false
                if (ok) page.leave()
            }
        }
        // the file again over a new connection: the text goes up
        function onEditAdopted(path, remote) {
            if (remote !== page.remotePath || !page.reconnecting)
                return
            page.reconnecting = false
            page.orphaned = false
            page.localPath = path
            page.owner.readEdit(path, page.encoding)
            page.save()
        }
    }
    Connections {
        target: sessions
        function onSessionEnded(ended) {
            if (ended !== page.owner)
                return
            page.saving = page.leaving = false
            page.owner = null
            if (page.modified)
                page.orphaned = true
            else
                page.leave()
        }
        function onEstablished(name) {
            if (page.reconnecting && !page.owner && name === page.site) {
                page.owner = sessions.current
                page.owner.adoptEdit(page.remotePath)
            }
        }
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton {
                icon.source: "icons/back.svg"
                icon.color: "white"
                Accessible.name: qsTr("Back")
                onClicked: if (!page.back()) page.StackView.view.pop()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: (page.modified ? "• " : "") + page.fileName
                    font.pixelSize: 16
                    font.bold: true
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
                Label {
                    text: [page.site, page.encoding, page.lineEnding].filter(function(t) { return t.length > 0 }).join(" · ")
                    font.pixelSize: 12
                    opacity: 0.85
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            ToolButton {
                icon.source: "icons/wrap.svg"
                icon.color: "white"
                checkable: true
                checked: page.wrap
                Accessible.name: qsTr("Wrap lines")
                onClicked: page.wrap = !page.wrap
            }
            ToolButton {
                icon.source: "icons/save.svg"
                icon.color: "white"
                Accessible.name: qsTr("Save")
                enabled: page.modified && page.owner !== null && !page.busy && !page.saving
                onClicked: page.save()
            }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        bottomMargin: page.keyboardHeight
        clip: true
        flickableDirection: page.wrap ? Flickable.VerticalFlick : Flickable.HorizontalAndVerticalFlick
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar { visible: !page.wrap }

        TextArea.flickable: TextArea {
            id: area
            font.family: monoFont
            font.pixelSize: 14
            // what the file says, never markup
            textFormat: TextEdit.PlainText
            wrapMode: page.wrap ? TextEdit.Wrap : TextEdit.NoWrap
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            selectByMouse: true
            persistentSelection: true
            background: null
            leftPadding: 12
            rightPadding: 12
            onTextChanged: { page.modified = true; page.revision++ }
        }
    }

    // the upload of a save; the session that ended
    footer: Pane {
        visible: page.busy || page.orphaned
        padding: 8
        ColumnLayout {
            anchors.fill: parent
            RowLayout {
                visible: page.busy
                Layout.fillWidth: true
                BusyIndicator { running: page.busy; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                Label { text: page.owner ? page.owner.status : ""; Layout.fillWidth: true; elide: Text.ElideRight }
            }
            Label {
                visible: page.orphaned
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("The connection to %1 ended. Your changes are only here.").arg(page.site)
            }
            Flow {
                visible: page.orphaned
                Layout.fillWidth: true
                spacing: 4
                Button {
                    text: qsTr("Reconnect and save")
                    highlighted: true
                    enabled: !page.reconnecting
                    onClicked: { page.reconnecting = true; sessions.open(page.site) }
                }
                Button {
                    text: qsTr("Save a copy")
                    flat: true
                    onClicked: {
                        var place = androidFiles.saveCopy(page.fileName, area.text, page.encoding)
                        if (place) { app.toast(qsTr("Saved to %1").arg(place)); page.modified = false; page.orphaned = false; page.leave() }
                        else app.toast(qsTr("The file could not be saved."))
                    }
                }
            }
        }
    }

    // a file that is not UTF-8: which encoding it is in tells only the user
    Dialog {
        id: encodingDialog
        property var encodings: []
        property string sample
        property bool chosen: false
        function show(file) {
            encodings = file.encodings
            sample = file.sample
            chosen = false
            encodingBox.currentIndex = 0
            open()
        }
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 460)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("This file is not UTF-8")
        onClosed: if (!chosen) page.leave()
        ColumnLayout {
            width: encodingDialog.availableWidth
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Choose its encoding. It is saved in the same encoding, so nothing else in the file changes.")
            }
            ComboBox {
                id: encodingBox
                Layout.fillWidth: true
                model: encodingDialog.encodings
                onActivated: encodingDialog.sample = page.owner.readEdit(page.localPath, currentText).sample
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WrapAnywhere
                font.family: monoFont
                font.pixelSize: 13
                text: encodingDialog.sample
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: encodingDialog.close() }
            Button {
                text: qsTr("Open")
                flat: true
                highlighted: true
                onClicked: { encodingDialog.chosen = true; encodingDialog.close(); page.load(encodingBox.currentText) }
            }
        }
    }

    Dialog {
        id: unsavedDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Save the changes?")
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: unsavedDialog.close() }
            Button { text: qsTr("Discard"); flat: true; onClicked: { unsavedDialog.close(); page.modified = false; page.orphaned = false; page.leave() } }
            // leaves when it arrived: a failed save keeps the page and the text
            Button {
                text: qsTr("Save")
                flat: true
                highlighted: true
                visible: page.owner !== null
                // a save on its way ends first
                enabled: !page.saving
                onClicked: { unsavedDialog.close(); page.leaving = true; page.save() }
            }
        }
    }
}
