# ChainIt System Architecture & Protocol Specification

**ChainIt** connects an **Android phone** and a **Windows laptop** as a unified pair ("**Siblings**") with zero-touch background auto-connection, smart offline/online multi-transport switching, instant bidirectional file transfers, a **dedicated dual-pane ChainIt File Manager**, and a **real Windows `Z:\` virtual drive** so phone directories behave like a local hard disk—even while the phone screen is off in your pocket.

---

## 1. High-Level Repository Architecture

ChainIt uses a **Shared Rust Core + Native Platform Adapters + Live Runnable Desktop/WebDAV Prototype** architecture:

```text
chainit/
├── ARCHITECTURE.md                 # Protocol, transport scoring, Sibling & VFS specification
├── Cargo.toml                      # Rust workspace (shared core + Windows native daemon)
├── crates/
│   ├── chainit-core/               # Cross-platform Rust core (CHNP/1.0 protocol, transport scoring,
│   │                               # Sibling crypto & rotating BLE beacons, resumable chunk engine)
│   └── chainit-windows/            # Windows background service, mDNS/BLE watcher & Z:\ drive mounter
│                                   # (WinFsp native kernel driver + Windows WebDAV WNet fallback)
├── android/                        # Native Kotlin Android client & screen-off Sibling Foreground Service
│   └── app/src/main/
│       ├── AndroidManifest.xml     # BLE hardware scan, LocalOnlyHotspot, MANAGE_EXTERNAL_STORAGE
│       └── java/com/chainit/android/
│           ├── service/SiblingDaemonService.kt      # Screen-off VFS server + BLE -> Hotspot bootstrap
│           ├── vfs/AndroidStorageVfsProvider.kt     # Direct POSIX random-access /storage/emulated/0 VFS
│           ├── receiver/BootAndBleWakeReceiver.kt   # Doze-safe BLE ScanFilter PendingIntent wakeup
│           └── ui/MainActivity.kt                   # Jetpack Compose pairing, Sibling & transport UI
├── scripts/
│   ├── mount-z-drive.ps1           # Mounts ChainIt WebDAV VFS directly as Z:\ in Windows Explorer
│   └── unmount-z-drive.ps1         # Cleanly unmounts Z:\
└── app/                            # Live Runnable Transfer Engine, RFC 4918 WebDAV Server (/dav),
                                    # Dedicated ChainIt File Manager UI (/) & Mobile Companion (/mobile)
```

---

## 2. Multi-Transport Connectivity Engine (Automatic vs. Manual)

ChainIt treats network transports as interchangeable byte-stream pipes beneath a unified session layer (**CHNP/1.0**). Users can switch between **Automatic (Smart Link)** and **Manual (Pinned Transport)** at any time.

### 2.1 Supported Offline Transports (Phase 1 — Implemented) & Pluggable Online (Phase 2)

| Priority | Transport ID | Mode | Typical Speed | Latency | Router Needed? | Primary Role |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | `usb_rndis` | Offline (Wired) | **320–480 MB/s** | `< 0.8 ms` | No | USB Tethering / ADB forward; fastest bulk transfers + charges phone |
| **2** | `wifi_lan` | Offline (LAN) | **45–115 MB/s** | `2–6 ms` | Local Wi-Fi AP | mDNS (`_chainit._tcp.local.`) over shared 5 GHz / 6 GHz Wi-Fi |
| **3** | `local_hotspot` | Offline (Direct) | **35–85 MB/s** | `3–8 ms` | **No (100% Offline)** | Android `LocalOnlyHotspot` / Wi-Fi Direct bootstrapped over BLE |
| **4** | `bluetooth` | Offline (WPAN) | **0.3–2.4 MB/s** | `40–180 ms` | **No (100% Offline)** | **Native RFCOMM sockets** (Windows `AF_BTH` / `ws2bth.h` ↔ Android `BluetoothServerSocket`) carrying the full VFS + WebDAV protocol, plus BLE wakeup beacon |
| **5** | `online_p2p` | Online (Pluggable) | **5–50 MB/s** | `20–90 ms` | Internet | Pluggable `TransportAdapter` trait (QUIC/WebRTC + DERP relay) |

### 2.2 Smart Auto-Selection Scoring Formula

In **Automatic Mode**, `SmartTransportSelector` continuously evaluates every reachable transport $t$ and computes a normalized composite score $S(t) \in [0, 100]$:

$$S(t) = 0.45 \cdot B(t) + 0.25 \cdot L(t) + 0.20 \cdot R(t) - 0.10 \cdot P(t) + H_{\text{active}}(t)$$

Where:
- $B(t) = 100 \cdot \min\left(1, \frac{\log_{10}(\text{BW}_{\text{MBps}} + 1)}{\log_{10}(500)}\right)$ — Logarithmic throughput score
- $L(t) = 100 \cdot \max\left(0, 1 - \frac{\text{RTT}_{\text{ms}}}{150}\right)$ — Low-latency responsiveness score (critical for `Z:\` directory browsing)
- $R(t) = 100 \cdot (1 - \text{PacketLossRatio}) \cdot \text{SignalFactor}$ — Link stability score
- $P(t) \in [0, 100]$ — Battery & metered-data penalty (`0` on USB, `15` on Wi-Fi LAN, `28` on Local Hotspot, `10` on BLE/RFCOMM, `55` on metered cellular Online P2P)
- $H_{\text{active}}(t) = +6.0$ — **Hysteresis bonus** applied to the currently active transport so minor Wi-Fi RSSI fluctuations never cause link flapping.

### 2.3 Dynamic BLE $\rightarrow$ 5 GHz Local Hotspot Promotion (Zero-Router Offline Speed)

When you are away from a Wi-Fi router (on a plane, campus, café without Wi-Fi, or power outage):
1. Laptop and Phone maintain a near-zero-power **native Bluetooth RFCOMM Sibling Link** (`bluetooth`). Small directory listings (`Z:\` `readdir`) work immediately over that socket even with Wi-Fi fully off (see §2.4).
2. As soon as a file read/write $\ge 1\text{ MB}$ starts (or `Z:\` streams media), `SmartTransportSelector` sends an encrypted `PromoteLocalHotspot` command over BLE GATT.
3. Android calls `WifiManager.startLocalOnlyHotspot()` in the background (screen stays off!) and returns the ephemeral 5 GHz `SSID` + `WPA2/WPA3 Passphrase` + local IP over the encrypted BLE channel.
4. Windows connects its Wi-Fi Direct / WLAN virtual adapter to the ephemeral hotspot and **seamlessly migrates the in-flight transfer at the next 1 MB chunk boundary** without restarting or corrupting the copy!

### 2.4 Native Bluetooth RFCOMM Transport (Implemented — Zero Wi-Fi / Zero USB)

Bluetooth is a **first-class transport**, not an OBEX file-push workaround: both ends open real RFCOMM byte streams and speak the identical `CHNP/1.0` HTTP + WebDAV protocol used on TCP, so `Z:\`, the File Manager, previews and transfers behave exactly the same with every radio except Bluetooth switched off.

| Role | UUID | Implementation |
| :--- | :--- | :--- |
| **Phone Server** (serves `Z:\` + File Manager to the PC) | `c4a1b170-0001-4000-8000-00805f9b34fb` | Android `BluetoothAdapter.listenUsingRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)` → `PhoneHttpAndWebDavServer.handleStreamSession(in, out, viaBluetooth = true)` |
| **PC Receiver** (accepts Phone → PC pushes) | `c4a1b170-0002-4000-8000-00805f9b34fb` | Windows `socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)` → `bind(BT_PORT_ANY)` → `listen` → `WSASetServiceW(NS_BTH, RNRSERVICE_REGISTER)` publishes the SDP record → accepted sockets are handed to the regular HTTP engine, so `POST /api/vfs/pc/upload?dir=/Desktop` writes to `C:\Users\<user>\Desktop` |

**Windows client side (`windows/src/chainit_win_main.cpp`)**
- A Bluetooth endpoint is addressed as `bt://AA:BB:CC:DD:EE:FF` (optional `:channel` suffix). `normalizeBtEndpoint()` accepts bare MACs (`AA-BB-CC-DD-EE-FF`, `AABBCCDDEEFF`) from the Manual-Connect box and never mistakes an IPv4 address for a MAC.
- `httpRequestToPhone()` branches on that prefix: `SOCKADDR_BTH{AF_BTH, btAddr, serviceClassId = kChainItPhoneUuid, port = BT_PORT_ANY}` lets the Windows stack run the SDP query and resolve the RFCOMM channel, then a non-blocking `connect` + `select` enforces the timeout (with a blocking retry for stacks that reject non-blocking `AF_BTH` connects).
- Because one RFCOMM link is a single serialized stream, all Bluetooth requests are queued on `gBtRfcommClientMtx`; TCP transports stay fully parallel.

**Discovery & failover**
- `bluetoothDiscoveryLoop()` calls `WSALookupServiceBeginW(NS_BTH, LUP_CONTAINERS | LUP_RETURN_NAME | LUP_RETURN_ADDR)` to read the stack's *remembered/paired* device cache — no 12-second inquiry scan, no user prompt — then probes each device's `bt://` endpoint with `GET /chainit/ping`.
- The Bluetooth endpoint TTL is **35 s** (vs 12 s for TCP) since page/SDP latency is higher, so the link never flickers out of the scored transport list; Automatic Mode fails over to it the instant USB/Wi-Fi/Hotspot disappear, and Manual Mode can pin `bluetooth` outright.
- Bluetooth needs **no firewall rule** and survives router AP-isolation, captive portals and corporate LAN lockdowns.

---

## 3. Sibling Mode Architecture (Zero-Touch Connect & Screen-Off Access)

Standard file-sharing apps require unlocking both devices, opening two apps, tapping "Send", and tapping "Accept". **Sibling Mode** eliminates both human steps:

### 3.1 Cryptographic Sibling Bond
1. **One-Time Pairing**: Laptop displays a QR code containing its **Ed25519 Static Public Key**, device name, and ephemeral pairing nonce. Phone scans QR (or verifies a 6-digit Short Authentication String) and returns its Ed25519 public key.
2. **Promoting to Sibling**: Enabling **Sibling Mode** signs a mutual `SiblingGrantCertificate` stored in Windows Credential Locker / DPAPI and Android Keystore:
   - Grants **unattended VFS mount permission** (`Z:\` access to `/storage/emulated/0`)
   - Grants **zero-prompt incoming transfer acceptance**
   - Enables **auto-reconnect standby** whenever any transport detects the peer

### 3.2 Privacy-Preserving Rotating BLE Wakeup Beacons
To detect a Sibling device automatically without leaking a static trackable MAC/UUID to strangers:
- Both devices derive a shared 256-bit `SiblingBeaconSecret` via HKDF from their X25519 shared secret.
- Every 15-minute epoch $E = \lfloor \text{unix\_time} / 900 \rfloor$, the expected 8-byte BLE advertisement service data tag is:
  $$\text{Tag}_E = \text{Trunc}_{64}\Big(\text{HMAC-SHA256}(\text{SiblingBeaconSecret}, E \parallel \text{DeviceRole})\Big)$$
- Only your paired Sibling knows `SiblingBeaconSecret` and recognizes $\text{Tag}_E$ (accepting $E \pm 1$ for clock skew).

### 3.3 Keeping Android Reachable With Screen OFF (Doze Mode Solution)
Android aggressively suspends background network sockets when the screen turns off. ChainIt solves this cleanly on Android 11–15 without draining battery:
1. **Hardware-Offloaded BLE `ScanFilter` + `PendingIntent`**: Android's Bluetooth controller chip filters for ChainIt's Service UUID in hardware (`< 0.5%` battery/day) while the main CPU sleeps. When your Windows laptop wakes up and broadcasts its Sibling BLE frame, Android wakes `BootAndBleWakeReceiver` even from deep Doze!
2. **`SiblingDaemonService` (`connectedDevice | dataSync` Foreground Service)**: Acquires a lightweight `PARTIAL_WAKE_LOCK` and `WIFI_MODE_FULL_LOW_LATENCY` lock **only while the Windows Sibling session is actively mounted**, keeping the screen completely black and locked while serving `Z:\` at full 5 GHz speed.
3. **One-Time `MANAGE_EXTERNAL_STORAGE` Grant**: Instead of Android's Storage Access Framework (`DocumentFile`, which requires UI pickers and takes ~400 ms per directory listing), granting *All Files Access* once during Sibling setup lets `AndroidStorageVfsProvider` perform native POSIX `FileChannel` random-access `pread`/`pwrite` across `/storage/emulated/0` (`DCIM`, `Download`, `Documents`, `Music`, etc.) in `< 2 ms`.

---

## 4. Dual Windows Experience: Real `Z:\` Drive + ChainIt File Manager

In Sibling Mode on Windows, phone files are accessible **both ways simultaneously**:

1. **Real Windows `Z:\` Drive in `explorer.exe`**:
   - **Tier 1 — Native WinFsp Volume (`crates/chainit-windows`)**: Mounts `Z:\` (`Galaxy S24 [ChainIt Sibling]`) via `WinFsp` (`FspFileSystem`), translating `ReadDirectory`, `Read` (byte-range streaming), `Write`, `Rename`, and `Cleanup/Delete` directly into CHNP/1.0 VFS RPCs with an LRU metadata cache.
   - **Tier 2 — Zero-Driver Local WebDAV Loopback (`/dav` + `scripts/mount-z-drive.ps1`)**: Runs an RFC 4918 compliant WebDAV server on `127.0.0.1` and maps it to `Z:\` via Windows' built-in `WebClient` redirector (`net use Z: http://127.0.0.1:4173/dav`). Supports `PROPFIND`, HTTP `Range` streaming, `PUT`, `MKCOL`, `MOVE`, `COPY`, and `DELETE` out of the box with zero kernel drivers.
2. **Dedicated ChainIt File Manager App**:
   - Dual-pane (`C:\` PC $\leftrightarrow$ `Z:\` Phone) and Single-Pane Explorer views.
   - Native keyboard shortcuts (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `F2`, `Del`, `Ctrl+A`), drag-and-drop across drives or directly from Windows Desktop, inline file preview/editing, and live chunked transfer speed telemetry.

## 5. Building the Shipped Binaries From Source

Both deliverables are produced by deterministic scripts (no IDE, no Gradle, no Visual Studio):

1. **Android** — `./android/build-apk.sh`
   `aapt2` (resources) → generated `R.kt` → `kotlinc -Xlambdas=class -Xsam-conversions=class` (Kotlin 2.x
   must not emit `invokedynamic`: `dx` cannot lower it for API 26) → `android/tools/verify_dex_safety.py`
   (stage 1: 68 first-party classes indy-free; stage 2: runtime jar indy users are `dx`-lowerable;
   stage 3: no source calls an indy-emulated stdlib method; stage 4: no runtime color parsing) →
   `dx --dex` → zipalign-style packer → `apksigner` (v2+v3) with `android/keystore/chainit-release.p12`.
2. **Windows** — `./windows/build.sh`
   `npm run build` (vite) → `scripts/gen_embedded_assets.py` (embeds `app/dist` as a C header) →
   `zig c++ -target x86_64-windows-gnu` over `windows/src/chainit_win_main.cpp` **and**
   `windows/res/chainit.rc`, which embeds `windows/res/chainit.ico` (generated from the Android launcher
   vector by `windows/res/make_icon.py`) as icon resource #1 — used by the exe file icon, the window class,
   the tray icon and desktop shortcuts.
3. **Release manifest** — `python3 scripts/publish_release_manifest.py` rewrites
   `releases/version.txt` (version + SHA-256 of both binaries). The Windows in-place updater and the
   Android `UpdateManager` both trust this file to decide "up to date" vs "replace in place".
