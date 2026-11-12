// The saved sites: search as you type (name, host, user), Favourites and
// Recent on top, folders that fold. Tap to connect; the star pins a
// favourite; long press for Edit, Duplicate, Delete. + adds a site, the menu
// adds a note or imports a WinSCP.ini. Note items (a note with no server)
// open as a note and never connect.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
// IconImage: icons in the theme's text colour
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

Page {
    id: page
    objectName: "sites"
    // opened over the browser for another tab: Back returns to it
    readonly property bool picker: StackView.index > 0
    readonly property bool searching: sites.filter.trim().length > 0
    // Android's keyboard height (Qt's on Android 9/10)
    readonly property real keyboardHeight: insets.keyboard || (Qt.inputMethod.visible ? Qt.inputMethod.keyboardRectangle.height / Screen.devicePixelRatio : 0)

    // Back: clears the search first
    function back() {
        if (searchField.text.length > 0) {
            searchField.text = ""
            sites.filter = ""
            Qt.inputMethod.hide()
            return true
        }
        return false
    }
    function edit(name) {
        if (name.length > 0 && sites.isNote(name))
            page.StackView.view.push(Qt.resolvedUrl("NotePage.qml"), { name: name })
        else
            page.StackView.view.push(Qt.resolvedUrl("SitePage.qml"), { original: name })
    }

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton {
                visible: page.picker
                icon.source: "icons/back.svg"
                icon.color: "white"
                Accessible.name: qsTr("Back")
                onClicked: page.StackView.view.pop()
            }
            Image {
                visible: !page.picker
                source: "nixscp-128.png"
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                Layout.leftMargin: 12
            }
            Label { text: page.picker ? qsTr("Open in a new tab") : "NixSCP"; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true }
            ToolButton {
                icon.source: "icons/more.svg"
                icon.color: "white"
                Accessible.name: qsTr("Menu")
                onClicked: menu.open()
                Menu {
                    // not under the status or navigation bar (popups cover the whole window)
                    topMargin: insets.top + 4
                    bottomMargin: insets.bottom + 4
                    id: menu
                    MenuItem { text: qsTr("NixSCP Pro"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("ProPage.qml")) }
                    MenuItem { text: qsTr("Teams"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("TeamsPage.qml")) }
                    MenuItem { text: qsTr("Settings"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml")) }
                    MenuItem { text: qsTr("SSH keys"); onTriggered: page.StackView.view.push(Qt.resolvedUrl("KeysPage.qml")) }
                    MenuItem { text: qsTr("New note…"); onTriggered: newNoteDialog.open() }
                    MenuItem { text: qsTr("Import WinSCP.ini…"); onTriggered: importDialog.open() }
                    MenuItem { text: qsTr("About NixSCP"); onTriggered: aboutDialog.open() }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- files shared to NixSCP: pick the site to upload them to ----
        Pane {
            Layout.fillWidth: true
            visible: androidFiles.sharedFiles.length > 0
            padding: 8
            leftPadding: 16
            background: Rectangle { color: Qt.tint(Material.dialogColor, Qt.rgba(Material.accent.r, Material.accent.g, Material.accent.b, 0.2)) }
            RowLayout {
                anchors.fill: parent
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: androidFiles.sharedFiles.length === 1
                          ? qsTr("Upload %1: choose a site, then the folder.").arg(androidFiles.sharedNames[0])
                          : qsTr("Upload %1 shared files: choose a site, then the folder.").arg(androidFiles.sharedFiles.length)
                }
                ToolButton { icon.source: "icons/close.svg"; Accessible.name: qsTr("Cancel"); onClicked: androidFiles.clearShared() }
            }
        }

        // ---- search -------------------------------------------------------
        Pane {
            Layout.fillWidth: true
            visible: sites.count > 0
            padding: 8
            leftPadding: 12
            rightPadding: 12
            Material.elevation: 2
            RowLayout {
                anchors.fill: parent
                spacing: 4
                IconImage { source: "icons/search.svg"; sourceSize: Qt.size(22, 22); color: Material.foreground; opacity: 0.6 }
                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search %1 sites").arg(sites.count)
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onTextEdited: sites.filter = text
                    // a focused field would take Back for itself (with the
                    // keyboard open, the keyboard takes it first)
                    Keys.onBackPressed: function(event) {
                        searchField.focus = false
                        event.accepted = page.back()
                    }
                    // Enter connects to the only match
                    onAccepted: {
                        Qt.inputMethod.hide()
                        if (sites.matches === 1) {
                            for (var i = 0; i < list.count; i++) {
                                var item = list.itemAtIndex(i)
                                if (item && !item.isHeader) { app.openSite(item.name); break }
                            }
                        }
                    }
                }
                ToolButton {
                    icon.source: "icons/close.svg"
                    visible: searchField.text.length > 0
                    Accessible.name: qsTr("Clear the search")
                    onClicked: page.back()
                }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: sites
            clip: true
            enabled: !session.busy
            bottomMargin: 96 + page.keyboardHeight
            onMovementStarted: Qt.inputMethod.hide()
            // the list is rebuilt for a star, a folded section or new sites: it stays where it was
            property real keptY: 0
            Connections {
                target: sites
                function onModelAboutToBeReset() { list.keptY = list.contentY }
                function onModelReset() { Qt.callLater(function() { list.contentY = Math.min(list.keptY, Math.max(list.originY, list.originY + list.contentHeight - list.height)) }) }
            }
            delegate: Item {
                id: row
                required property bool isHeader
                required property string name
                required property string title
                required property string host
                required property string user
                required property string protocol
                required property string section
                required property bool collapsed
                required property int siteCount
                required property bool favourite
                // a note item (no server); a site with a note; its first line
                required property bool noteItem
                required property bool hasNote
                required property string noteLine
                width: ListView.view.width
                height: isHeader ? headerRow.height : siteRow.height

                // a section: Favourites, Recent or a folder; tap folds it
                ItemDelegate {
                    id: headerRow
                    visible: row.isHeader
                    width: parent.width
                    height: visible ? 44 : 0
                    enabled: !page.searching
                    onClicked: sites.toggleSection(row.section)
                    contentItem: RowLayout {
                        spacing: 8
                        IconImage {
                            source: "icons/chevron.svg"
                            color: Material.foreground
                            sourceSize: Qt.size(18, 18)
                            rotation: row.collapsed ? -90 : 0
                            visible: !page.searching
                            opacity: 0.7
                        }
                        Label {
                            text: row.title
                            font.bold: true
                            color: Material.accent
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label { text: row.siteCount; opacity: 0.6; font.pixelSize: 13 }
                    }
                }

                ItemDelegate {
                    id: siteRow
                    visible: !row.isHeader
                    width: parent.width
                    height: visible ? implicitHeight : 0
                    leftPadding: 24
                    onClicked: app.openSite(row.name)
                    onPressAndHold: row.noteItem ? noteMenu.show(row.name, row.favourite) : siteMenu.show(row.name, row.favourite)
                    contentItem: RowLayout {
                        spacing: 4
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: row.title; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                // a note item: its icon and the note's first line
                                IconImage {
                                    visible: row.noteItem
                                    source: "icons/note.svg"
                                    sourceSize: Qt.size(15, 15)
                                    color: Material.accent
                                }
                                Label {
                                    text: row.noteItem ? (row.noteLine || qsTr("Empty note"))
                                                       : row.protocol + " · " + (row.user ? row.user + "@" : "") + row.host
                                    opacity: 0.65
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                // a site with a note
                                IconImage {
                                    visible: row.hasNote && !row.noteItem
                                    source: "icons/note.svg"
                                    sourceSize: Qt.size(14, 14)
                                    color: Material.foreground
                                    opacity: 0.5
                                    Accessible.name: qsTr("Has a note")
                                }
                            }
                        }
                        ToolButton {
                            icon.source: row.favourite ? "icons/star.svg" : "icons/star-outline.svg"
                            icon.color: row.favourite ? Material.accent : Material.hintTextColor
                            Accessible.name: row.favourite ? qsTr("Remove from favourites") : qsTr("Add to favourites")
                            onClicked: sites.toggleFavourite(row.name)
                        }
                    }
                }
            }
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 64
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        opacity: 0.7
        visible: page.searching && sites.matches === 0
        text: qsTr("No sites match \"%1\".").arg(sites.filter.trim())
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 64
        visible: sites.count === 0
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
            text: qsTr("Add an SFTP, SCP, FTP, WebDAV or S3 site with +, or import your WinSCP.ini from the menu.")
        }
    }

    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 64
        height: 64
        icon.source: "icons/add.svg"
        Accessible.name: qsTr("Add a site")
        icon.width: 28
        icon.height: 28
        highlighted: true
        visible: !session.busy && !Qt.inputMethod.visible
        onClicked: page.edit("")
    }

    // ---- connecting, with Cancel ---------------------------------------------
    Rectangle {
        anchors.fill: parent
        color: "#66000000"
        visible: session.connecting
        MouseArea { anchors.fill: parent }   // the list stays untouched meanwhile
        Pane {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            Material.elevation: 8
            padding: 20
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                RowLayout {
                    spacing: 16
                    BusyIndicator { running: session.connecting; Layout.preferredWidth: 40; Layout.preferredHeight: 40 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: session.site; font.bold: true; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label { text: session.status; opacity: 0.7; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    }
                }
                Button {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: session.cancel()
                }
            }
        }
    }

    // ---- a site's actions (long press) ----------------------------------------
    Menu {
        // not under the status or navigation bar (popups cover the whole window)
        topMargin: insets.top + 4
        bottomMargin: insets.bottom + 4
        id: siteMenu
        property string site
        property bool favourite
        function show(name, fav) { site = name; favourite = fav; popup() }
        MenuItem { text: siteMenu.site; enabled: false }
        MenuItem { text: qsTr("Connect"); onTriggered: sessions.open(siteMenu.site) }
        MenuItem { text: qsTr("Edit…"); onTriggered: page.edit(siteMenu.site) }
        MenuItem {
            text: qsTr("Duplicate…")
            onTriggered: {
                var copy = sites.duplicateSite(siteMenu.site)
                if (copy) page.edit(copy)
            }
        }
        MenuItem { text: qsTr("Check connection"); onTriggered: { checkDialog.report = ""; checkDialog.site = siteMenu.site; checkDialog.open(); checker.check(siteMenu.site) } }
        MenuItem {
            text: qsTr("Add to home screen")
            onTriggered: if (!androidFiles.pinShortcut(siteMenu.site)) app.toast(qsTr("The launcher does not take shortcuts."))
        }
        MenuItem {
            text: siteMenu.favourite ? qsTr("Remove from favourites") : qsTr("Add to favourites")
            onTriggered: sites.toggleFavourite(siteMenu.site)
        }
        MenuItem { text: qsTr("Delete…"); onTriggered: { removeDialog.site = siteMenu.site; removeDialog.open() } }
    }

    // ---- a note item's actions (long press): no connecting, checking or shortcut ----
    Menu {
        // not under the status or navigation bar (popups cover the whole window)
        topMargin: insets.top + 4
        bottomMargin: insets.bottom + 4
        id: noteMenu
        property string site
        property bool favourite
        function show(name, fav) { site = name; favourite = fav; popup() }
        MenuItem { text: noteMenu.site; enabled: false }
        MenuItem { text: qsTr("Open"); onTriggered: page.edit(noteMenu.site) }
        MenuItem {
            text: noteMenu.favourite ? qsTr("Remove from favourites") : qsTr("Add to favourites")
            onTriggered: sites.toggleFavourite(noteMenu.site)
        }
        MenuItem { text: qsTr("Delete…"); onTriggered: { removeDialog.site = noteMenu.site; removeDialog.open() } }
    }

    // "New note…": a note in the site list, with no server behind it
    Dialog {
        id: newNoteDialog
        anchors.centerIn: undefined
        x: (parent.width - width) / 2
        y: Math.max(8, Math.min((parent.height - height) / 2, parent.height - app.keyboardHeight - height - 8))
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("New note")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { newNoteField.text = ""; newNoteField.forceActiveFocus() }
        onAccepted: {
            if (newNoteField.text.trim().length === 0)
                return
            var name = sites.newNote(newNoteField.text)
            if (name)
                page.StackView.view.push(Qt.resolvedUrl("NotePage.qml"), { name: name, startEditing: true })
        }
        ColumnLayout {
            width: newNoteDialog.availableWidth
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("A note in your site list, with no server behind it: office network details, who to call, anything that belongs with your sites.")
            }
            TextField {
                id: newNoteField
                Layout.fillWidth: true
                placeholderText: qsTr("Note name")
                inputMethodHints: Qt.ImhNoPredictiveText
                onAccepted: newNoteDialog.accept()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.6
                font.pixelSize: 12
                text: qsTr("Use / for folders, e.g. Office/Network.")
            }
        }
    }

    // "Check connection": the name, the port, the server's greeting
    Dialog {
        id: checkDialog
        property string site
        property string report
        property bool ok: false
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 480)
        modal: true
        title: site
        standardButtons: Dialog.Close
        onClosed: checker.stop()
        Connections {
            target: checker
            function onFinished(site, ok, report) {
                if (site === checkDialog.site) {
                    checkDialog.ok = ok
                    checkDialog.report = report
                }
            }
        }
        ColumnLayout {
            width: checkDialog.availableWidth
            spacing: 8
            RowLayout {
                visible: checker.checking
                BusyIndicator { running: checker.checking; Layout.preferredWidth: 32; Layout.preferredHeight: 32 }
                Label { text: qsTr("Checking…") }
            }
            Label {
                Layout.fillWidth: true
                visible: checkDialog.report.length > 0
                text: checkDialog.report
                wrapMode: Text.Wrap
                font.family: monoFont
                font.pixelSize: 13
            }
            Label {
                Layout.fillWidth: true
                visible: !checker.checking && checkDialog.report.length > 0
                wrapMode: Text.Wrap
                font.bold: true
                color: checkDialog.ok ? Material.color(Material.Green) : Material.color(Material.Red)
                text: checkDialog.ok ? qsTr("The server answers. If connecting fails, check the user, password or key.")
                                     : qsTr("The server cannot be reached from this phone.")
            }
        }
    }

    Dialog {
        id: removeDialog
        property string site
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 420)
        modal: true
        title: sites.isNote(site) ? qsTr("Delete note?") : qsTr("Delete site?")
        Label { text: removeDialog.site; wrapMode: Text.Wrap; width: removeDialog.availableWidth }
        standardButtons: Dialog.Cancel | Dialog.Yes
        onAccepted: sites.removeSite(site)
    }

    FileDialog {
        id: importDialog
        title: qsTr("WinSCP.ini to import")
        // Android's picker filters by MIME type, and *.ini maps to one that
        // downloaded .ini files never have (they are octet-stream or
        // text/plain): every file would be greyed out. The import checks it.
        nameFilters: Qt.platform.os === "android" ? [] : [qsTr("WinSCP configuration (*.ini)"), qsTr("All files (*)")]
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
            width: aboutDialog.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("WinSCP for Linux, macOS, Windows and Android. Free and open source (GPL-3.0), based on WinSCP. https://nixscp.com")
        }
    }
}
