import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

DrawerType2 {
    id: root

    anchors.fill: parent
    expandedHeight: parent.height * 0.9

    expandedStateContent: ColumnLayout {
        id: content

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        Header2Type {
            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.rightMargin: 16
            Layout.leftMargin: 16
            Layout.bottomMargin: 16

            headerText: qsTr("Connection details")
            descriptionText: qsTr("Live statistics of the current tunnel")
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Server")
            descriptionText: ServersUiController.defaultServerName
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Protocol")
            descriptionText: ServersUiController.defaultServerDefaultContainerName
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Last handshake")
            descriptionText: ConnectionStatsController.lastHandshakeText
            descriptionColor: ConnectionStatsController.isHandshakeStale ? AmneziaStyle.color.goldenApricot
                                                                         : AmneziaStyle.color.mutedGray
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("This session")
            descriptionText: qsTr("Received %1 · Sent %2").arg(ConnectionStatsController.rxTotalText)
                                                           .arg(ConnectionStatsController.txTotalText)
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Current speed")
            descriptionText: "↓ " + ConnectionStatsController.rxRateText + "  ↑ " + ConnectionStatsController.txRateText
        }

        DividerType {
        }

        BasicButtonType {
            id: copyButton

            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.bottomMargin: 24
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            text: qsTr("Copy diagnostics")

            Keys.onEnterPressed: copyButton.clicked()
            Keys.onReturnPressed: copyButton.clicked()

            clickedFunc: function() {
                ConnectionStatsController.copyReport()
                PageController.showNotificationMessage(qsTr("Copied"))
            }
        }
    }
}
