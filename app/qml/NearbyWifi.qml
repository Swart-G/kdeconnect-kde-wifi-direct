// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
import QtQuick
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kdeconnect

FormCard.FormCard {
    id: root
    property bool searching: stateProperty.value === "searching"
    property bool session: ["connecting", "getting-ip", "verifying", "awaiting-pairing", "connected"].indexOf(stateProperty.value) >= 0
    Connections {
        target: DaemonDbusInterface
        function onWifiDirectChanged() {
            statusProperty.get();
            peersProperty.get();
            stateProperty.get();
            availableProperty.get();
            reasonProperty.get();
            deviceProperty.get();
        }
    }
    DBusProperty {
        id: statusProperty
        object: DaemonDbusInterface
        read: "wifiDirectStatus"
        change: "wifiDirectChanged"
        defaultValue: ""
    }
    DBusProperty {
        id: availableProperty
        object: DaemonDbusInterface
        read: "wifiDirectAvailable"
        change: "wifiDirectChanged"
        defaultValue: false
    }
    DBusProperty {
        id: reasonProperty
        object: DaemonDbusInterface
        read: "wifiDirectUnavailableReason"
        change: "wifiDirectChanged"
        defaultValue: ""
    }
    DBusProperty {
        id: stateProperty
        object: DaemonDbusInterface
        read: "wifiDirectState"
        change: "wifiDirectChanged"
        defaultValue: "unavailable"
    }
    DBusProperty {
        id: peersProperty
        object: DaemonDbusInterface
        read: "wifiDirectPeers"
        change: "wifiDirectChanged"
        defaultValue: []
    }
    DBusProperty {
        id: deviceProperty
        object: DaemonDbusInterface
        read: "wifiDirectConnectedDeviceId"
        change: "wifiDirectChanged"
        defaultValue: ""
    }
    FormCard.FormTextDelegate {
        text: i18n("On your phone, open Nearby connection and make it available for 60 seconds. Find and select the phone here, then accept the system Wi-Fi Direct request. Compare the KDE Connect pairing codes on both devices.")
        textItem.wrapMode: Text.WordWrap
        textItem.elide: Text.ElideNone
    }
    FormCard.FormTextDelegate {
        text: !availableProperty.value ? reasonProperty.value : statusProperty.value
        textItem.wrapMode: Text.WordWrap
        textItem.elide: Text.ElideNone
    }
    FormCard.FormButtonDelegate {
        text: root.searching ? i18n("Stop nearby search") : i18n("Find nearby devices (60 seconds)")
        icon.name: root.searching ? "process-stop" : "network-wireless"
        enabled: !root.session
        onClicked: root.searching ? DaemonDbusInterface.stopWifiDirectDiscovery() : DaemonDbusInterface.startWifiDirectDiscovery()
    }
    FormCard.FormComboBoxDelegate {
        id: peerSelector
        text: i18n("Nearby device (unverified)")
        model: peersProperty.value
        textRole: "name"
        valueRole: "id"
        visible: peersProperty.value.length > 0
        enabled: !root.session
    }
    FormCard.FormButtonDelegate {
        text: i18n("Connect through Wi-Fi Direct")
        icon.name: "network-connect"
        visible: peersProperty.value.length > 0
        enabled: availableProperty.value && peerSelector.currentIndex >= 0 && !root.session
                 && peersProperty.value[peerSelector.currentIndex].available
        onClicked: DaemonDbusInterface.connectWifiDirectPeer(peersProperty.value[peerSelector.currentIndex].id)
    }
    FormCard.FormButtonDelegate {
        text: i18n("Open KDE Connect device")
        icon.name: "smartphone"
        visible: deviceProperty.value.length > 0
        onClicked: applicationWindow().openDevice(deviceProperty.value, "")
    }
    FormCard.FormButtonDelegate {
        text: ["connecting", "getting-ip", "verifying"].indexOf(stateProperty.value) >= 0 ? i18n("Cancel connection") : i18n("Disconnect Wi-Fi Direct")
        icon.name: "network-disconnect"
        visible: root.session
        onClicked: DaemonDbusInterface.disconnectWifiDirectSession()
    }
}
