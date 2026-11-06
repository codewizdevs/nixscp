// A quick look at a file without another app: an image (pinch or double
// tap to zoom) or a text (read only). Share and Open hand it on.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "preview"
    // the session the file came from (the phone's own files: the current one)
    property var owner: session
    property string localPath
    property string name
    // "image" or "text"
    property string kind: "image"
    property string text: ""
    // handed to another app: it may still be reading the file
    property bool handedOver: false

    Component.onCompleted: {
        if (kind === "text") {
            var file = owner.readText(localPath)
            if (file.error) { app.toast(file.error); Qt.callLater(function() { page.StackView.view.pop() }) }
            else text = file.text
        }
    }
    // a server's file does not stay on the phone once it was looked at
    StackView.onRemoved: if (owner && !handedOver) owner.closePreview(localPath)

    header: ToolBar {
        Material.background: Material.primary
        Material.foreground: "white"
        RowLayout {
            anchors.fill: parent
            ToolButton { icon.source: "icons/back.svg"; icon.color: "white"; Accessible.name: qsTr("Back"); onClicked: page.StackView.view.pop() }
            Label { text: page.name; font.pixelSize: 17; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
            ToolButton {
                icon.source: "icons/share.svg"
                icon.color: "white"
                Accessible.name: qsTr("Share")
                onClicked: {
                    page.handedOver = true
                    if (!androidFiles.share([page.localPath])) app.toast(qsTr("This file cannot be shared."))
                }
            }
            ToolButton {
                text: qsTr("Open")
                onClicked: {
                    page.handedOver = true
                    if (!androidFiles.open(page.localPath)) app.toast(qsTr("No app on this phone opens this kind of file."))
                }
            }
        }
    }

    // ---- image ----
    Flickable {
        id: flick
        anchors.fill: parent
        visible: page.kind === "image"
        clip: true
        contentWidth: Math.max(width, image.width * image.scale)
        contentHeight: Math.max(height, image.height * image.scale)
        Rectangle { anchors.fill: parent; color: "black"; z: -1 }
        Image {
            id: image
            // as a URL: a name may hold # ? %
            source: page.kind === "image" ? androidFiles.fileUrl(page.localPath) : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            width: flick.width
            height: flick.height
            transformOrigin: Item.TopLeft
            x: Math.max(0, (flick.contentWidth - width * scale) / 2)
            y: Math.max(0, (flick.contentHeight - height * scale) / 2)
        }
        PinchHandler {
            target: image
            minimumScale: 1
            maximumScale: 8
            xAxis.enabled: false
            yAxis.enabled: false
            rotationAxis.enabled: false
        }
        TapHandler {
            onDoubleTapped: image.scale = image.scale > 1 ? 1 : 2.5
        }
    }
    BusyIndicator { anchors.centerIn: parent; running: page.kind === "image" && image.status === Image.Loading }
    Label {
        anchors.centerIn: parent
        visible: page.kind === "image" && image.status === Image.Error
        text: qsTr("This image cannot be shown.")
    }

    // ---- text ----
    Flickable {
        anchors.fill: parent
        visible: page.kind === "text"
        clip: true
        contentWidth: width
        contentHeight: textArea.implicitHeight
        ScrollBar.vertical: ScrollBar {}
        TextArea {
            id: textArea
            width: parent.width
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.PlainText
            font.family: monoFont
            font.pixelSize: 13
            text: page.text
            background: null
        }
    }
}
