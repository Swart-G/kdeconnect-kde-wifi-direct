# KDE Connect: Wi-Fi Direct with ordinary Wi-Fi preserved

This private Android/Linux build discovers Wi-Fi Direct peers and establishes
one manual P2P session. Device selection and system confirmation replace the
old network-name/password workflow. KDE Connect certificates, TLS, pairing and
plugins are reused. Bluetooth and legacy Wi-Fi client association are not used.

## First connection

1. Keep both devices connected to the router; enable Wi-Fi.
2. On Android open **Nearby connection** and **Make available for 60 seconds**.
   Allow nearby Wi-Fi permission. Android 10-12 also requires precise location
   permission and the location switch. The feature requires Android 10+.
3. On Linux select **Find nearby devices (60 seconds)** on the welcome page or
   in Settings. Unsupported adapters show the reason and are left connected.
4. Select the phone's unverified system P2P name and connect. Accept the system
   Wi-Fi Direct request on Android.
5. Wait for group negotiation, IP configuration and KDE Connect TLS verification.
   Open the verified KDE Connect device, compare pairing codes and confirm trust.
6. **Connected through Wi-Fi Direct** means the KDE Connect session is paired
   and ready. Normal plugins and file sharing use the direct interface.

Search is manual and ends after 60 seconds even when the Android screen closes.
Stopping search preserves the list; the peer is checked again before connecting.
Negotiation and IP configuration have a combined 90-second deadline; TLS discovery
has a further 30 seconds. Closing the screen keeps an established session alive.
There is no automatic search or reconnection after failure or process restart.
Repeat search and selection for the next connection; KDE Connect trust is retained.

On Linux nl80211 must advertise client and group-owner P2P roles and valid
combinations with existing interfaces. With an active station, two-channel
concurrency is required. One-channel combinations are intentionally refused,
since the NetworkManager P2P API cannot fix the negotiated group channel. The
current RTL8192EU/rtl8xxxu adapter is unsupported; replacing its driver is out
of scope. A second adapter is selected only when its own combinations are safe,
with connected compatible adapters preferred and stable interface-name ordering.

NetworkManager owns negotiation and IP setup. A `wifi-p2p` profile is volatile,
never autoconnects and supplies no default route or DNS. Applications bind
outgoing discovery, TCP and file transfers to actual P2P addresses. Subnet
conflicts and loss of the original Wi-Fi connection abort the session.

Foreign profiles and groups are not deleted. Android can prove removal ownership
for a group formed by its explicit connect operation to the selected peer. For
incoming groups, the application has no public API proving which application
created the group: ending the KDE Connect session leaves removal to system
Wi-Fi Direct settings and explains this. A pre-existing group is always busy.

Upgrading clears the old Android automatic-group flag and obsolete credentials
once, without touching KDE Connect certificates or paired devices. Old
`KDE Connect Direct ...` NetworkManager profiles are not reused or activated.
Upgrade desktop UI and daemon together because their internal D-Bus API changed.

## Build and verification

Android development build and upstream unit suite:

```sh
export ANDROID_HOME="$HOME/Android/Sdk"
cd kdeconnect-android-wifi-direct
./gradlew testDebugUnitTest assembleDebug --console=plain
```

Fedora 44 needs the usual KDE/Qt build dependencies plus `libnl3-devel`
(`libnl-3.0`, `libnl-genl-3.0`). Build with `BUILD_TESTING=ON` and
`BLUETOOTH_ENABLED=OFF`; run CTest inside a private D-Bus session with
`QT_QPA_PLATFORM=offscreen` and the build's `bin` in `QT_PLUGIN_PATH`.
The new `wifidirectdiagnostic` target performs a safe unsupported-hardware check:

```sh
python3 tools/integration/check_unsupported.py \
  --probe .local/p2p-v2/bin/wifidirectdiagnostic --sta-interface wlp2s0f0u2
```

On compatible hardware disconnect Ethernet manually, connect both devices to
Wi-Fi, then use the debug builds and run:

```sh
python3 tools/integration/test_wireless.py --device VERIFIED_KDE_CONNECT_ID \
  --sta-interface wlan0 --p2p-interface p2p-wlan0-0 --cycles 5
```

The script prompts for each manual search, selection and disconnection. It checks
STA, DNS and default routes, actual established TCP source addresses, and 4 MiB
file hashes in both directions. ADB is instrumentation only. Also record both
P2P roles, refusal, cancellation, disappearance, LAN fallback and ten minutes
with the phone screen off and USB power disconnected. Adjust the interface names
and receive folder to the actual devices.

**Release gate:** first pass this hardware scenario on debug builds. Only then
increase the APK/RPM release numbers and run `packaging/android/build.py` and
`packaging/fedora/build.py` to create signed packages. Retain the private signing
key under `.local/android-signing`; never include it in source archives. Repeat
hardware verification on the release packages and update SHA-256 sums and report.
Existing files under `dist` and earlier diagnostic reports belong to the legacy
implementation and do not validate this P2P version. Current work and results
are in `diagnostics/p2p-v2/REPORT.md`.

Current development artifacts are under `dist/p2p-v2`, with separate SHA-256
sums. The Android debug package uses `org.kde.kdeconnect_tp.debug` and its own
pairing data. The desktop bundle targets Fedora 44 and requires its KDE/Qt
runtime dependencies. `tools/integration/run_debug_desktop.sh` launches both
matching components on a private D-Bus bus and stores persistent debug pairing
data under `.local/p2p-v2`. With `--shell`, run `kdeconnect-app` and the manual
hardware script from that shell. Its receive folders can be set with
`--desktop-receive-dir` and `--android-receive-dir`. To smoke-test the UI and
daemon without discovery, run `python3 tools/integration/smoke_desktop.py`.

No commits, pushes or upstream submissions are part of this private local task.

Primary API references: [NetworkManager P2P discovery](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.Device.WifiP2P.html),
[P2P settings](https://networkmanager.dev/docs/api/latest/settings-wifi-p2p.html),
[Android Wi-Fi Direct](https://developer.android.com/develop/connectivity/wifi/wifi-direct).
