# ChainIt

**ChainIt** connects your **Android phone** and **Windows laptop** as seamless **"Siblings"**—combining **automatic offline/online multi-transport switching**, **zero-touch auto-connection**, a **real Windows `Z:\` virtual drive**, and a **dedicated dual-pane ChainIt File Manager** so you can browse, copy, paste, drag-and-drop, move, and edit phone files just like a local hard disk without ever touching or unlocking your phone.

---

## Ready-to-Install Final Releases (`releases/`)

Both standalone, zero-dependency binaries are compiled and ready to use right now:

| Platform | File | Size | Details |
| :--- | :--- | :--- | :--- |
| **Windows 10 / 11 (64-bit)** | [`releases/ChainIt-Windows.exe`](./releases/ChainIt-Windows.exe) | ~950 KB | **v1.1.0** — Single portable `.exe` (no installer, no runtime, no extra DLLs). Includes System Tray icon, Smart Link UDP auto-discovery, **native Bluetooth RFCOMM sockets (`AF_BTH` / `ws2bth.h`) for both directions**, automatic `Z:\` File Explorer drive mounter, and the dedicated ChainIt File Manager window. |
| **Android 8.0 – 15.0+** | [`releases/ChainIt-Android.apk`](./releases/ChainIt-Android.apk) | ~832 KB | **v1.1.0 (`versionCode 2`)** — V2+V3 signed native Kotlin APK (`com.chainit.android`). Runs the screen-off `SiblingDaemonService` over USB Tethering, Wi-Fi LAN, Local Hotspot, **or pure Bluetooth RFCOMM** with zero confirmation prompts. |

**Direct downloads (always the latest build on this branch):**
- Windows: <https://github.com/mubareksam/chainit/raw/arena/37fd3890-chainit/releases/ChainIt-Windows.exe>
- Android: <https://github.com/mubareksam/chainit/raw/arena/37fd3890-chainit/releases/ChainIt-Android.apk>

---

## One-Time Setup Guide (Step-by-Step)

### Part A — On Your Android Phone (2 Minutes, Once Only)
1. Copy or download **`ChainIt-Android.apk`** onto your Android phone and tap it to install *(if prompted by Android Chrome/Files, allow "Install unknown apps" once)*.
2. Open **ChainIt Sibling** on your phone:
   - Tap **`1. Tap to Grant All-Files Access`** and switch **Allow access to manage all files** to **ON**. *(Required once so Windows `Z:\` can read/write all phone folders without popups).*
   - Tap **`2. Tap to Allow Screen-Off Background Standby`** and tap **Allow**. *(Prevents Android from sleeping the connection when you lock your phone screen and put it in your pocket).*
   - Tap **`3. Tap to Allow Bluetooth Connectivity`** *(Android 12+)* and allow **Nearby devices / Bluetooth**. The line turns green — `✓ 3. Bluetooth Sibling Socket Ready (RFCOMM Active)` — when the native Bluetooth server is live. *(Only needed if you want Bluetooth mode.)*
   - **For Bluetooth-only use:** pair the phone with your PC **once** (Android *Settings → Bluetooth* ↔ Windows *Settings → Bluetooth & devices*). ChainIt reuses that pairing to open RFCOMM sockets automatically in the background — no re-pairing and no confirmation prompts.
3. Leave the **Sibling Mode** switch **ON** *(default Sibling Key is `CHAINIT-SIBLING`)*. You can now lock your phone screen and put it away!

### Part B — On Your Windows 10 / 11 Laptop
1. Download **`ChainIt-Windows.exe`** (e.g., onto your Desktop) and double-click it.
   - *If Windows SmartScreen appears (because this is your custom self-signed build)*: click **More info** $\rightarrow$ **Run anyway**.
   - *When Windows Defender Firewall prompts*: check both **Private** and **Public** networks and click **Allow access** *(needed for automatic offline Wi-Fi & Hotspot discovery)*.
2. ChainIt adds an icon to your **Windows System Tray** (bottom-right clock area) and opens the **ChainIt File Manager** window automatically.
   - *(Optional — Start with Windows)*: Press `Win + R`, type `shell:startup`, press Enter, and drop a shortcut to `ChainIt-Windows.exe` there so Sibling Mode is always standing by when you turn on your laptop.

### Part C — Connecting Offline or Online (Automatic & Manual)
ChainIt automatically detects and scores whichever connection is fastest—no "Connect" or "Accept" clicks needed:
- **Same Wi-Fi Router (Wireless LAN)**: Automatically discovers your phone in `< 2 seconds`.
- **100% Offline Anywhere (No Router / No Internet)**: Turn on **Mobile Hotspot** on your Android phone (or **Mobile Hotspot** in Windows 11 Quick Settings) and join it from the other device. Works completely offline at full 5 GHz Wi-Fi speed!
- **Fastest Speed (~420 MB/s Wired)**: Plug in a USB cable and turn on **USB Tethering** in Android Settings. Automatic Mode immediately switches to USB Direct.
- **Native Bluetooth — Zero Wi-Fi, Zero Hotspot, Zero Cable, Zero Mobile Data**: After one pairing, ChainIt opens **real RFCOMM sockets** (not file-push profiles): Windows uses `socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)` from `ws2bth.h` against the phone's service UUID `c4a1b170-0001-4000-8000-00805f9b34fb`, and Android uses `BluetoothServerSocket` / `BluetoothSocket`. `Z:\` browsing, copy/paste, cut/move, inline preview and Phone → PC pushes all keep working with **Wi-Fi, Hotspot and USB all OFF** (~0.3–2.4 MB/s). Automatic Mode keeps it as the always-warm last-resort link; Manual Mode can pin it.
  - **Manual Bluetooth link**: in ChainIt File Manager → *Connectivity*, type the phone's Bluetooth address as `bt://AA:BB:CC:DD:EE:FF` (a bare `AA:BB:CC:DD:EE:FF` works too) and press **Connect Link**.
  - **Phone → PC over Bluetooth only**: in the Android app tap **`Send File via Bluetooth Only (Works with Wi-Fi OFF)`** — the file lands in `C:\Users\<you>\Desktop` through the PC's published receiver UUID `c4a1b170-0002-4000-8000-00805f9b34fb` (Windows publishes the SDP record with `WSASetService` on an `AF_BTH` listening socket).
  - **No firewall prompt**: Bluetooth sockets bypass the Windows Firewall entirely, so Bluetooth mode works even on locked-down/public networks where LAN discovery is blocked.
- **What Happens Automatically**:
  1. **Dedicated ChainIt File Manager**: Left pane shows your Windows files (`C:\`), right pane shows your real Android storage (`Z:\`)—drag-and-drop, `Ctrl+C` / `Ctrl+V`, `Ctrl+X` (Move), rename, delete, or double-click to preview/edit files directly on your phone.
  2. **Windows File Explorer (`Z:\` Drive)**: Open `This PC` in Windows Explorer (`Win + E`)—your phone is mounted as **`Z:\`** like a local hard drive! *(If Windows `WebClient` service was disabled on your PC, run `powershell -ExecutionPolicy Bypass -File .\scripts\mount-z-drive.ps1` once as Administrator to set `WebClient` to Automatic and enable 4 GB file support).*

---

## Repository Source Structure

| Path | Description |
| :--- | :--- |
| [`releases/`](./releases) | Compiled final deliverables: `ChainIt-Windows.exe` & `ChainIt-Android.apk` |
| [`ARCHITECTURE.md`](./ARCHITECTURE.md) | Complete protocol (`CHNP/1.0`), transport scoring formula, Sibling crypto, and VFS design |
| [`windows/src/chainit_win_main.cpp`](./windows/src/chainit_win_main.cpp) | Native Win32 C++17 single-binary source (System Tray, UDP discovery, **`AF_BTH` RFCOMM client + published SDP receiver + `WSALookupService` paired-device discovery**, multipath WebDAV `Z:\` mounter, embedded UI server) |
| [`android/`](./android) | Native Kotlin Android source (`SiblingDaemonService`, `PhoneHttpAndWebDavServer`, **`BluetoothRfcommTransport`**, `UdpSiblingDiscovery`, `AndroidStorageVfsProvider`, `MainActivity`) |
| [`crates/`](./crates) | Shared Rust core (`chainit-core`) & Windows library (`chainit-windows`) |
| [`app/`](./app) | ChainIt File Manager React/TypeScript UI + Node live preview server |
| [`scripts/`](./scripts) | PowerShell `Z:\` drive helpers (`mount-z-drive.ps1`, `unmount-z-drive.ps1`) & asset embedder |
