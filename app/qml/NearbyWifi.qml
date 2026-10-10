// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kdeconnect

FormCard.FormCard {
    id: root

    Connections {
        target: DaemonDbusInterface
        function onWifiDirectChanged() {
            statusProperty.get();
            networksProperty.get();
            searchingProperty.get();
            connectingProperty.get();
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
        id: networksProperty
        object: DaemonDbusInterface
        read: "wifiDirectNetworks"
        change: "wifiDirectChanged"
        defaultValue: []
    }
    DBusProperty {
        id: searchingProperty
        object: DaemonDbusInterface
        read: "wifiDirectSearching"
        change: "wifiDirectChanged"
        defaultValue: false
    }
    DBusProperty {
        id: connectingProperty
        object: DaemonDbusInterface
        read: "wifiDirectConnecting"
        change: "wifiDirectChanged"
        defaultValue: false
    }

    FormCard.FormTextDelegate {
        text: i18n("Start the direct Wi-Fi network in Nearby connection on your phone. Search here, select its network and enter the password shown on the phone. The password is saved for later connections.")
        textItem.wrapMode: Text.WordWrap
        textItem.elide: Text.ElideNone
    }
    FormCard.FormTextDelegate {
        text: statusProperty.value
        textItem.wrapMode: Text.WordWrap
        textItem.elide: Text.ElideNone
    }
    FormCard.FormButtonDelegate {
        text: searchingProperty.value ? i18n("Stop Wi-Fi search") : i18n("Find nearby Wi-Fi networks (60 seconds)")
        icon.name: searchingProperty.value ? "process-stop" : "network-wireless"
        enabled: !connectingProperty.value
        onClicked: {
            if (searchingProperty.value) {
                DaemonDbusInterface.stopWifiDirectDiscovery();
            } else {
                DaemonDbusInterface.startWifiDirectDiscovery();
            }
        }
    }
    FormCard.FormComboBoxDelegate {
        id: networkSelector
        text: i18n("Phone network")
        model: networksProperty.value
        visible: networksProperty.value.length > 0
        enabled: !connectingProperty.value
    }
    FormCard.FormTextFieldDelegate {
        id: passwordField
        label: i18n("Wi-Fi password")
        placeholderText: i18n("Leave empty to use the saved password")
        echoMode: Controls.TextField.Password
        visible: networksProperty.value.length > 0
        enabled: !connectingProperty.value
    }
    FormCard.FormTextDelegate {
        visible: networksProperty.value.length > 0
        text: i18n("Connecting uses your computer's Wi-Fi adapter and disconnects its current Wi-Fi network. Ethernet stays available.")
        textItem.wrapMode: Text.WordWrap
        textItem.elide: Text.ElideNone
    }
    FormCard.FormButtonDelegate {
        text: i18n("Connect directly over Wi-Fi")
        icon.name: "network-connect"
        visible: networksProperty.value.length > 0
        enabled: networkSelector.currentIndex >= 0 && !connectingProperty.value
        onClicked: {
            DaemonDbusInterface.connectWifiDirectNetwork(networkSelector.currentText, passwordField.text);
            passwordField.text = "";
        }
    }
    FormCard.FormButtonDelegate {
        text: connectingProperty.value ? i18n("Cancel connection") : i18n("Disconnect direct Wi-Fi")
        icon.name: "network-disconnect"
        onClicked: DaemonDbusInterface.disconnectWifiDirectNetwork()
    }
}
