// A quick look at a file without another app: an image (pinch or double
// tap to zoom) or a text (read only). Share and Open hand it on.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: page
    objectName: "preview"
    property string localPath
    property string name
    // "image" or "text"
    property string kind: "image"
    property string text: ""

    Component.onCompleted: {
        if (kind === "text") {
            var file = session.readText(localPath)
            if (file.error) { app.toast(file.error); Qt.callLater(function() { page.StackView.view.pop() }) }
            else text = file.text
        }
    }

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
                onClicked: if (!androidFiles.share([page.localPath])) app.toast(qsTr("This file cannot be shared."))
            }
            ToolButton {
                text: qsTr("Open")
                onClicked: if (!androidFiles.open(page.localPath)) app.toast(qsTr("No app on this phone opens this kind of file."))
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
            source: page.kind === "image" ? "file://" + page.localPath : ""
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
            font.family: monoFont
            font.pixelSize: 13
            text: page.text
            background: null
        }
    }
}
