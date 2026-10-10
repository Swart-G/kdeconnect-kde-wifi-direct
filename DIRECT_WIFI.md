# Direct Wi-Fi connection

This private build connects a computer directly to an Android phone without
Bluetooth, a router, USB networking, or a separate phone application.

## First connection

1. Enable Wi-Fi on the phone and computer.
2. On Android, open KDE Connect -> Nearby connection -> Start direct Wi-Fi
   network. Allow the nearby Wi-Fi permission. Android 10-12 also requires
   location permission and the system location switch.
3. Keep the network name and password on the phone screen.
4. In KDE Connect on the computer, open Settings or the welcome page and select
   Find nearby Wi-Fi networks (60 seconds).
5. Select the phone's DIRECT-KC-KDEConnect network and enter its password.
6. Connect, select the phone in the device list, and confirm KDE Connect trust.
   Compare the verification codes. Existing KDE Connect pairing is preserved.

The password is entered once. Android keeps the same private network settings;
NetworkManager saves the computer's Wi-Fi profile and password. On later
connections, start the phone network, search on the computer, select it and
leave the password field empty. Enter a new password if the phone's settings
were reset. Changing the password updates the existing profile.

## Search and disconnection

The application searches only after a button press, for up to 60 seconds.
Stop Wi-Fi search stops it immediately. The resulting list remains available
when the search finishes. Search does not connect to any phone automatically.
Selecting Connect stops the search and starts a bounded connection attempt;
Cancel connection or Disconnect direct Wi-Fi stops that attempt or connection.
A lost connection requires another explicit search and connection.

Start/stop of the phone network is separate from computer search. The phone
keeps its network available while Nearby connection is enabled, restores it
when its service restarts, and removes its own group when disabled. Another
application's Wi-Fi Direct group is never adopted or removed.

The computer uses its adapter as a regular WPA2 client of the phone-owned
Wi-Fi Direct group. It does not require P2P support on its adapter. The network
uses 2.4 GHz. Connecting replaces the adapter's current Wi-Fi connection;
Ethernet stays available. The saved profile has autoconnect disabled, no default
route and no DNS, so it does not take over the computer's Internet connection.
Bluetooth transports are not registered in either application, including after
an upgrade from the previous Bluetooth-assisted build.

Android requires version 10 or newer and Wi-Fi Direct support. The desktop
requires NetworkManager on Linux. Existing KDE Connect LAN discovery, TLS
pairing and plugins carry all application traffic over the direct Wi-Fi link.
