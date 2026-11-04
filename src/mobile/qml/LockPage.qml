// The lock of NixSCP Pro's encrypted configuration: the NixSCP password (or
// the recovery key), or the fingerprint when it is turned on (asked right
// away). Covers the whole app; transfers go on behind it.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: lockPage
    color: Material.backgroundColor
    property bool recovery: false
    property string error: ""

    function tryUnlock() {
        error = lock.unlock(field.text, recovery)
        if (error.length === 0) {
            field.text = ""
            Qt.inputMethod.hide()
        }
    }
    function fingerprint() {
        error = ""
        lock.unlockWithBiometric()
    }

    // the fingerprint at once when it is turned on
    onVisibleChanged: if (visible && lock.biometricEnabled) Qt.callLater(fingerprint)
    Component.onCompleted: if (visible && lock.biometricEnabled) Qt.callLater(fingerprint)

    Connections {
        target: lock
        function onBiometricFinished(ok, message) {
            if (!ok && message.length > 0) lockPage.error = message
        }
    }

    // taps do not reach the app behind
    MouseArea { anchors.fill: parent }

    Flickable {
        anchors.fill: parent
        anchors.topMargin: insets.top
        anchors.bottomMargin: Math.max(insets.bottom, app.keyboardHeight)
        contentHeight: Math.max(height, column.implicitHeight + 48)
        clip: true

        ColumnLayout {
            id: column
            width: Math.min(parent.width - 48, 420)
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(24, (parent.height - implicitHeight) / 3)
            spacing: 14

            Image {
                Layout.alignment: Qt.AlignHCenter
                source: "nixscp-128.png"
                sourceSize: Qt.size(72, 72)
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 20
                font.bold: true
                text: qsTr("NixSCP is locked")
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.7
                text: lockPage.recovery ? qsTr("Enter the recovery key you saved when you set up NixSCP Pro.")
                                        : qsTr("Your sites and passwords are encrypted. Enter your NixSCP password.")
            }
            TextField {
                id: field
                Layout.fillWidth: true
                echoMode: lockPage.recovery ? TextInput.Normal : TextInput.Password
                placeholderText: lockPage.recovery ? qsTr("Recovery key") : qsTr("NixSCP password")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                onAccepted: lockPage.tryUnlock()
                onTextEdited: lockPage.error = ""
            }
            Label {
                Layout.fillWidth: true
                visible: lockPage.error.length > 0
                wrapMode: Text.Wrap
                color: Material.color(Material.Red)
                text: lockPage.error
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Unlock")
                highlighted: true
                enabled: field.text.length > 0
                onClicked: lockPage.tryUnlock()
            }
            Button {
                Layout.fillWidth: true
                visible: lock.biometricEnabled && !lockPage.recovery
                flat: true
                text: qsTr("Use fingerprint")
                icon.source: "icons/key.svg"
                onClicked: lockPage.fingerprint()
            }
            Button {
                Layout.alignment: Qt.AlignHCenter
                flat: true
                text: lockPage.recovery ? qsTr("Use the password") : qsTr("Forgot it? Use the recovery key")
                onClicked: { lockPage.recovery = !lockPage.recovery; field.text = ""; lockPage.error = "" }
            }
        }
    }
}
