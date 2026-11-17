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
├── windows/                        # Shipped Win32 single-binary engine (no Visual Studio needed)
│   ├── src/chainit_win_main.cpp    # Tray + native hub window, UDP discovery, AF_BTH RFCOMM, WebDAV Z:\,
│   │                               # parallel chunked transfers, self-updater, /uninstall
│   ├── src/chainit_installer.cpp   # Self-contained setup stub (reads its own appended payload)
│   ├── res/make_icon.py            # Renders the Android launcher vector -> chainit.ico / PNG icons
│   └── build.sh                    # zig c++ cross-compile + manifest republish
├── installer/                      # Windows setup packaging (no Inno Setup / NSIS / WiX)
│   ├── build-installer.sh          # Compiles the stub, then packs stub + payload + trailer
│   └── pack_installer.py           # 52-byte trailer: magic | payload size | SHA-256 | trailer size
├── releases/                       # Committed deliverables + version.txt (in-place update manifest)
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
   - **Drive labelling**: Explorer derives a network drive's display name from
     `HKCU" + BS + "Software" + BS + "Microsoft" + BS + "Windows" + BS + "CurrentVersion" + BS + "Explorer" + BS + "MountPoints2" + BS + "<key>" + BS + "_LabelFromReg`,
     so `setZDriveLabelInExplorer()` writes `<phone name> (ChainIt)` there after `net use` and re-applies it on a
     short back-off (Explorer only creates the key on first browse). The `127.0.0.1:4173` loopback URL is never
     shown as the drive name.
3. **Dedicated ChainIt File Manager App** — since v1.5.0 this is a **native Win32 window drawn by
   `ChainIt-Windows.exe` itself** (see §8); the React bundle below is still served at `/` for browsers,
   remote use and the phone's Mobile Companion page, but the desktop app never opens it.
   - Dual-pane (`C:\` PC $\leftrightarrow$ `Z:\` Phone) and Single-Pane Explorer views, list *and* grid.
   - Explorer-grade columns (**Name / Size / Type / Modified**), click-to-sort headers, folders-first toggle,
     live filtering, and a status bar with item count, selection size and the active sort chip. The sort model
     (`app/client/utils/sort.ts`, persisted as `chainit.sort.v1`) is shared by both panes.
   - Native keyboard shortcuts (`Enter` open, `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `F2`, `Del`, `Ctrl+A`, `F5` refresh,
     `Backspace`/`Alt+ArrowUp` up, `Ctrl+Shift+N` new folder, `Ctrl+E` filter, `Esc` clear), drag-and-drop across
     drives or directly from Windows Desktop, inline file preview/editing, a right-click menu with
     *Download to this PC*, and live chunked transfer speed telemetry.
4. **Native ChainIt hub window (taskbar identity + mini form)**:
   - ChainIt creates a real top-level `ChainItHubWindowClass` window whose class icon is resource #1 of the exe,
     and pins one taskbar identity with
     `SetCurrentProcessExplicitAppUserModelID(L"ChainIt.Sibling.Hub")` - so the taskbar, Alt-Tab and the tray all
     show the chain-link mark rather than a generic globe. Since v1.5.0 the File Manager window shares that same
     identity, so every ChainIt window groups under one button (§8.1).
   - It is created at startup but **stays hidden**: since v1.5.0 the native File Manager is the window that owns
     the taskbar button, so showing both would just stack two ChainIt windows. The hub remains one tray
     double-click away, and the Link tab has a *Show mini hub panel* button.
   - The same window is the app's **mini form**: link state, transport, `Z:\` status and integrity counters with
     keycap buttons for *Open File Manager*, *Open `Z:\`*, *Mount / Refresh `Z:\`*, *Create Desktop Icon*,
     *Check for Updates* and *Quit*. It is the first tray menu entry and the tray double-click action; closing it
     keeps ChainIt resident. The tray's *Mini status panel* entry opens this same native window — the Edge
     app-mode `/mini` launcher was deleted in v1.5.0.

## 5. Building the Shipped Binaries From Source

Both deliverables are produced by deterministic scripts (no IDE, no Gradle, no Visual Studio):

1. **Android** — `./android/build-apk.sh`
   `aapt2` (resources) → generated `R.kt` → `kotlinc -Xlambdas=class -Xsam-conversions=class` (Kotlin 2.x
   must not emit `invokedynamic`: `dx` cannot lower it for API 26) → `android/tools/verify_dex_safety.py`
   (stage 1: 208 first-party classes indy-free; stage 2: runtime jar indy users are `dx`-lowerable;
   stage 3: no source calls an indy-emulated stdlib method; stage 4: no runtime color parsing) →
   `dx --dex` → zipalign-style packer → `apksigner` (v2+v3) with `android/keystore/chainit-release.p12`.
   The script also **fails the build if any Android source opens `http://…:4173` with `ACTION_VIEW`** and asserts
   that `PcBrowserActivity`, `PcVfsClient` and `ChainItFileProvider` are present in the final `classes.dex`, so the
   phone cannot quietly regress into borrowing a browser again (§9.6).
2. **Windows** — `./windows/build.sh`
   `npm run build` (vite) → `scripts/gen_embedded_assets.py` (embeds `app/dist` as a C header) →
   `zig c++ -target x86_64-windows-gnu` over `windows/src/chainit_win_main.cpp` **and**
   `windows/res/chainit.rc`, which embeds `windows/res/chainit.ico` (generated from the Android launcher
   vector by `windows/res/make_icon.py`) as icon resource #1 — used by the exe file icon, the window class,
   the tray icon and desktop shortcuts.
3. **Windows installer** — `./installer/build-installer.sh`
   compiles `windows/src/chainit_installer.cpp` with `zig c++` (icon + `VERSIONINFO` from
   `windows/res/chainit_installer.rc`), then `installer/pack_installer.py` appends the finished
   `ChainIt-Windows.exe` and a **52-byte trailer**: `"CHAINIT1" | payload size (u64) | SHA-256 (32 B) |
   trailer size (u32)`. At run time the stub reads the trailer back out of its own image, recomputes the
   digest, and only then writes the payload - so the distributed setup file is self-verifying with no external
   packaging tool. Install is per-user (`%LOCALAPPDATA%\Programs\ChainIt`, no UAC prompt) and in-place:
   re-running setup overwrites the same file, asking the running instance to exit first if it holds a lock.
   Flags: `/S` silent, `/UNINSTALL` remove.
4. **Release manifest** — `python3 scripts/publish_release_manifest.py --version <v> --code <n>` rewrites
   `releases/version.txt` (version + SHA-256 of all three artifacts). The Windows in-place updater and the
   Android `UpdateManager` both trust this file to decide "up to date" vs "replace in place".

   Before writing anything it **reads the compiled `VERSIONINFO` back out of the binaries** and refuses to
   publish unless every `VS_FIXEDFILEINFO` in the exe and in the installer (the packed setup carries two: its
   own stub resource plus the appended payload's) matches `--version`, and unless the UTF-16 `FileVersion`
   string is present too. `windows/res/*.rc` state the version twice — numeric `FILEVERSION`/`PRODUCTVERSION`
   and the `FileVersion`/`ProductVersion` strings — and a hand bump can update one without the other. Explorer's
   *Details* tab shows the string, so that mistake is invisible in the UI while `GetFileVersionInfo` and anything
   comparing versions numerically still report the previous release; v1.7.0 shipped that way once, and the check
   turns it into a build failure (`exit 1`, manifest left untouched) instead.

5. **Release audit** — `python3 scripts/audit_release.py` re-derives every claim the release
   table makes from the **published bytes**, with no SDK, no jars and no network. It exists because the
   checks that used to cover this all depended on tools provisioned into `/tmp` (`apksigner.jar`, the
   compiled `.class` tree for `verify_dex_safety.py`, a Windows SDK for the version resource), and `/tmp` is
   wiped regularly — which turned "verified" into "silently not verified" rather than into a failure.

   It parses the artifacts instead:

   - `releases/version.txt` agrees with the bytes on disk (size + SHA-256 of all three).
   - the exe and the installer carry the right **numeric** `VERSIONINFO`, the installer's 52-byte trailer
     digest matches its appended payload, and that payload is byte-identical to the published exe (§5.4).
   - the Windows binary contains no browser-launch string (`msedge`, `--app=`, `iexplore`, `chrome.exe`) —
     the native-UI guarantee of §8 checked against the shipped file rather than the source.
   - the APK's v2 **and** v3 signing blocks are *verified*, not just spotted: the signer certificate is the
     pinned release key (what makes an upgrade install in place rather than fail with
     `INSTALL_FAILED_UPDATE_INCOMPATIBLE`), the chunked content digest recomputed from the file — three
     sections, 1 MiB chunks, `0xA5`/`0x5A` prefixes, with the EOCD's central-directory offset hashed as
     though it pointed at the signing block — still matches what the signer committed to, and the
     RSASSA-PKCS1-v1_5 signature over `signedData` verifies against the public key embedded in the block.
     Both "signed by us" and "unchanged since signing" are therefore proved in pure Python.
   - the **binary** `AndroidManifest.xml` really says the published `versionName`/`versionCode` (read out of
     the AXML chunks, not out of the source file).
   - no **first-party** class contains `invoke-custom`/`invoke-polymorphic`, scanned per code item at real
     instruction boundaries. Sites inside prebuilt kotlin-stdlib classes are counted and reported but not
     failed, which is precisely the distinction `verify_dex_safety.py` draws at build time (§5.1).

   An operand word is not an opcode: testing every code unit's low byte reports ~1,100 phantom
   `invoke-custom`s in this very APK, and getting the `move-wide` family's widths wrong (12x/22x/32x, not
   2/3/5) still leaves ~80 — so the walk uses the real instruction-width table and steps over inline
   switch/`fill-array-data` payloads. The checks were then proved to fail when they should: `--releases-dir`
   points the audit at a copy, and a single flipped byte in a ZIP entry, in the central directory, inside
   `signedData`, or inside the certificate is each caught by the corresponding check and no other.

---

## 6. Fast, Reliable Transfers: Parallel Chunking + End-to-End Integrity

Files are the product, so the transport is designed around two rules: **go fast** and **never hand either
device a file that is not byte-identical to the original**.

### 6.1 Parallel chunk pipeline (Windows $\leftarrow$ phone)

```text
downloadPhonePathToLocal(phoneIp, /DCIM/IMG_0001.jpg, C:\Users\me\Downloads)
  1. GET /chainit/raw?path=...   Range: bytes=0-0
        -> 206, Content-Range: bytes 0-0/18_874_368
           X-ChainIt-File-Sha256: <digest of the WHOLE file>
  2. N workers (4 over USB/Wi-Fi/hotspot, 1 over Bluetooth RFCOMM)
        loop: off = atomic cursor.fetch_add(2 MiB)
              GET Range: bytes=off-(off+len-1)
              X-ChainIt-Chunk-Sha256 == SHA256(body) ?  -> keep : abort
              WriteFile(hPart, body, OVERLAPPED{Offset=off})   # no seeking, no interleaving
  3. whole-file gate:  SHA256(<dst>.chainit-part) == X-ChainIt-File-Sha256
  4. MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)          # atomic, in-place
  any failure -> DeleteFileW(part) and fall back to one verified single-stream GET
```

* **Chunk size 2 MiB**, `kTransferChunkBytes`; **4 workers**, `kTransferWorkersLan`. Work is claimed through an
  atomic cursor, so workers never overlap and a slow worker simply takes fewer chunks.
* **Bluetooth stays single-stream on purpose.** RFCOMM serializes one link (`gBtRfcommClientMtx`); extra
  workers over the radio would only queue behind each other and add contention.
* **Writes use `OVERLAPPED` offsets**, so the reassembly buffer needs no shared file pointer and no locking.
* **Chunking is transport-only and invisible.** The buffer is created with
  `FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY`, so it never appears in Explorer, on the Desktop or in any
  ChainIt listing; it is renamed over the destination only after the whole-file hash matches, and deleted on
  any failure. A file is therefore either complete and correct, or it does not exist - never half-written.
* Counters `gIntegrityChunksOk` / `gIntegrityFailures` are published in `/api/state` under `security` and shown
  on the Connectivity view's *Sibling security & transfer integrity* card.

### 6.2 Cross-device isolation (a compromised device must not poison the other)

| Control | Where | Behaviour |
| :--- | :--- | :--- |
| **Mandatory Pairing Key** | `PhoneHttpAndWebDavServer.checkSiblingAuth` | Every VFS/WebDAV request must carry the shared key; missing or wrong $\rightarrow$ `401`. Previously an absent key was tolerated. |
| **Brute-force brake** | same | 5 failed keys inside 60 s locks the VFS port for 30 s and answers `429 Too Many Requests`. |
| **Published digests** | `streamFileResponse` | Every served range advertises `X-ChainIt-File-Sha256` (whole file) and `X-ChainIt-Chunk-Sha256` (that range), so the receiver can prove what it got. |
| **Inbound verification** | WebDAV `PUT`, `/chainit/upload` | The body is hashed and compared with the sender's `x-chainit-file-sha256`; on mismatch the partial file is **deleted** and the request answered `409 Conflict`. |
| **Receiver-side gate** | `downloadPhonePathToLocal` | Per-chunk hash, then a whole-file hash, then the atomic rename. A tampered or truncated transfer cannot reach the user's disk. |
| **Sender-side gate** | `uploadLocalPathToPhone` | Sends `X-ChainIt-File-Sha256` and treats `409` as a hard failure rather than a silent success. |

The point is not to *scan* for malware on the PC - it is to make the link incapable of silently altering a
file, and to make every refusal visible instead of failing quietly.

### 6.3 Windows identity & shortcut guarantees

* `windows/res/chainit.rc` embeds `windows/res/chainit.ico` (rendered from the Android launcher vector by
  `make_icon.py`: 16/20/24/32/40/48/64/128/256 px) as icon resource **#1**, plus `VERSIONINFO`
  (*ChainIt Sibling*, 1.3.0.0). Nine `RT_ICON` images and one `RT_GROUP_ICON` are verified in the built PE.
* `createDesktopShortcut()` writes `ChainIt.lnk` via `IShellLinkW`, **verifies the file exists**, and if the
  shell refused, writes the documented MS-SHLLINK binary layout itself (`writeShellLinkFile`: 76-byte header,
  `LINKINFO` with `VolumeID` + ANSI `LocalBasePath`, then `NAME`/`WORKING_DIR`/`ICON_LOCATION` string data).
  It probes the known Desktop folder, `%USERPROFILE%\Desktop` and the OneDrive Desktop, also writes a Start
  Menu entry, flushes the shell notification queue, and returns the exact verified path.
* `ChainIt-Windows.exe /uninstall` (registered as the Apps & features uninstaller by the setup program)
  unmaps `Z:\`, removes both shortcuts, deletes the uninstall registry key and the saved settings in
  `%APPDATA%\ChainIt`, then hands its own file and folder to a detached `cmd.exe` - a running exe cannot
  delete itself.

---

## 7. Bluetooth as a First-Class Drive Transport (v1.4.0)

Before v1.4.0 the RFCOMM link came up and then appeared to do nothing. Three independent causes, all fixed.

### 7.1 Why it looked dead

| Cause | Symptom | Fix |
| :--- | :--- | :--- |
| Windows dialled the radio **per request** behind one global mutex | An RFCOMM connect is an SDP lookup + page (1-5 s); Explorer issues dozens of requests per folder, so the queue never drained | Three **persistent channels** (`kBtChannelCount`), each with its own mutex, reused as HTTP/1.1 keep-alive sessions |
| The phone served **one request per connection** | Even a reused socket could not carry a second request | `handleStreamSession` loops: parses as many requests as the peer pipelines, frames each response by `Content-Length`/chunked (never by EOF), drains unconsumed bodies, flushes per response |
| `/dav` **faked success** when the phone did not answer | Explorer mounted `Z:\` and showed an empty synthetic drive - "connected but does nothing" | Real **503 + the actual reason**, logged, transport re-scan forced, published as `linkProblem` in `/api/state`; before any phone has paired, `Z:\` serves one readable `READ ME FIRST` file |

### 7.2 Persistent RFCOMM channel pool (Windows)

```text
httpRequestToPhone("bt://AA:BB:CC:DD:EE:FF", ...)
  btAcquireChannel()   -> reuse a parked channel, else btRfcommConnect()
                          (first dial caches the SDP-discovered RFCOMM channel
                           number in gBtCachedPort, so later dials skip SDP)
  lock channel.mtx     -> per channel, NOT global: 3 requests run in parallel
  btHttpExchangeOnChannel()
      send request with "Connection: keep-alive"
      read headers byte-at-a-time to CRLFCRLF (never over-reads into the body)
      body from Content-Length, or by de-chunking, or (no framing) to EOF -> drop channel
  btReleaseChannel(ok && keepAliveOk)  -> park for reuse, or close
  on failure: btPoolReset() and re-dial (up to 3 attempts)
```

`btPoolReset()` only closes channels marked `inPool`; a channel mid-request is owned by its caller and closed
by it, so a transport switch can never pull a socket out from under an in-flight transfer.

Telemetry published in `/api/state -> bluetooth`: `pooledChannels`, `maxChannels`, `cachedRfcommChannel`,
`requestsServed`, `redials`, `lastRttMs`, `keepAlive`.

### 7.3 Keep-alive session loop (Android)

`PhoneHttpAndWebDavServer.handleStreamSession` wraps the stream in a `CountingInputStream` so that after each
handler returns it can drain `Content-Length - consumed` bytes. Without that, an upload body a handler did not
read would be parsed as the next request line and desynchronise the session permanently. Responses ask
`connectionHeader()` (a `ThreadLocal` set per request) whether to advertise `keep-alive` or `close`. Sessions
are registered so a sweeper can force-close one idle for 10 minutes - Bluetooth streams have no `SO_TIMEOUT`
of their own, and a blocked `read()` on a vanished peer would otherwise pin a thread forever.

### 7.4 Surviving a phone locked for hours

| Mechanism | Detail |
| :--- | :--- |
| Foreground type | `connectedDevice` (not `dataSync`, which Android 14 stops after ~6 h per 24 h) |
| Doze heartbeat | `setExactAndAllowWhileIdle` every 9 minutes -> `selfCheck()` restarts the VFS server, RFCOMM listener or UDP discovery if any died, and re-takes the Wi-Fi/multicast locks |
| Wake lock | `PARTIAL_WAKE_LOCK` renewed 30 minutes before its 12-hour ceiling, so there is never an unlocked gap |
| Recents swipe | `onTaskRemoved` schedules an immediate restart via `AlarmManager` |
| Listener watchdog | Re-arms RFCOMM if `accept()` throws, if the radio is toggled, or if Doze suspends it; secure channel first, then the insecure channel many desktop stacks require |
| Battery exemption | Surfaced as a setup row with an `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` intent |
| Proof of life | Survived heartbeats are counted and shown in the notification, the Android footer and the Windows About tab |

### 7.5 Refusals are never silent

A correct Pairing Key with Sibling Mode switched off answers **403** with the exact remedy instead of a 401
that is indistinguishable from a dead link. Wrong or missing keys still answer 401 and still trip the
brute-force lockout. Any of these, plus an unreachable phone, is written to `gState.phoneLinkProblem`, shown
in an amber banner in the web UI and as the headline of the native hub window.

### 7.6 Attribution

**Built by Qmer** - the Windows About tab, the native hub window header and footer, the Android app footer,
and the `VERSIONINFO` of both exes (`CompanyName = Qmer`, `LegalCopyright = "Built by Qmer"`).

## 8. The Native Win32 File Manager (v1.5.0)

### 8.1 Why the browser had to go

The desktop UI was opened with `ShellExecuteW(msedge.exe, L"--app=http://127.0.0.1:4173")`. App-mode removes
Edge's own chrome, but the **window still belongs to the browser process**, and Windows derives a taskbar
button's icon from the process that owns the window plus its AppUserModelID. Neither is ChainIt's, so the
button showed Edge's **globe**, and no `RT_GROUP_ICON` in this exe could change it. The only correct fix is to
own the window, so both browser launchers (`launchDesktopUiWindow`, `launchMiniPanelWindow`) were deleted and
replaced by `showNativeMainWindow()`.

Consequences that are now guaranteed by construction:

| Property | How |
| :--- | :--- |
| Taskbar/Alt-Tab icon is the chain-link mark | The window is created by this process; class icon + `WM_SETICON` (`ICON_BIG` 32 px, `ICON_SMALL`/`ICON_SMALL2` 16 px) all resolve resource `#1` |
| One taskbar button for the whole app | `SetCurrentProcessExplicitAppUserModelID(L"ChainIt.Sibling.Hub")` is applied to the process, shared with the hub window |
| No runtime dependency | Plain Win32 + GDI: no Edge, WebView2, Chromium, .NET or PWA install |
| No second UI from a double launch | `FindWindowW(kMainClass)` → restore + `PostMessage(WM_UI_REFRESH_ALL)` → exit |
| Closing the window keeps the service | `WM_CLOSE` hides; the engine, tray icon and `Z:\` mount stay up |
| One window on launch | The mini hub is created hidden; the File Manager is the only window shown |
| A new build replaces an old one | `ChainItBuild` window property + `stopResidentChainIt()` (§8.7) |

### 8.2 Drawing model

`ChainItMainWindowClass` (`CS_DBLCLKS`, `WS_OVERLAPPEDWINDOW`, no background brush, `WM_ERASEBKGND` returns 1)
paints into a memory bitmap and blits once, so there is no flicker and no partial frame. Every visual is GDI:

```
tab strip (Files | Link | Recent | About) + brand  kTabH    = 38
toolbar (14 measured buttons, live enabled state)  kToolH   = 42
per pane: breadcrumb strip                         kPathH   = 26
          column header (Name | Size | Type | Mod) kColHeaderH = 27
          rows                                     kRowH    = 28
status bar (counts · transport · version)           kStatusH = 28
toast pill (amber on refusal, emerald on success), drawn above the status bar
```

Two of those strips are v1.7.0 additions: the tab strip grew **Recent** (keys `1`–`4` select the tabs), and the
Recent tab draws its own taller rows (`kRecentRowH = 46`) because each entry is two lines — the file name, then
`Received · Phone → PC · Bluetooth · pushed by the phone · at 2026-11-17 16:04` — with size, measured MB/s and a
relative timestamp on the right (§10.4). The Files pane also has a **This PC** level above the profile folder,
reached with **Up** or the **Drives** toolbar button (§10.1).

Rows are hand-drawn rather than `SysListView32` on purpose: full control of the dark theme, no header/theming
quirks, identical metrics on every build, and only the visible slice of a 400-row listing is drawn. Columns
degrade with width — below 560 px Type and Modified are dropped, below 400 px only Name remains — and
`WM_GETMINMAXINFO` keeps the window at 720×480 or larger. Dual pane needs 900 px and collapses to a single
pane automatically below that. `SetDCBrushColor`/`SetDCPenColor` with the stock `DC_BRUSH`/`DC_PEN` objects
means no GDI brush or pen is allocated per frame.

Each pane also carries a **live type-ahead filter**. `allItems` holds everything the last listing returned and
`items` is the filtered + sorted view, so `WM_CHAR` appends to `UiList::filter`, `uiApplyFilter()` rebuilds the
view, and selection/focus indices stay parallel to what is actually drawn. `Backspace` edits the filter while it
is non-empty and goes up a level otherwise; `Esc` clears the filter before it clears the selection; navigating
into a folder drops the filter so a stale one cannot make a new folder look empty.

`uiComputeLayout()` is the single source of geometry: it returns every rect (tabs, toolbar buttons, pane
blocks, column header, list area, Link-tab action column) and both the painter and the hit tester consume it,
so a click can never land somewhere the pixels say it should not.

### 8.3 Threading contract

The UI thread never waits on the link. `uiLoadPaneAsync()` spawns a worker that lists a directory (locally with
`listLocalDirJson`, or over LAN/Bluetooth with `httpRequestToPhone`) and posts a heap `ListPayload` back as
`WM_UI_LISTDONE`; renames, deletes, folder creation, transfers and downloads do the same and finish with
`WM_UI_REFRESH`. Toasts are posted too (`WM_UI_TOAST` carrying a heap `ToastMsg`), so the toast string is only
ever written on the UI thread and the painter cannot read a half-assigned `std::wstring`. A 1 s timer refreshes
telemetry and notices the moment a phone appears — it reloads `Z:\` and says so, instead of leaving a stale
"No phone linked yet" panel on screen.

### 8.4 Data contract

Both sides emit byte-identical listing JSON, and the native UI parses it directly with `parseVfsListing()`:

```json
{"currentPath":"/DCIM","items":[{"name":"IMG_1.jpg","path":"/DCIM/IMG_1.jpg","kind":"file",
  "sizeBytes":3312,"modifiedMs":1763299200000,"mimeType":"image/jpeg","ext":".jpg"}]}
```

Since v1.7.0 the same reply also carries `drives[]`, `allowOtherDrives` and `pcRoot`, and may carry `error`
instead of items when a named volume cannot be served — see §10.5. `modifiedMs` is the entry's **real last-write
time** in Unix milliseconds. It used to be `nowMs()` — every one of
up to 400 rows claimed to have been modified *this instant*, so the Modified column showed the current time
everywhere and "sort by date" shuffled arbitrarily (v1.6.0 reads `GetFileTime` with
`FILE_FLAG_BACKUP_SEMANTICS` and converts FILETIME's 1601 epoch). Both native UIs sort on that field, and
`app/server/selftest.ts` asserts the contract the phone relies on (§9.7).

Filenames are UTF-8 on the wire (Android JSON, `fs::u8path` on Windows), so the UI converts with `CP_UTF8`
explicitly rather than the ANSI code page — accents, Cyrillic, CJK and emoji survive. `\uXXXX` escapes are
decoded during parsing. File-type icons come from `SHGetFileInfoW` with `SHGFI_USEFILEATTRIBUTES`, which
resolves an icon from the *name* alone, so a phone-only `.heic` gets the right glyph on a PC that has never
seen one. Icons are cached per extension.

### 8.5 Native prompt

Win32 ships no input box, so Rename and New Folder assemble a `DLGTEMPLATE` in memory (header, `DS_SETFONT`,
four `DLGITEMTEMPLATE` children, DWORD-aligned, no resource entry) and run it with `DialogBoxIndirectParamW`.
That keeps the modal behaviour, tab order, Enter/Escape and system menu correct without adding a `.rc` dialog
or borrowing a browser `prompt()`.

### 8.6 What the web bundle is still for

`app/dist` stays embedded and served: `/mobile` remains available for anyone who *wants* the page in a browser,
`/mini` still renders for remote use, and `/dav` is the WebDAV surface Explorer mounts as `Z:\`. The difference is
that **neither app opens them any more** — the desktop experience is entirely the native Win32 window (§8) and, as of
v1.6.0, the phone's PC browsing is entirely `PcBrowserActivity` (§9). Nothing in either binary launches a browser.

### 8.7 Replacing a build that is already running (v1.5.1)

Removing the browser launch from the source is not sufficient on a machine where ChainIt is already resident, and
two independent bugs let an **older** build keep serving the user after an update.

**Bug 1 - setup could not overwrite a running exe.** Windows locks the image file of a running process, so
`CreateFileW(GENERIC_WRITE)` on `ChainIt-Windows.exe` fails while the engine lives. The installer's fallback posted
`WM_CLOSE` to `ChainItHubWindowClass`, but ChainIt is a resident service: the hub window and the File Manager both
*hide* on `WM_CLOSE` and keep the engine alive. Every retry failed, setup aborted, and the previous build stayed
installed - still launching a browser.

The window that actually stops the engine is the **tray window**, whose `WM_DESTROY` sets `gRunning = false`, deletes
the tray icon and posts `WM_QUIT`. It is created with `HWND_MESSAGE`, which makes it *message-only*: `FindWindow()`
does not enumerate those, only `FindWindowEx(HWND_MESSAGE, ...)` does. `stopRunningChainIt()` therefore

1. opens the single-instance mutex and finds the tray window through `HWND_MESSAGE`,
2. posts it `WM_CLOSE` (`DefWindowProc` -> `DestroyWindow` -> `WM_DESTROY` -> clean exit),
3. polls until the window is gone and then `WaitForSingleObject`s the mutex - the window can disappear a beat
   before the process releases its handles, and it is the *mutex*, not the window, that proves the image unlocked,
4. only then writes the payload, with a bounded retry as a backstop, and
5. if the old instance still will not leave, reports exactly that instead of failing on a locked file.

`/uninstall` runs the same shutdown first, so the delayed self-delete of the exe succeeds.

**Bug 2 - a new launch handed the user back to the old process.** Only one engine can own port 4173 and `Z:\`, so a
second launch defers to the resident instance. Deferring to an *older* build reproduced the old behaviour exactly,
however many times the user reinstalled.

Each build now publishes `kChainItBuildNumber` on its File Manager window with `SetPropW(hwnd, L"ChainItBuild", ...)`.
Window properties live in the window object, so another process can read them with `GetPropW`. On startup:

```
CreateMutexW(Local\ChainItSiblingEngineSingleInstance) -> ERROR_ALREADY_EXISTS?
  live  = FindWindowW(ChainItMainWindowClass)
  build = live ? GetPropW(live, L"ChainItBuild") : 0        // 0 for any pre-1.5.0 build
  build >= kChainItBuildNumber -> restore + WM_UI_REFRESH_ALL + exit   (hand off)
  otherwise                    -> stopResidentChainIt() -> re-acquire  (take over)
                                   still held -> explain how to quit it from the tray, and exit
```

Taking over is the correct default precisely because the property is missing on older builds: `GetPropW` returns 0,
which is less than any current build number, so a pre-1.5.0 instance is always replaced rather than deferred to. The
build number mirrors the APK `versionCode` and rises with every release.

The **About tab shows `Version <x.y.z> (build <n>)`** so a user can confirm in one glance which binary is actually
running instead of guessing from behaviour.

**v1.5.1 was a Windows-only patch** (the APK stayed at v1.5.0 / `versionCode 6`). The Android updater compares the
*downloaded APK's own* `versionCode` with the installed one (`archive.second <= installedCode -> UpToDate`) and never
reads the manifest's top-level `version`, so a mixed manifest cannot make the phone chase a no-op update. **v1.6.0
moves both platforms together** — Windows `1.6.0` / build 8 and Android `1.6.0` / `versionCode 7` — because the
phone's browser jump was live code, not a stale process (§9).

---

## 9. The Native Android PC File Browser (v1.6.0)

### 9.1 Why the phone's browser jump had to go

Windows needed §8 because a browser-hosted UI means somebody else owns the window. The phone had the same defect in
**live code**, not in a stale process:

```kotlin
// MainActivity.buildShareCard(), v1.5.0 and earlier
browsePcBtn = Ds.button(this, "Browse PC files (C:\\)", Ds.ButtonVariant.GHOST) {
    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("http://$ip:4173/mobile")))
}
```

On a phone that costs more than on a desktop: the recent-apps tile and the switcher thumbnail belong to
Edge/Chrome, the page needs Wi-Fi even when Bluetooth is paired and serving, there is no back-stack into ChainIt,
and the label lied — the shared root is the Windows **profile** folder (`%USERPROFILE%`, i.e.
`Desktop / Documents / Downloads / Pictures`), not `C:\`. `grep -rn ACTION_VIEW android/app/src/main/java` now
returns exactly two legitimate uses: the APK-install intent in `UpdateManager`, and "open this *saved file* with
another app" (§9.4). Neither hands ChainIt's own interface to a browser.

### 9.2 One client, two real transports

`net/PcVfsClient.kt` speaks the PC's existing HTTP/1.1 API — no new protocol, no WebView:

| Link | How | Endpoints |
| :--- | :--- | :--- |
| Wi-Fi / hotspot / USB tethering | `HttpURLConnection` to `http://<pcIp>:4173` | `/api/vfs/pc/list`, `raw`, `text`, `mkdir`, `rename`, `delete`, `upload` |
| Bluetooth RFCOMM, zero Wi-Fi | hand-written HTTP/1.1 over `createRfcommSocketToServiceRecord(c4a1b170-0002-…)` | the same paths — `bluetoothServerLoop()` hands every accepted socket to `handleHttpClient` |

Bluetooth is a **full link, not a degraded fallback**: the PC publishes its whole HTTP API on the receiver UUID the
phone already dials for file pushes, so browsing works with Wi-Fi, hotspot and USB all off. Order is Wi-Fi first,
then RFCOMM; a `4xx`/`5xx` counts as an *answer* (never re-asked over Bluetooth) while a transport failure falls
through. `connect()` runs on a worker thread joined with a 12 s deadline, because a paired-but-asleep radio
otherwise blocks the dial for tens of seconds; secure RFCOMM is tried first and insecure second, matching the
phone's own listener.

### 9.3 Streaming, and why the sink is re-opened per attempt

Downloads write straight to `FileOutputStream` and uploads read straight from the content resolver, so a 2 GB video
never enters the heap (in-memory answers are capped at 24 MB and are only listings, JSON and text). `BodySink` is a
*factory*, not a stream: each transport attempt opens its own file, so a Wi-Fi download that dies half-way cannot
leave its bytes in front of the Bluetooth attempt's. Response framing is done by a small `ByteSource` that buffers
one read at a time and handles fixed `Content-Length`, `Transfer-Encoding: chunked`, and read-to-close — the same
three cases the Windows RFCOMM pool handles. Uploads must know their length before dialling (the PC reads a fixed
`Content-Length`), so a file whose size Android will not report is buffered, and refused with an explanation when it
is too large to buffer.

### 9.4 The screen itself

`ui/PcBrowserActivity.kt` is built from the same `Ds` tokens as the rest of the app (no XML, no androidx):

- header with logo, target label and a live transport pill (`WI-FI` / `BLUETOOTH` / `NO LINK`);
- path bar with **Up** (dimmed at the root) and **Refresh**, plus a tappable path offering the root and
  `Desktop / Documents / Downloads / Pictures / Music / Videos`;
- **New folder**, **Send file here** (upload into the folder being viewed) and **Sort**;
- tappable `NAME · SIZE · MODIFIED` column headers — four sort modes, direction toggles on a second tap, folders
  first, and an explicit `Comparator` (never `compareBy { }`, which is `invokedynamic` in the published stdlib);
- type badges (`DIR`, `PDF`, `JPG`, …) tinted by category instead of icon fonts, so nothing depends on a glyph a
  device may not ship;
- tap a file for an action sheet, long-press for multi-select with a **Save / Delete / Clear** bar;
- text opens in a native selectable `TextView` via `/api/vfs/pc/text`; images are downloaded to cache and decoded
  with `inSampleSize` into an `ImageView`; anything else can be saved to Downloads and handed to another app as
  `content://` through `net/ChainItFileProvider.kt`, which resolves and then confines every path to ChainIt's own
  directories;
- saves land in `Downloads/ChainIt` when the app may write public storage, otherwise in app-private storage, and the
  toast says which — followed by `MediaScannerConnection.scanFile` so the file shows up in Files/Gallery at once.

### 9.5 Refusals are sentences

The empty state is a card, not a blank list: it repeats the transport's own explanation and offers **Try again**
(with an IP field), **Bluetooth settings** and **Wi-Fi settings**. Distinct wording exists for no radio, radio off,
nothing paired, a 12 s dial timeout, a refused service, 401 (Pairing Key mismatch), 403 (Sibling Mode off on the
PC), 429 (30 s lockout), 404 (file vanished), 503, a truncated body ("sent N of M bytes"), and a phone that cannot
write its own storage. The status line always shows item count, folder count, total bytes, transport and round-trip
milliseconds, so "it worked" is observable rather than assumed.

### 9.6 The guard that keeps it that way

`android/build-apk.sh` fails the build if any Android source opens `http://…:4173` with `ACTION_VIEW`, and the
final `classes.dex` is asserted to contain `PcBrowserActivity`, `PcVfsClient` and `ChainItFileProvider` alongside
the classes already guarded. `verify_dex_safety.py` still proves all 208 first-party classes (183 at v1.6.0, +25 in
v1.7.0) are
`invokedynamic`-free and that no source touches an indy-emulated stdlib method or parses a colour at runtime.

### 9.7 Contract test

`app/server/selftest.ts` §6 exercises exactly what the phone calls: the listing shape (all seven item fields,
virtual paths inside the root, folders before files, `modifiedMs` finite and never in the future), then
`mkdir → upload → list → raw?download=1` (with `Content-Disposition: attachment`) `→ text → rename →` a **404** for
the renamed-away file `→ delete`. If that shape changes, the phone's native browser breaks on a device nobody can
debug from a PC — so the test fails first.


---

## 10. Volumes, Provenance and Battery Telemetry (v1.7.0)

Three separate gaps closed in one release, all of them "the feature looked done in the web preview but the native
apps did not have it":

| Ask | What was true before | What is true now |
| :--- | :--- | :--- |
| "Show my phone's battery again" | Engine parsed `batteryPct` from `/chainit/ping`; only the **web** UI drew it. The native Win32 window had no battery anywhere. | Native gauge + rows on the Link tab, status-bar text on every tab, an Android BATTERY tile, plus charge/plug/temperature — and an explicit *not live* state when no real phone has answered. |
| "Recent shared files" | `gState.transfers` held one aggregate row per batch, was cleared at exit, and reported a hard-coded `88.5 MB/s` / `durationMs: 8`. | One row **per file** with direction, transport, measured MB/s, `localPath`, `virtualPath` and `source` (who moved the bytes), capped at 60, persisted in `%APPDATA%\ChainIt\transfer-history.json` and reloaded at startup. |
| "Access other drives, not only C:" | `/` mapped to `%USERPROFILE%` and `resolveLocalSafe()` confined everything inside it — no second partition, card reader, USB stick or mapped share was reachable. | `/` still means the profile folder; a virtual path may name a volume (`/D:/Games`), confined to *that* volume. Volumes are enumerated and published inline with every listing. |

### 10.1 Addressing a volume without breaking any existing client

The VFS root has always been `%USERPROFILE%`, not `C:\`. Every client — the native pane, `Z:\`, the phone's
`PcVfsClient` — sends `/`, `/Documents`, `/DCIM/Camera`. So the change had to be **additive at the parser level**:

```
resolveLocalSafe("/D:/Games/x.txt")  →  D:\Games\x.txt      (canonical, confined to D:\)
resolveLocalSafe("/Documents/a.txt") →  %USERPROFILE%\Documents\a.txt   (unchanged)
resolveLocalSafe("D:\Games")        →  D:\Games\x.txt      (a Windows path is accepted too)
```

`virtualDrivePrefix()` detects a leading `<letter>:` (with or without the slash) and hands back the volume root plus
the remainder; the remainder is then canonicalised **inside that volume** and rejected if the canonical path escapes
it — a `..` walk lands on the volume root rather than on the profile folder or another drive. `toVirtualPosix()` is the
inverse and is volume-aware, so a listing under `D:\Games` reports `virtualPath: "/D:/Games"` and the phone can feed
that string straight back.

One path is deliberately **not** real: the native UI's *This PC* level uses the synthetic `kPcThisPc = L"@thispc"`.
It cannot collide with anything because real virtual paths start with `/`, and it keeps Explorer's navigation model
(**Up** from the profile folder → the drive list → double-click a drive → back into files) without inventing a
second root in the wire protocol. Every `uiOp*` action is guarded by `uiBlockAtThisPc()` so a file operation at that
level is dimmed instead of acting on "no file".

### 10.2 Enumeration must never block the thread that draws

`enumerateDrives()` is `GetLogicalDrives` → per bit `GetDriveTypeW` → for local types only, `GetVolumeInformationW`
(label, filesystem) and `GetDiskFreeSpaceExW` (capacity). Two rules came out of testing against a dead mapped share:

1. **`DRIVE_REMOTE` is listed from `GetDriveTypeW` alone** with `ready: false` and zero sizes. Probing a network
   volume whose server is gone blocks the caller for seconds — and this runs on the UI thread, so the window freezes.
2. **No capacity is invented.** An empty card reader reports `ready: false` and `0` bytes; the UI prints
   `(no media)` instead of a plausible total.

The result is cached for 3 s (`pcVolumesJson()`), because the Files pane repaints on hover and a repaint must not
re-walk the volume table.

### 10.3 Widening reach is a decision, so it is a switch that lives on the PC

Exposing every volume to a phone over Bluetooth is a real privacy change, so it is not a silent default and it is not
a client-side preference either:

- `%APPDATA%\ChainIt\settings.json` stores `allowOtherDrives` (default **on** for a PC that has never been asked,
  because the user's own machine and their own phone are the only parties).
- `POST /api/drives/scope {"allowOtherDrives":bool}` flips it, persists it, logs it, toasts it and returns the new
  volume table. The native Link tab's 8th button and the phone's Drives sheet both call it — the phone can ask, but
  the state and the UI that shows it belong to the PC.
- When it is off, `/D:/…` is refused with the PC's own sentence rather than a bare `400` or a silent fallback to the
  profile folder (§10.5).

### 10.4 Shared-file history: per file, measured, attributed, persisted

`recordTransfer()` is called **once per file** at the point the bytes were verified, with the real elapsed time, so
`effectiveSpeedMBps` is measured rather than a constant. Each entry carries:

```
{ id, label, op, direction, transportUsed, localPath, virtualPath, source,
  promotedHotspot, bytes, durationMs, effectiveSpeedMBps, status, timestampMs }
```

`source` is the part that makes an entry honest about *who* moved the bytes — the user asked specifically for
"plus some other unique features like" that:

| `source` | Meaning | Recorded by |
| :--- | :--- | :--- |
| `ui` | Started in the native Windows File Manager | `uiOpCopyMove` / the pane's upload + save paths |
| `api` | Started by a client call (`POST /api/vfs/transfer`, WebDAV) | the transfer route |
| `phone-upload` | The phone pushed the file — possibly with its screen off | `POST /api/vfs/pc/upload` |
| `phone-download` | The phone saved a file from the PC | `GET …/raw?download=1` |

An image **preview** (`/raw` without `download=1`) is deliberately not recorded: opening a thumbnail is not sharing a
file, and a history full of previews would bury the real transfers.

**Every recording path measures.** That took a second pass, because "not hard-coded" is not the same as "measured":

- the Windows `/raw?download=1` path recorded `0.0` MB/s. Since `TransferItem` stores a *rate* and
  `historyJsonLocked()` derives `durationMs` from it (`bytes / rate`), a zero rate produced `durationMs: 0` too, so a
  saved file reached the Recent tab with no rate **and** no duration. It is now timed across the read *and* the send -
  the interval the phone actually waited through - and recorded after the response goes out.
- the node preview's upload route quoted the transport's nominal `speedMBps` (94.5), so a 44-byte upload claimed
  94.5 MB/s while the download of the same file correctly measured 0.02 MB/s. Its transfer path did the same with
  `linkSpeed || 85`. Both now derive the rate from the bytes and elapsed time they already had.

A nominal speed is the same lie as the old constant, just wearing the transport's number. The contract test therefore
asserts the rate is **reproducible from the entry's own fields** - `|effectiveSpeedMBps - bytes/durationMs|` within
tolerance, and `durationMs >= 1` - for both a phone upload and an explicit save, so a nominal rate fails the build
instead of reaching a user. (Checked by reverting one line: the assertion reports
`effectiveSpeedMBps 72.4 is not what 21 B in 1 ms implies (0.02)`.)

Persistence is plain JSON with a schema number (`{"schema":2,"transfers":[…]}`), loaded at startup before the first
snapshot, bounded at 60 entries, and written on every append. The uninstaller already removes `%APPDATA%\ChainIt`,
so a clean uninstall leaves no history behind.

Endpoints: `GET /api/history?limit=25` (returns `items[]` + the `historyFile` path, because "where is this stored"
should be answerable), `POST /api/history/clear`, and `POST /api/pc/reveal {"path"}` which runs
`explorer /select,<local>` after checking the file exists (404 otherwise).

### 10.5 Refusals carry the runtime's own sentence

A listing of a volume that cannot be served answers **200** with `items: []` and an `error` string, never a 400 and
never an empty list with no explanation — the phone shows that sentence verbatim, and the native pane paints it in
the row area:

- `Drive Q: is not present.` (volume letter absent)
- `Access to drives other than C: is switched off on the PC (Link tab).` (reach off)

Every listing also carries `drives[]`, `allowOtherDrives` and `pcRoot` **inline**. Over Bluetooth a second round trip
is the expensive part of the interaction, so a client must be able to offer the drive picker and report the reach
state from the reply it already has.

### 10.6 Battery telemetry: real values, or an explicit "not live"

The phone reads `BatteryManager` plus the sticky `ACTION_BATTERY_CHANGED` broadcast in `net/BatteryTelemetry.kt` and
publishes `batteryPct`, `charging`, `chargeSource` (`ac|usb|wireless|none`) and `temperatureC` in **both** the
`/chainit/ping` reply and the UDP sibling beacon. The engine stores `phoneSeenMs` when a real phone answers, and the
snapshot derives:

- `phoneTelemetryAgeMs = now - phoneSeenMs`
- `phoneTelemetryLive = age < 180 s`

`phoneSeenMs == 0` means no phone has ever answered, and the UI says so — `awaiting the phone's first report` —
instead of drawing a gauge at a simulated 88%. A stale report still shows the last known number, but greyed and
labelled `last reported 12 min ago`. That distinction is the whole point: a plausible number with no phone attached
is worse than no number.

### 10.7 The node preview mirrors the contract, honestly

`app/` is a preview runtime, not the product, but it is what the contract test exercises — so it implements the same
wire shapes with the same semantics and its own limits stated rather than faked:

- `vfsService.listVolumes()` uses `fs.statfsSync` (guarded; reports `0` rather than guessing) and serves exactly
  **one** volume, the seeded `.runtime/pc-drive`, marked `isPcRoot: true`. It never invents `D:`/`E:`.
- A request for an unserved letter is refused with `Drive Q: is not present on this PC (the node preview serves C:
  only).` — the preview's own sentence about the preview.
- `POST /api/pc/reveal` returns **501** `{ok:false}` with `path` and `localPath` and says the node preview cannot open
  Explorer, instead of pretending.
- History and settings persist to `.runtime/transfer-history.json` and `.runtime/settings.json` (label surfaced as
  `historyFile`), and `engineState` recomputes `phoneSeenMs` / `phoneTelemetryLive` per snapshot on the same 3-minute
  window.

### 10.8 Contract test

`app/server/selftest.ts` §7 pins all of it: a listing carries `drives[]` with exactly one `isPcRoot` whose
`virtualPath` is `/`; `/api/drives` names the root letter; an absent volume answers with `items: []`, a non-empty
`error` **and** the volume list; flipping the reach switch off changes the refusal sentence and back on restores it;
an upload then appears in `/api/history` with `source: "phone-upload"`, `direction: "phone_to_pc"` and a `virtualPath`
that ends with the filename, and a rate consistent with its own `bytes`/`durationMs`; an explicit `raw?download=1` adds
a `phone-download` entry whose duration is real and non-zero; `/api/pc/reveal` resolves a
real path and 404s on a missing one; `history/clear` reports how many entries went and leaves the list empty; and the
snapshot carries the five battery fields, `pcVolumes[]` and `allowOtherDrives`.
