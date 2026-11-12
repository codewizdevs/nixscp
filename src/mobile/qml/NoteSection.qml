// A site's note (SessionData::Note), on the site page and on the page of a
// note item: read-only until Edit, then Save or Cancel. It is saved on its
// own (sites.saveNote): the site's other settings stay as they are. A team's
// note is edited by the team's owners and admins only (sites.canEditNote).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ColumnLayout {
    id: section
    objectName: "noteSection"
    // the stored site or note item
    property string site: ""
    // a note item: the note is all there is (no title; the text area also when empty)
    property bool noteItem: false
    property bool editing: false
    // the note as it is stored
    property string saved: ""
    // why the last Save was refused
    property string error: ""
    // teams.teams: a team's roles may change while the page is open
    readonly property bool canEdit: site.length > 0 && teams.teams !== undefined && sites.canEditNote(site)
    readonly property bool changed: editing && noteText.text !== saved
    readonly property bool over: sites.noteBytes(noteText.text) > sites.maxNoteBytes
    spacing: 6
    visible: site.length > 0 && (noteItem || editing || saved.length > 0 || canEdit)

    function load() {
        var data = sites.siteData(site)
        saved = data.note || ""
        // what is being written stays (a sync meanwhile)
        if (!editing)
            noteText.text = saved
    }
    function startEdit() {
        if (!canEdit)
            return
        error = ""
        editing = true
        noteText.forceActiveFocus()
        noteText.cursorPosition = noteText.length
    }
    // false: refused (error tells why), still editing
    function save() {
        if (!editing)
            return true
        var failure = sites.saveNote(site, noteText.text)
        if (failure.length > 0) {
            error = failure
            return false
        }
        error = ""
        editing = false
        Qt.inputMethod.hide()
        load()
        return true
    }
    function cancel() {
        editing = false
        error = ""
        Qt.inputMethod.hide()
        noteText.text = saved
    }
    // before the page closes: a changed note is saved, dropped, or the page stays
    function confirmLeave(proceed) {
        if (!changed) {
            proceed()
            return
        }
        leaveDialog.proceed = proceed
        leaveDialog.open()
    }

    Component.onCompleted: load()
    onSiteChanged: load()
    Connections {
        target: sites
        // a sync or another page changed the sites
        function onRowsChanged() { section.load() }
    }

    Label {
        visible: !section.noteItem
        Layout.topMargin: 12
        text: qsTr("Note")
        font.bold: true
    }
    TextArea {
        id: noteText
        objectName: "noteText"
        Layout.fillWidth: true
        Layout.minimumHeight: section.noteItem ? 160 : 0
        visible: section.noteItem || section.editing || section.saved.length > 0
        readOnly: !section.editing
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        selectByMouse: true
        // Material's own leave room for a floating placeholder
        leftPadding: 12
        rightPadding: 12
        topPadding: 10
        bottomPadding: 10
        // read: a card; written: a field (no floating placeholder, the hint is below)
        background: Rectangle {
            radius: 4
            color: section.editing ? "transparent" : Qt.rgba(Material.foreground.r, Material.foreground.g, Material.foreground.b, 0.05)
            border.width: !section.editing ? 0 : (noteText.activeFocus ? 2 : 1)
            border.color: noteText.activeFocus ? Material.accent : Material.hintTextColor
        }
        onTextChanged: if (section.editing) section.error = ""
    }
    Label {
        Layout.fillWidth: true
        visible: noteText.visible && noteText.text.length === 0
        wrapMode: Text.Wrap
        opacity: 0.6
        font.pixelSize: 13
        text: !section.editing ? (section.canEdit ? qsTr("No note yet. Tap Edit to write it.") : qsTr("No note yet."))
              : section.noteItem ? qsTr("Anything that belongs with your sites: office network details, who to call, how to reach a server.")
                                 : qsTr("What you need to know about this server: where its logs are, what runs there, what not to restart, who to call.")
    }
    Label {
        Layout.fillWidth: true
        visible: section.editing || section.saved.length > 0
        text: sites.noteSize(noteText.text)
        color: section.over ? Material.color(Material.Red) : Material.foreground
        opacity: section.over ? 1 : 0.6
        font.pixelSize: 12
    }
    Label {
        Layout.fillWidth: true
        visible: !section.canEdit
        wrapMode: Text.Wrap
        opacity: 0.7
        font.pixelSize: 13
        text: qsTr("This note belongs to a team: only the team's owners and admins edit it.")
    }
    Label {
        Layout.fillWidth: true
        visible: section.error.length > 0
        wrapMode: Text.Wrap
        color: Material.color(Material.Red)
        text: section.error
    }
    RowLayout {
        Layout.fillWidth: true
        visible: section.canEdit
        Button {
            visible: !section.editing
            flat: true
            text: (section.noteItem || section.saved.length > 0) ? qsTr("Edit") : qsTr("Add a note")
            onClicked: section.startEdit()
        }
        Item { Layout.fillWidth: true }
        Button {
            visible: section.editing
            flat: true
            text: qsTr("Cancel")
            onClicked: section.cancel()
        }
        Button {
            visible: section.editing
            flat: true
            highlighted: true
            text: qsTr("Save")
            onClicked: section.save()
        }
    }

    Dialog {
        id: leaveDialog
        property var proceed: null
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Save the note?")
        Label {
            width: leaveDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("You changed the note of \"%1\".").arg(section.site.split("/").pop())
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); flat: true; onClicked: leaveDialog.close() }
            Button { text: qsTr("Discard"); flat: true; onClicked: { leaveDialog.close(); section.cancel(); leaveDialog.proceed() } }
            Button { text: qsTr("Save"); flat: true; highlighted: true; onClicked: { leaveDialog.close(); if (section.save()) leaveDialog.proceed() } }
        }
    }
}
