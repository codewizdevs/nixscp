// One file pane: the server's folder (remote) or the phone's. Path bar
// (tap: parent folders, bookmarks, Go to), filter as you type, sort and
// hidden files (Settings), long press to select several files, pull down to
// refresh. Tap a folder to open it, a file for its actions (server files
// also: Edit, Extract, Properties, Download as archive).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
// IconImage: icons in the theme's text colour
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: pane
    property bool remote: true
    // the other pane can take files (phone: the server is connected)
    property bool active: true
    readonly property var files: remote ? session.files : phone.files
    readonly property bool busy: remote && session.busy
    readonly property bool selecting: files.selectedCount > 0
    readonly property bool accessible: remote || phone.accessible
    // the path segments: [{title, path}]
    readonly property var crumbs: {
        var list = []
        if (remote) {
            var dir = session.directory
            list.push({ title: "/", path: "/" })
            var parts = dir.split("/").filter(function(p) { return p.length > 0 })
            var path = ""
            for (var i = 0; i < parts.length; i++) {
                path += "/" + parts[i]
                list.push({ title: parts[i], path: path })
            }
        } else {
            list.push({ title: qsTr("Phone"), path: "" })
            var lparts = phone.displayDirectory.split("/").filter(function(p) { return p.length > 0 })
            var lpath = ""
            for (var j = 0; j < lparts.length; j++) {
                lpath += (lpath ? "/" : "") + lparts[j]
                list.push({ title: lparts[j], path: lpath })
            }
        }
        return list
    }
    signal activated()

    // ---- operations, the same for both sides ------------------------------
    function cd(path) { remote ? session.cd(path) : phone.cd(path) }
    function enter(name) { remote ? session.enter(name) : phone.enter(name) }
    function refresh() { remote ? session.refresh() : phone.refresh() }
    // false at the top
    function up() {
        if (remote) {
            if (session.directory === "/" || session.busy) return false
            session.enter("..")
            return true
        }
        if (phone.atRoot) return false
        phone.up()
        return true
    }
    // Back: selection, then the filter, then up a folder
    function back() {
        if (selecting) { files.clearSelection(); return true }
        if (filterBar.visible) { closeFilter(); return true }
        return up()
    }
    function closeFilter() { files.filter = ""; filterBar.visible = false; filterField.focus = false; Qt.inputMethod.hide() }

    // the phone's Download folder ("" without access: Download/NixSCP through Android)
    readonly property string downloads: phone.accessible ? phone.root + "/Download" : ""
    // the phone pane shows another folder: "Copy to" it is offered too
    readonly property bool phoneElsewhere: phone.accessible && phone.directory !== downloads
    function copyToPhone(names) {
        session.download(names, phone.directory)
        files.clearSelection()
    }
    function transfer(names) {
        if (remote)
            session.download(names, downloads)
        else
            session.upload(phone.paths(names))
        files.clearSelection()
    }
    function share(names) {
        if (remote) session.shareFiles(names)
        else if (!androidFiles.share(phone.paths(names))) app.toast(qsTr("These files cannot be shared."))
        files.clearSelection()
    }
    function openFile(name) {
        if (remote) session.openFile(name)
        else if (!androidFiles.open(phone.path(name))) app.toast(qsTr("No app on this phone opens this kind of file."))
    }
    function askRemove(names) {
        if (settings.confirmDelete) { deleteDialog.names = names; deleteDialog.open() }
        else remove(names)
    }
    function edit(name) { session.editFile(name); files.clearSelection() }
    function preview(name) {
        var kind = files.previewKind(name)
        if (remote) session.previewFile(name)
        else stack.push(Qt.resolvedUrl("PreviewPage.qml"), { localPath: phone.path(name), name: name, kind: kind })
    }
    // the shared files (Share -> NixSCP) to the server's folder
    function uploadShared() {
        session.upload(androidFiles.sharedFiles)
        androidFiles.clearShared()
    }
    // the selection packed into one archive, saved to Download
    function archive(names) {
        session.archive(names, settings.archiveFormat, phone.accessible ? phone.root + "/Download" : "")
        files.clearSelection()
    }
    function properties(names) { propertiesDialog.show(names) }
    function remove(names) {
        if (remote) session.remove(names)
        else if (!phone.remove(names)) app.toast(phone.lastError())
        files.clearSelection()
    }
    function rename(from, to) {
        if (remote) session.rename(from, to)
        else if (!phone.rename(from, to)) app.toast(phone.lastError())
        files.clearSelection()
    }
    function createFile(name) {
        if (remote) session.createFile(name)
        else if (!phone.createFile(name)) app.toast(phone.lastError())
    }
    function createFolder(name) {
        if (remote) session.createFolder(name)
        else if (!phone.createFolder(name)) app.toast(phone.lastError())
    }

    // the view of both panes (Settings)
    Binding { target: pane.files; property: "sortBy"; value: settings.sortBy }
    Binding { target: pane.files; property: "descending"; value: settings.descending }
    Binding { target: pane.files; property: "showHidden"; value: settings.showHidden }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- path bar, or the selection's actions ---------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            // the selection: the accent over the bar, readable in light and dark
            color: pane.selecting ? Qt.tint(Material.dialogColor, Qt.rgba(Material.accent.r, Material.accent.g, Material.accent.b, 0.22))
                                  : Material.dialogColor
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Material.dividerColor }

            RowLayout {
                anchors.fill: parent
                spacing: 0
                visible: !pane.selecting
                ToolButton {
                    icon.source: pane.remote ? "icons/server.svg" : "icons/phone.svg"
                    icon.color: pane.active ? Material.accent : Material.foreground
                    Accessible.name: qsTr("Parent folder")
                    enabled: !pane.busy
                    onClicked: { pane.activated(); pane.up() }
                }
                ItemDelegate {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: pane.accessible
                    onClicked: { pane.activated(); pathMenu.popup(0, height) }
                    contentItem: Label {
                        text: pane.remote ? (session.directory || "/") : ("/" + phone.displayDirectory)
                        elide: Text.ElideLeft
                        font.pixelSize: 14
                        font.family: monoFont
                        verticalAlignment: Text.AlignVCenter
                    }
                    Menu {
                        // not under the status or navigation bar (popups cover the whole window)
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        id: pathMenu
                        // the parent folders (nearest first), then the site's bookmarks
                        property var entries: []
                        onAboutToShow: {
                            var list = pane.crumbs.slice().reverse()
                            if (pane.remote) {
                                var marks = session.bookmarks()
                                for (var i = 0; i < marks.length; i++)
                                    list.push({ title: "★ " + marks[i].name, path: marks[i].path })
                                // recently visited folders that are not on the path already
                                var shown = list.map(function(e) { return e.path })
                                var recent = session.recentFolders()
                                for (var j = 0, n = 0; j < recent.length && n < 5; j++) {
                                    if (shown.indexOf(recent[j]) >= 0 || recent[j] === session.directory) continue
                                    // the end of a long path is the telling part
                                    var shown_ = recent[j].length > 30 ? "…" + recent[j].slice(-29) : recent[j]
                                    list.push({ title: "» " + shown_, path: recent[j] })
                                    n++
                                }
                            }
                            entries = list
                        }
                        // Repeater items would go to the end of a Menu: inserted at their place
                        Instantiator {
                            model: pathMenu.entries
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.title
                                onTriggered: pane.cd(modelData.path)
                            }
                            onObjectAdded: function(index, object) { pathMenu.insertItem(index, object) }
                            onObjectRemoved: function(index, object) { pathMenu.removeItem(object) }
                        }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Go to folder…"); onTriggered: gotoDialog.open() }
                        MenuItem {
                            text: qsTr("Bookmark this folder…")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            onTriggered: bookmarkDialog.open()
                        }
                        MenuItem {
                            text: qsTr("Manage bookmarks…")
                            visible: pane.remote && pathMenu.entries.length > pane.crumbs.length
                            height: visible ? implicitHeight : 0
                            onTriggered: bookmarksDialog.open()
                        }
                    }
                }
                ToolButton {
                    icon.source: "icons/search.svg"
                    Accessible.name: qsTr("Filter")
                    enabled: pane.accessible
                    checked: filterBar.visible
                    onClicked: {
                        pane.activated()
                        if (filterBar.visible) pane.closeFilter()
                        else { filterBar.visible = true; filterField.forceActiveFocus() }
                    }
                }
                ToolButton {
                    icon.source: "icons/more.svg"
                    Accessible.name: qsTr("View")
                    enabled: pane.accessible
                    onClicked: { pane.activated(); viewMenu.popup() }
                    Menu {
                        // not under the status or navigation bar (popups cover the whole window)
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        id: viewMenu
                        MenuItem { text: qsTr("Sort by name"); checkable: true; checked: settings.sortBy === 0; onTriggered: settings.sortBy = 0 }
                        MenuItem { text: qsTr("Sort by date"); checkable: true; checked: settings.sortBy === 1; onTriggered: settings.sortBy = 1 }
                        MenuItem { text: qsTr("Sort by size"); checkable: true; checked: settings.sortBy === 2; onTriggered: settings.sortBy = 2 }
                        MenuItem { text: qsTr("Reverse order"); checkable: true; checked: settings.descending; onTriggered: settings.descending = !settings.descending }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Show hidden files"); checkable: true; checked: settings.showHidden; onTriggered: settings.showHidden = !settings.showHidden }
                        MenuItem { text: qsTr("Select all"); onTriggered: pane.files.selectAll() }
                        MenuItem {
                            text: sessions.clipboardMove ? qsTr("Move %1 items here").arg(sessions.clipboardCount)
                                                         : qsTr("Paste %1 items here").arg(sessions.clipboardCount)
                            visible: pane.remote && sessions.canPaste
                            height: visible ? implicitHeight : 0
                            onTriggered: sessions.pasteFiles()
                        }
                        MenuItem {
                            text: qsTr("Find files…")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            onTriggered: stack.push(Qt.resolvedUrl("SearchPage.qml"))
                        }
                        MenuItem { text: qsTr("New file…"); onTriggered: { nameDialog.mode = "file"; nameDialog.open() } }
                        MenuItem { text: qsTr("New folder…"); onTriggered: { nameDialog.mode = "folder"; nameDialog.open() } }
                        MenuItem { text: qsTr("Upload from other apps…"); visible: pane.remote; height: visible ? implicitHeight : 0; onTriggered: uploadDialog.open() }
                        MenuItem { text: qsTr("Refresh"); onTriggered: pane.refresh() }
                    }
                }
            }

            // the selection: count and what can be done with it
            RowLayout {
                anchors.fill: parent
                spacing: 0
                visible: pane.selecting
                ToolButton { icon.source: "icons/close.svg"; Accessible.name: qsTr("Clear selection"); onClicked: pane.files.clearSelection() }
                Label { text: qsTr("%1 selected").arg(pane.files.selectedCount); font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                ToolButton {
                    icon.source: pane.remote ? "icons/download.svg" : "icons/upload.svg"
                    Accessible.name: pane.remote ? qsTr("Download") : qsTr("Upload")
                    enabled: !session.busy && (pane.remote || session.connected)
                    onClicked: pane.transfer(pane.files.selectedNames())
                }
                ToolButton {
                    icon.source: "icons/share.svg"
                    Accessible.name: qsTr("Share")
                    enabled: !pane.files.selectedHasFolder && !session.busy
                    onClicked: pane.share(pane.files.selectedNames())
                }
                ToolButton {
                    icon.source: "icons/delete.svg"
                    Accessible.name: qsTr("Delete")
                    enabled: !session.busy || !pane.remote
                    onClicked: pane.askRemove(pane.files.selectedNames())
                }
                ToolButton {
                    icon.source: "icons/more.svg"
                    Accessible.name: qsTr("More")
                    onClicked: selectionMenu.popup()
                    Menu {
                        // not under the status or navigation bar (popups cover the whole window)
                        topMargin: insets.top + 4
                        bottomMargin: insets.bottom + 4
                        id: selectionMenu
                        MenuItem { text: qsTr("Select all"); onTriggered: pane.files.selectAll() }
                        MenuItem {
                            text: qsTr("Copy (paste in another folder)")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            onTriggered: { sessions.copyFiles(pane.files.selectedNames(), false); pane.files.clearSelection() }
                        }
                        MenuItem {
                            text: qsTr("Cut (move to another folder)")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            onTriggered: { sessions.copyFiles(pane.files.selectedNames(), true); pane.files.clearSelection() }
                        }
                        MenuItem {
                            text: qsTr("Copy to /%1").arg(phone.displayDirectory)
                            visible: pane.remote && pane.phoneElsewhere
                            height: visible ? implicitHeight : 0
                            enabled: !session.busy
                            onTriggered: pane.copyToPhone(pane.files.selectedNames())
                        }
                        MenuItem {
                            text: qsTr("Rename…")
                            enabled: pane.files.selectedCount === 1
                            onTriggered: { nameDialog.mode = "rename"; nameDialog.original = pane.files.selectedNames()[0]; nameDialog.open() }
                        }
                        MenuItem {
                            text: qsTr("Open")
                            enabled: pane.files.selectedCount === 1 && !pane.files.selectedHasFolder
                            onTriggered: { var n = pane.files.selectedNames()[0]; pane.files.clearSelection(); pane.openFile(n) }
                        }
                        MenuItem {
                            text: qsTr("Download as archive")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            enabled: !session.busy
                            onTriggered: pane.archive(pane.files.selectedNames())
                        }
                        MenuItem {
                            text: qsTr("Properties…")
                            visible: pane.remote
                            height: visible ? implicitHeight : 0
                            onTriggered: pane.properties(pane.files.selectedNames())
                        }
                    }
                }
            }
        }

        // ---- filter as you type ---------------------------------------------
        Rectangle {
            id: filterBar
            visible: false
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Material.dialogColor
            TextField {
                id: filterField
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                placeholderText: qsTr("Filter this folder")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                text: pane.files.filter
                onTextEdited: pane.files.filter = text
                onAccepted: Qt.inputMethod.hide()
                // a focused field would take Back for itself
                Keys.onBackPressed: function(event) { pane.closeFilter(); event.accepted = true }
            }
        }

        // ---- what can be put here: copied / cut files, shared files ----
        Pane {
            Layout.fillWidth: true
            visible: pane.remote && session.connected && (sessions.canPaste || androidFiles.sharedFiles.length > 0)
            padding: 4
            leftPadding: 12
            background: Rectangle { color: Qt.tint(Material.dialogColor, Qt.rgba(Material.accent.r, Material.accent.g, Material.accent.b, 0.15)) }
            RowLayout {
                anchors.fill: parent
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: androidFiles.sharedFiles.length > 0
                          ? qsTr("%1 shared files to upload").arg(androidFiles.sharedFiles.length)
                          : (sessions.clipboardMove ? qsTr("%1 items to move").arg(sessions.clipboardCount)
                                                    : qsTr("%1 items to paste").arg(sessions.clipboardCount))
                }
                Button {
                    flat: true
                    text: androidFiles.sharedFiles.length > 0 ? qsTr("Upload here") : (sessions.clipboardMove ? qsTr("Move here") : qsTr("Paste here"))
                    enabled: !session.busy
                    onClicked: androidFiles.sharedFiles.length > 0 ? pane.uploadShared() : sessions.pasteFiles()
                }
                ToolButton {
                    icon.source: "icons/close.svg"
                    Accessible.name: qsTr("Cancel")
                    onClicked: androidFiles.sharedFiles.length > 0 ? androidFiles.clearShared() : sessions.clearClipboard()
                }
            }
        }

        // ---- the files --------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                visible: pane.accessible
                model: pane.files
                clip: true
                bottomMargin: 96
                onMovementStarted: { pane.activated(); Qt.inputMethod.hide() }
                // pull down to refresh
                onDragEnded: if (contentY < -80 && !pane.busy) pane.refresh()
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: Math.max(8, -list.contentY - height - 8)
                    visible: list.contentY < -20
                    text: list.contentY < -80 ? qsTr("Release to refresh") : qsTr("Pull to refresh")
                    opacity: 0.6
                    font.pixelSize: 12
                }
                delegate: ItemDelegate {
                    id: row
                    required property string name
                    required property bool isDir
                    required property bool isLink
                    required property string size
                    required property string date
                    required property string rights
                    required property bool selected
                    required property string thumbnail
                    required property string preview
                    width: ListView.view.width
                    enabled: !pane.busy
                    highlighted: selected
                    contentItem: RowLayout {
                        spacing: 12
                        Item {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            Image {
                                anchors.centerIn: parent
                                visible: !row.selected && thumb.status !== Image.Ready
                                source: row.isDir ? "icons/folder.svg" : "icons/file.svg"
                                sourceSize: Qt.size(28, 28)
                            }
                            // an image's thumbnail (phone: the file; server: fetched in the background)
                            Image {
                                id: thumb
                                anchors.fill: parent
                                visible: !row.selected && status === Image.Ready
                                source: settings.thumbnails ? row.thumbnail : ""
                                sourceSize: Qt.size(80, 80)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                            }
                            Rectangle {
                                anchors.fill: parent
                                visible: row.selected
                                radius: width / 2
                                color: Material.accent
                                Image { anchors.centerIn: parent; source: "icons/check.svg"; sourceSize: Qt.size(20, 20) }
                            }
                            // tap the icon to select (as in the phone's Files app)
                            MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: { pane.activated(); pane.files.toggle(row.name) } }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: row.name + (row.isLink ? " ↗" : ""); font.pixelSize: 15; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            Label {
                                text: (row.isDir ? "" : row.size + " · ") + row.date + (pane.remote ? " · " + row.rights : "")
                                font.pixelSize: 12
                                opacity: 0.6
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                    onClicked: {
                        pane.activated()
                        if (pane.selecting) pane.files.toggle(name)
                        else if (isDir) pane.enter(name)
                        else actions.show(name)
                    }
                    onPressAndHold: { pane.activated(); pane.files.toggle(name) }
                }
                ScrollIndicator.vertical: ScrollIndicator {}
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: pane.files.total > 0 ? qsTr("No files match \"%1\"").arg(pane.files.filter)
                                           : qsTr("Empty folder")
                opacity: 0.6
                visible: pane.accessible && list.count === 0 && !pane.busy
            }

            // the phone's files need a permission first
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 420)
                visible: !pane.accessible
                spacing: 12
                IconImage { Layout.alignment: Qt.AlignHCenter; source: "icons/phone.svg"; sourceSize: Qt.size(48, 48); color: Material.foreground; opacity: 0.6 }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: 16
                    text: qsTr("Allow NixSCP to see the phone's files to copy them to and from the server.")
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    font.pixelSize: 13
                    text: qsTr("Android opens its setting \"All files access\"; switch it on for NixSCP and come back. Without it, downloads go to Download/NixSCP.")
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Allow access")
                    highlighted: true
                    onClicked: phone.requestAccess()
                }
            }
        }
    }

    // ---- per file actions (a tap on a file) ----------------------------------
    Menu {
        // not under the status or navigation bar (popups cover the whole window)
        topMargin: insets.top + 4
        bottomMargin: insets.bottom + 4
        id: actions
        property string file
        function show(name) { file = name; popup() }
        MenuItem { text: actions.file; enabled: false }
        MenuItem {
            text: qsTr("Preview")
            visible: pane.files.previewKind(actions.file) !== ""
            height: visible ? implicitHeight : 0
            onTriggered: pane.preview(actions.file)
        }
        MenuItem { text: qsTr("Open"); onTriggered: pane.openFile(actions.file) }
        MenuItem {
            text: qsTr("Edit")
            visible: pane.remote
            height: visible ? implicitHeight : 0
            onTriggered: pane.edit(actions.file)
        }
        MenuItem {
            text: qsTr("Extract here")
            visible: pane.remote && session.canRunCommands && session.isArchive(actions.file)
            height: visible ? implicitHeight : 0
            onTriggered: session.extract(actions.file)
        }
        MenuItem { text: qsTr("Share…"); onTriggered: pane.share([actions.file]) }
        MenuItem {
            text: pane.remote ? qsTr("Download") : qsTr("Upload to the server")
            enabled: pane.remote || session.connected
            onTriggered: pane.transfer([actions.file])
        }
        MenuItem {
            text: qsTr("Copy to /%1").arg(phone.displayDirectory)
            visible: pane.remote && pane.phoneElsewhere
            height: visible ? implicitHeight : 0
            onTriggered: pane.copyToPhone([actions.file])
        }
        MenuItem { text: qsTr("Rename…"); onTriggered: { nameDialog.mode = "rename"; nameDialog.original = actions.file; nameDialog.open() } }
        MenuItem {
            text: qsTr("Download as archive")
            visible: pane.remote
            height: visible ? implicitHeight : 0
            onTriggered: pane.archive([actions.file])
        }
        MenuItem {
            text: qsTr("Properties…")
            visible: pane.remote
            height: visible ? implicitHeight : 0
            onTriggered: pane.properties([actions.file])
        }
        MenuItem {
            text: qsTr("Copy (paste in another folder)")
            visible: pane.remote
            height: visible ? implicitHeight : 0
            onTriggered: sessions.copyFiles([actions.file], false)
        }
        MenuItem {
            text: qsTr("Cut (move to another folder)")
            visible: pane.remote
            height: visible ? implicitHeight : 0
            onTriggered: sessions.copyFiles([actions.file], true)
        }
        MenuItem { text: qsTr("Select"); onTriggered: pane.files.toggle(actions.file) }
        MenuItem { text: qsTr("Delete…"); onTriggered: pane.askRemove([actions.file]) }
    }

    Dialog {
        id: nameDialog
        property string mode: "folder"
        property string original
        parent: Overlay.overlay
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: mode === "rename" ? qsTr("Rename") : (mode === "file" ? qsTr("New file") : qsTr("New folder"))
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { field.text = mode === "rename" ? original : ""; field.forceActiveFocus(); field.selectAll() }
        onAccepted: mode === "rename" ? pane.rename(original, field.text)
                    : (mode === "file" ? pane.createFile(field.text) : pane.createFolder(field.text))
        TextField {
            id: field
            width: nameDialog.availableWidth
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            onAccepted: nameDialog.accept()
        }
    }

    Dialog {
        id: gotoDialog
        parent: Overlay.overlay
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: qsTr("Go to folder")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: {
            gotoField.text = pane.remote ? session.directory : phone.displayDirectory
            gotoField.forceActiveFocus()
            gotoField.selectAll()
        }
        onAccepted: if (gotoField.text.trim().length > 0 || !pane.remote) pane.cd(gotoField.text.trim())
        ColumnLayout {
            width: parent.width
            TextField {
                id: gotoField
                Layout.fillWidth: true
                font.family: monoFont
                placeholderText: pane.remote ? "/var/www" : "Download"
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                onAccepted: gotoDialog.accept()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                font.pixelSize: 12
                text: pane.remote ? qsTr("A folder on the server, e.g. /var/www or ~/logs")
                                  : qsTr("A folder of the phone's storage, e.g. Download or DCIM/Camera")
            }
        }
    }

    Dialog {
        id: deleteDialog
        property var names: []
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: names.length === 1 ? qsTr("Delete?") : qsTr("Delete %1 items?").arg(names.length)
        standardButtons: Dialog.Cancel | Dialog.Yes
        Label {
            width: deleteDialog.availableWidth
            wrapMode: Text.Wrap
            text: deleteDialog.names.slice(0, 8).join("\n") + (deleteDialog.names.length > 8 ? "\n…" : "")
                  + (pane.remote ? "" : "\n\n" + qsTr("From the phone; this cannot be undone."))
        }
        onAccepted: pane.remove(names)
    }

    Dialog {
        id: bookmarkDialog
        parent: Overlay.overlay
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: qsTr("Bookmark this folder")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: {
            var dir = session.directory
            bookmarkName.text = dir === "/" ? "/" : dir.substring(dir.lastIndexOf("/") + 1)
            bookmarkName.forceActiveFocus()
            bookmarkName.selectAll()
        }
        onAccepted: if (session.addBookmark(bookmarkName.text, session.directory)) app.toast(qsTr("Bookmark added."))
        ColumnLayout {
            width: bookmarkDialog.availableWidth
            Label { text: session.directory; font.family: monoFont; opacity: 0.7; elide: Text.ElideLeft; Layout.fillWidth: true }
            TextField {
                id: bookmarkName
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
                onAccepted: bookmarkDialog.accept()
            }
        }
    }

    Dialog {
        id: bookmarksDialog
        property var marks: []
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: qsTr("Bookmarks of %1").arg(session.site)
        standardButtons: Dialog.Close
        onAboutToShow: marks = session.bookmarks()
        ListView {
            implicitWidth: bookmarksDialog.availableWidth
            implicitHeight: Math.min(contentHeight, 360)
            clip: true
            model: bookmarksDialog.marks
            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width
                contentItem: RowLayout {
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { text: modelData.path; opacity: 0.6; font.pixelSize: 12; font.family: monoFont; Layout.fillWidth: true; elide: Text.ElideLeft }
                    }
                    ToolButton {
                        icon.source: "icons/delete.svg"
                        Accessible.name: qsTr("Delete bookmark")
                        onClicked: { session.removeBookmark(modelData.name); bookmarksDialog.marks = session.bookmarks() }
                    }
                }
                onClicked: { bookmarksDialog.close(); pane.cd(modelData.path) }
            }
        }
    }

    // rights (checkboxes / octal), owner and group of the files
    Dialog {
        id: propertiesDialog
        property var names: []
        property bool hasFolder: false
        property int mode: 0
        property string owner
        property string group
        function show(list) {
            names = list
            var first = pane.files.entry(list[0])
            hasFolder = false
            for (var i = 0; i < list.length; i++)
                if (pane.files.isDir(list[i])) hasFolder = true
            mode = first.mode
            owner = first.owner
            group = first.group
            ownerField.text = owner
            groupField.text = group
            recursive.checked = false
            octal.text = ("000" + mode.toString(8)).slice(-4)
            open()
        }
        parent: Overlay.overlay
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        // centred, above the keyboard
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 24, 460)
        modal: true
        title: names.length === 1 ? names[0] : qsTr("%1 items").arg(names.length)
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: {
            session.setProperties(names, mode,
                                  ownerField.text !== owner ? ownerField.text : "",
                                  groupField.text !== group ? groupField.text : "",
                                  recursive.checked)
            pane.files.clearSelection()
        }
        ColumnLayout {
            width: propertiesDialog.availableWidth
            spacing: 4
            GridLayout {
                columns: 4
                columnSpacing: 0
                rowSpacing: 0
                Label { text: "" }
                Label { text: qsTr("Read"); horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                Label { text: qsTr("Write"); horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                Label { text: qsTr("Execute"); horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                Repeater {
                    // owner, group, others: rows of 3 bits
                    model: [qsTr("Owner"), qsTr("Group"), qsTr("Others")]
                    delegate: Label { required property string modelData; required property int index; text: modelData; Layout.row: index + 1; Layout.column: 0 }
                }
                Repeater {
                    model: 9
                    delegate: CheckBox {
                        required property int index
                        readonly property int bit: 1 << (8 - index)
                        Layout.row: Math.floor(index / 3) + 1
                        Layout.column: index % 3 + 1
                        Layout.alignment: Qt.AlignHCenter
                        checked: (propertiesDialog.mode & bit) !== 0
                        onToggled: {
                            propertiesDialog.mode = checked ? (propertiesDialog.mode | bit) : (propertiesDialog.mode & ~bit)
                            octal.text = ("000" + propertiesDialog.mode.toString(8)).slice(-4)
                        }
                    }
                }
            }
            RowLayout {
                Label { text: qsTr("Octal"); opacity: 0.7 }
                TextField {
                    id: octal
                    Layout.preferredWidth: 90
                    font.family: monoFont
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: RegularExpressionValidator { regularExpression: /[0-7]{3,4}/ }
                    onTextEdited: if (acceptableInput) propertiesDialog.mode = parseInt(text, 8)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                TextField { id: ownerField; Layout.fillWidth: true; placeholderText: qsTr("owner"); inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText }
                TextField { id: groupField; Layout.fillWidth: true; placeholderText: qsTr("group"); inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText }
            }
            CheckBox {
                id: recursive
                visible: propertiesDialog.hasFolder
                text: qsTr("Also everything inside the folders")
            }
        }
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Files to upload")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++)
                paths.push(selectedFiles[i].toString())
            session.upload(paths)
        }
    }
}
