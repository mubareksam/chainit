# ChainIt

**ChainIt** connects your **Android phone** and **Windows laptop** as seamless **"Siblings"**—combining **automatic offline/online multi-transport switching**, **zero-touch auto-connection**, a **real Windows `Z:\` virtual drive**, and a **dedicated dual-pane ChainIt File Manager** so you can browse, copy, paste, drag-and-drop, move, and edit phone files just like a local hard disk without ever touching or unlocking your phone.

---

## Ready-to-Install Final Releases (`releases/`)

All three artifacts are compiled, signed and committed — pick the portable exe, the setup program, or the APK:

| Platform | File | Size | Details |
| :--- | :--- | :--- | :--- |
| **Windows 10 / 11 (64-bit)** | [`releases/ChainIt-Windows.exe`](./releases/ChainIt-Windows.exe) | ~1.37 MB | **v1.5.1** — Portable `.exe`. **Fully native: the File Manager is a Win32 window this process draws itself**, so ChainIt never launches Edge (or any browser) and the taskbar button is ChainIt’s own chain-link icon — not a globe. v1.5.1 additionally **replaces a build that is already running**: a new launch closes an older resident instance instead of handing the user back to it, which is what kept the browser-opening build alive after an update. Dual-pane PC + `Z:\` browser, sortable columns, shell file-type icons, right-click menu, type-ahead filter, cut/copy/paste between devices, Link + About tabs. Built by **Qmer**. |
| **Windows installer** | [`releases/ChainIt-Setup-1.5.1.exe`](./releases/ChainIt-Setup-1.5.1.exe) | ~1.68 MB | **v1.5.1** — Setup program with **no third-party packaging tool**: the stub carries the very same `ChainIt-Windows.exe` as an appended payload plus a SHA-256 trailer it verifies before touching disk. It now **closes a running ChainIt properly before overwriting the exe** (Windows locks a running image, so the old build used to survive the install). Per-user install into `%LOCALAPPDATA%\Programs\ChainIt` (no UAC prompt), Desktop + Start Menu shortcuts, Apps & features entry with a working uninstaller, `/S` silent. Built by **Qmer**. |
| **Android 8.0 – 15.0+** | [`releases/ChainIt-Android.apk`](./releases/ChainIt-Android.apk) | ~872 KB | **v1.5.0 (`versionCode 6`)** — unchanged by the v1.5.1 Windows fix, so there is no need to update your phone. V2+V3 signed Kotlin APK on the same certificate as every previous build, so it upgrades the installed `com.chainit.android` in place. The VFS server speaks **HTTP/1.1 keep-alive**, so one Bluetooth RFCOMM link serves a whole `Z:\` session; the listener falls back from the secure to the insecure channel and re-arms itself forever. Runs as a **`connectedDevice`** foreground service with a 9-minute Doze heartbeat, self-renewing wake lock and recents-swipe restart, so **a phone locked all night keeps serving**. Built by **Qmer**. |

**Direct downloads (always the latest build on this branch):**
- Windows (portable): <https://github.com/mubareksam/chainit/raw/main/releases/ChainIt-Windows.exe>
- Windows (installer): <https://github.com/mubareksam/chainit/raw/main/releases/ChainIt-Setup-1.5.1.exe>
- Android: <https://github.com/mubareksam/chainit/raw/main/releases/ChainIt-Android.apk>
- Release manifest (version + SHA-256 of all three artifacts): [`releases/version.txt`](./releases/version.txt)

### What is new in v1.5.1

> **A Windows-only patch.** The phone app is untouched at v1.5.0 (`versionCode 6`) — there is nothing to update there.

v1.5.0 removed every browser launch from the source, but a machine that already had ChainIt running could still end
up looking at Edge. Two separate bugs, both of which let the **old** build stay in charge:

1. **Setup could not replace a running ChainIt, so the old exe survived the install.**
   Windows locks the image file of a running executable, so writing the payload over an active install fails. The
   installer’s fallback posted `WM_CLOSE` to the hub window — but ChainIt is a resident service and its windows
   *hide* on `WM_CLOSE` rather than quitting, so the engine kept running, kept the file locked, and every retry
   failed. Setup aborted while the **previous build stayed installed** and kept launching a browser.
   - The window that actually stops the engine is the **tray window**, and because it is created with `HWND_MESSAGE`
     it is *message-only*: plain `FindWindow` cannot see it at all, only `FindWindowEx(HWND_MESSAGE, …)` can.
     Setup now finds it that way, closes it, and waits on the single-instance mutex until the process has really
     gone — **before** it writes a single byte. If something still refuses to leave, it says so plainly instead of
     failing with a locked file.
   - `/uninstall` closes the resident engine first too, so the delayed self-delete of `ChainIt-Windows.exe` succeeds.
2. **A new launch handed the user back to the old resident process.**
   ChainIt allows only one engine (port 4173 and `Z:\` have a single owner), so a second launch used to find the
   running instance and defer to it. If that instance was an older build, deferring meant the user got the old
   behaviour — including “Open File Manager” launching Edge — no matter how many times they reinstalled.
   - Each build now publishes its number on the File Manager window as the cross-process-readable `ChainItBuild`
     property. A launch compares it with its own: **current or newer → hand off** (restore the window, refresh both
     panes, exit); **older → close it and take over**, so the build you just installed is the one that runs.
   - If an old instance cannot be closed, ChainIt says exactly what to do rather than starting a second engine that
     cannot bind port 4173: *right-click the tray icon → “Unmount `Z:\` && Exit ChainIt”, then start again.*

**How to tell which build you are actually running:** open the **About** tab. v1.5.1 reads
`Version 1.5.1 (build 7) · native Win32 · no browser, no runtime`. If it says 1.5.0 or 1.4.0, the old process was
still resident — quit it from the tray and start the new one.

### What is new in v1.5.0

> **ChainIt on Windows is now a genuinely standalone native application.** No browser is involved at any point.

1. **The File Manager is drawn by ChainIt itself — Edge is gone.**
   Until now the desktop UI was opened with `ShellExecuteW(msedge.exe, "--app=http://127.0.0.1:4173")`. That hands the
   window — and therefore the **taskbar button** — to a *browser* process, which is exactly why Windows showed
   Edge’s **globe/internet icon** while ChainIt was running, and why no icon compiled into the `.exe` could change it.
   Both browser launchers (`launchDesktopUiWindow`, `launchMiniPanelWindow`) have been **deleted**. The File Manager is
   now a `WS_OVERLAPPEDWINDOW` window of class `ChainItMainWindowClass`, painted with GDI into an off-screen bitmap:
   tab strip, toolbar, breadcrumb strip, sortable column header, rows with shell file-type icons, a slim scrollbar, a
   status bar and an amber/emerald toast pill. One process, one window, one taskbar button — the chain-link mark.
   - **Nothing to install or enable**: no Edge, no WebView2, no Chromium runtime, no `.NET`, no PWA. The window is
     ordinary Win32 + GDI, so it behaves identically on every Windows 10/11 build.
   - **Icons are loaded at exact sizes** (16 px for the caption and alt-tab, 32 px for the class, 64 px for the About
     tab) from resource `#1` — the same artwork as the Android launcher and the tray — and set with
     `WM_SETICON`/`ICON_SMALL2`, so nothing is stretched or blurred.
   - **The process’s AppUserModelID is `ChainIt.Sibling.Hub`**, so the File Manager, the mini hub and any dialog group
     under a single taskbar button wearing ChainIt’s icon.
   - **Launching ChainIt twice no longer opens a second UI**: the second process finds the live window with
     `FindWindowW`, restores it, posts `WM_UI_REFRESH_ALL` and exits.
2. **Everything the browser UI did, and a few things it could not.**
   - **Dual pane by default**: `This PC` on the left, the phone (`Z:\`) on the right, each with its own breadcrumb,
     sort state, selection and scroll. Collapse to one pane with the *Split view* button or the *PC*/*Phone* buttons.
   - **Sort by Name / Size / Type / Modified** by clicking a column header (click again to reverse), folders first.
   - **Real shell icons per file type**, resolved with `SHGFI_USEFILEATTRIBUTES` so phone-only files get the right
     icon even though they do not exist on this PC.
   - **Right-click menu**: Open, Open in Explorer (PC), Send to phone / Save to PC, Save to Downloads, Cut, Copy,
     Paste, Rename, Delete, New folder, Refresh, Split view.
   - **Explorer key bindings**: `Ctrl+A/C/X/V`, `Del`, `F2`, `Enter`, `Backspace` or `Alt+↑` (up a level), `F5`,
     `Ctrl+Shift+N`, `F6` (switch pane), `Ctrl+T` and `1`/`2`/`3` (switch tab).
   - **Live type-ahead filter**: just start typing and the active pane narrows to matching names, with an emerald
     chip in the breadcrumb strip showing the text and a `12/400` match count. `Backspace` edits it, `Esc` clears
     it, `Ctrl+F` restarts it, and navigating into a folder drops it so a stale filter can never make a new folder
     look empty.
   - **Cut/copy/paste moves files between the two devices**, through the same integrity-gated 2 MiB chunk pipeline as
     `Z:\` — SHA-256 per chunk and per file, partial data in a hidden `.chainit-part` that only replaces the target
     once it verifies. A transfer is never shown as done when it was not.
   - **Double-clicking a phone file downloads it and opens it** with the right Windows app; nothing is opened from a
     half-written file.
   - **Rename and New folder use a native modal dialog built in memory** (`DialogBoxIndirectParamW`) — Win32 has no
     input box and this app will not borrow a browser `prompt()`.
3. **The UI thread never blocks on the link.**
   Listing the phone over Bluetooth can take seconds, so every listing and every transfer runs on a worker thread and
   posts its result back (`WM_UI_LISTDONE`). While it works the pane says *reading…*; a refusal is reported as a
   real reason in an amber panel (HTTP 401/403/429 message, RFCOMM silence, no phone paired) together with the retry
   hint, and a failed phone read forces an immediate transport re-scan.
4. **New Link and About tabs.**
   - **Link** shows the live picture: active transport, phone endpoint, peer device, Sibling Mode, `Z:\` mount state,
     phone storage, discovered endpoints, the RFCOMM channel pool (`n of 3 keep-alive channels open`), cached SDP
     channel, requests served, re-dials, last round-trip, radio/receiver state and the paired Bluetooth devices —
     plus one-click actions: rescan every transport, mount/refresh `Z:\`, open `Z:\` in Explorer, create the desktop
     shortcut, show the mini hub, update in place, copy the pairing key.
   - **About** carries the version, the **Built by Qmer** credit and six tiles explaining what the build guarantees.
5. **Filenames survive every language.** The listings on both sides are UTF-8, and the UI converts through `CP_UTF8`
   explicitly rather than the ANSI code page, so accents, Cyrillic, CJK and emoji render correctly instead of turning
   into question marks.
6. **Closing the window does not kill the service.** The File Manager hides, while the engine, the tray icon and the
   `Z:\` mount keep running; *Unmount `Z:\` && Exit ChainIt* in the tray is still the way out.

### What is new in v1.4.0

> **Built by Qmer** — credited on the Windows About tab, in the native hub window, in the Android app footer,
> and in the `VERSIONINFO` of both exes (`CompanyName` = Qmer, `LegalCopyright` = "Built by Qmer").

1. **Bluetooth now behaves like Wi-Fi/LAN — a real `Z:\` drive, not a status line.**
   The link was up before but effectively did nothing, for three concrete reasons, all fixed:
   - *Windows dialled the radio for every single request.* An RFCOMM connect is an SDP lookup plus a page
     (1–5 s), and a global mutex meant only one request could ever be in flight. Explorer issues dozens of
     requests to draw one folder, so the queue never drained. ChainIt now keeps **three persistent channels**
     open and reuses them as **HTTP/1.1 keep-alive** sessions, each with its own mutex so they run in
     parallel. One dial serves a whole browsing session.
   - *The phone answered exactly one request per connection.* `handleStreamSession` now loops: it parses as
     many requests as the peer pipelines, frames each response by `Content-Length`/chunked instead of EOF,
     drains any body a handler did not consume (so upload bytes can never be mis-read as the next request
     line), and flushes per response.
   - *`Z:\` faked success.* When the phone did not answer, the `/dav` handler served a synthetic empty drive, so
     Explorer showed a mounted disk with nothing in it — the literal "connected but does nothing". It now
     returns **503 with the actual reason**, logs it, forces a transport re-scan, and publishes it as
     `linkProblem` in `/api/state`, which the UI shows in an amber banner and the hub window shows as its
     headline. Before any phone has ever paired, `Z:\` serves one readable `READ ME FIRST` file instead of a void.
   - Also: the SDP-discovered RFCOMM channel number is cached (later dials skip SDP), the Android listener
     falls back from the secure to the **insecure** channel that many desktop stacks require, and a watchdog
     re-arms it forever. Live telemetry (`pooledChannels`, `requestsServed`, `redials`, `lastRttMs`) is in
     `/api/state` and on the About tab, and the Android footer shows its own RFCOMM readout.
2. **A phone locked for hours keeps working.**
   - The service now runs as **`connectedDevice`**, not `dataSync` — Android 14 time-caps `dataSync`
     foreground services at roughly 6 h per 24 h and then stops them, which is exactly the "worked, then died
     overnight" failure.
   - A **Doze heartbeat** (`setExactAndAllowWhileIdle`, 9-minute cadence, the minimum Android allows) runs a
     self-check that restarts the VFS server, the RFCOMM listener or UDP discovery if any of them died, and
     re-takes the Wi-Fi/multicast locks.
   - The partial wake lock **renews itself** 30 minutes before its 12-hour ceiling, so there is never an
     unlocked gap.
   - Swiping ChainIt out of recents (`onTaskRemoved`) schedules an immediate restart.
   - The battery-optimisation exemption is surfaced as a setup row with a tap-to-allow intent.
   - Survived heartbeats are shown in the notification and the UI as proof of life.
3. **A refusal is never silent.** The Pairing Key is still mandatory, but a *correct* key with Sibling Mode
   switched off now answers **403 with the exact remedy** ("enable Sibling Mode on the phone") instead of a
   401 that looks like a dead link. Wrong keys still 401 and still trip the brute-force lockout.
4. **About tab.** New fourth tab in the Windows/browser UI: attribution, version, and live per-transport
   telemetry (RFCOMM channels parked, requests served, dials, cached channel, integrity counters), plus a
   plain-English explanation of what each transport actually does.

### What is new in v1.3.0

1. **The taskbar finally shows the ChainIt chain-link.** The web File Manager runs in Edge app-mode, so the
   button Windows drew on the taskbar belonged to the *browser* process and fell back to its generic globe.
   ChainIt now owns a real top-level Win32 window (`ChainItHubWindowClass`) created from the icon compiled
   into the exe as resource #1, and pins one taskbar identity for the process with
   `SetCurrentProcessExplicitAppUserModelID(L"ChainIt.Sibling.Hub")`. Taskbar, Alt-Tab, the tray tooltip and
   every shortcut now show the same emerald + cyan chain-link mark as the Android launcher icon.
2. **A native mini panel in the app list.** That hub window *is* the mini form of the app: link status, the
   active transport, `Z:" + BS + "` state and the integrity counters, with keycap buttons for *Open File Manager*,
   *Open `Z:" + BS + "`*, *Mount / Refresh `Z:" + BS + "`*, *Create Desktop Icon*, *Check for Updates* and *Quit*. It is one
   click from the tray (first menu entry, and tray double-click), starts with the app, and closing it keeps
   ChainIt resident in the tray. `/mini` in the browser is still available.
3. **Desktop icon that is actually created and verified.** `createDesktopShortcut()` no longer trusts COM: it
   writes the `.lnk` through `IShellLinkW`, then *checks that the file exists*, and if Explorer's shell host
   refused, it writes the documented MS-SHLLINK binary layout itself (`writeShellLinkFile`). It probes several
   Desktop locations (known folder, `%USERPROFILE%" + BS + "Desktop`, OneDrive Desktop), also drops a Start Menu
   entry, flushes the shell notification queue, and reports the exact verified path back to the UI.
4. **`Z:" + BS + "` shows your phone's name, never `127.0.0.1:4173`.** After `net use`, the engine writes
   `_LabelFromReg` on the Explorer `MountPoints2` key for the share so "This PC" reads
   `<Phone name> (ChainIt)`. Explorer only creates that key on first browse, so the label is re-applied on a
   short back-off (2 s, then 6 s x 3) and refreshed whenever the sibling reports a new device name.
5. **Full Windows-Explorer behaviour in the file panes.** A real **Modified** column (sortable, with the full
   locale timestamp on hover), list/grid views, *Name / Size / Type / Date modified* sort with a
   folders-first toggle, live filtering, a status bar with item count, selection size and the active sort
   chip, Explorer keyboard shortcuts (**Enter** open, **F2** rename, **Del** delete, **F5** refresh,
   **Backspace**/**Alt+" + chr(0x2191) + "** up, **Ctrl+A/C/X/V**, **Ctrl+Shift+N** new folder, **Ctrl+E** filter,
   **Esc** clear) and a **Download to this PC** entry in the right-click menu.
6. **Malware isolation between the two devices.** Sensitive data must not let one compromised device poison
   the other, so both ends now enforce integrity instead of trusting the link:
   - *Android:* the Pairing Key is **mandatory** on every VFS/WebDAV request (401 without it), and 5 bad keys
     inside 60 s locks the port for 30 s (429) to stop brute-force guessing.
   - *Both directions:* every served byte range publishes `X-ChainIt-File-Sha256` (whole file) and
     `X-ChainIt-Chunk-Sha256` (that range). Inbound `PUT` / `/chainit/upload` bodies are verified against the
     sender's declared digest and **deleted + rejected with `409 Conflict`** on mismatch.
   - *Windows:* each downloaded chunk is hashed before it is written, the finished file is hashed again as a
     whole, and only then does it replace anything on disk. Failures are counted and surfaced
     (`/api/state` -> `security`, plus the Connectivity "Sibling security & transfer integrity" card).
7. **Skeuomorphic UI.** A depth layer over the existing `.ci-*` design system
   (`app/client/index.css`): brushed-metal header, bevelled cards, recessed insets and tool groups, embossed
   buttons that physically depress on `:active`, glass LED status dots with a real glow, engraved pills, a
   heavily floating modal, and paper-grain hover on file rows. Nothing was renamed, so every existing screen
   and feature is preserved.
8. **Fast, reliable transfers: simultaneous chunks.** Downloads split into **2 MiB chunks fetched by 4
   parallel workers** over an atomic offset cursor, written at exact offsets with `OVERLAPPED` I/O into a
   temporary reassembly buffer. Bluetooth RFCOMM stays single-stream on purpose (the radio serializes one
   link, so extra workers would only add contention). Chunking is **transfer-only**: the buffer carries the
   hidden + temporary attributes so it never appears in Explorer or any listing, it is SHA-256 gated before
   the atomic `MoveFileExW` rename, and it is deleted on any failure - a file is either complete and correct
   or it does not exist.
9. **A real Windows installer.** `installer/build-installer.sh` compiles `windows/src/chainit_installer.cpp`
   and appends the app payload with a 52-byte trailer (`installer/pack_installer.py`): magic, payload size,
   SHA-256 and trailer size. The setup program reads the trailer back out of its own image, verifies the
   digest before touching disk, installs per-user into `%LOCALAPPDATA%" + BS + "Programs" + BS + "ChainIt`, creates the
   shortcuts, registers Apps & features with `ChainIt-Windows.exe /uninstall`, and can launch the app.
   `/S` installs silently; `/uninstall` removes everything. No Inno Setup, no NSIS, no WiX.
10. **Version metadata in the binaries.** Both exes now carry `VERSIONINFO` (FileDescription *ChainIt
    Sibling*, version 1.3.0.0), so Task Manager, Alt-Tab and file properties show the product name instead of
    a bare filename.

### What is new in v1.2.1 / v1.2.0

1. **Android launch crash fixed (was a blocker).** Kotlin 2.x emits `invokedynamic` lambdas by default and
   the `dx` dexer cannot lower them for API 26, so the previous build died inside `attachBaseContext` with
   *"ChainIt Sibling keeps stopping"*. `android/build-apk.sh` now compiles with
   `-Xlambdas=class -Xsam-conversions=class`, and `android/tools/verify_dex_safety.py` proves three things on
   every build: all 68 first-party classes are `invokedynamic`-free, the bundled Kotlin runtime's
   `invokedynamic` users are only the ones `dx` legitimately lowers, and no first-party source calls a stdlib
   method that is itself `invokedynamic`-emulated (`File.copyRecursively`, `compareBy`, `Regex.findAll`, ...).
   Those calls were replaced by `com.chainit.android.vfs.FileOps`. `ChainItApp` also installs an
   uncaught-exception handler that writes `chainit-crash.log` and shows it on the next launch instead of
   failing silently.
   A *second* startup crash (`IllegalArgumentException: Unknown color`) came from a pill building its
   background with `Color.parseColor(alphaHex + fgHex)` at runtime. Every color in the app is now a
   compile-time literal (`0xFFRRGGBB.toInt()`) or `Ds.withAlpha()`, and `verify_dex_safety.py` stage 4
   fails the build if any runtime color parsing ever returns.
2. **One identity for both apps.** The emerald + cyan chain-link launcher icon is unchanged on Android and is
   now the brand mark everywhere else too: browser favicon, desktop UI header, and
   `app/client/components/Brand.tsx`.
3. **Redesigned, icon-led interfaces.** The Windows/browser UI runs on a token-based `.ci-*` design system
   (`app/client/index.css`: cards, navigation pills, buttons, inputs, file rows, breadcrumbs, toasts, modals,
   tool groups, sort menus), and the Android app on `ui/DesignSystem.kt` + a rewritten `MainActivity` — one
   clean, organized visual language on both sides. Toolbars are icon-first with tooltips, so long labels
   disappear where an icon says it better.
4. **Explorer-grade sorting.** Both drive panes sort by *Name, Size, Date modified or Type*, ascending or
   descending, with a *folders-first* toggle; the list column headers are clickable sort toggles and the
   choice is remembered in `localStorage`.
5. **One icon everywhere.** `windows/res/chainit.ico` is generated from the exact Android launcher vector by
   `windows/res/make_icon.py` and compiled into the exe (`windows/res/chainit.rc`), so the file icon, the
   running window, the tray icon and desktop shortcuts all show the chain-link mark. The same artwork is the
   browser favicon and the desktop header logo.
6. **Desktop icon creation.** Tray → *Create desktop icon*, or the header *Desktop icon* button, asks the
   engine for `POST /api/app/desktop-shortcut`, which writes a real `ChainIt.lnk` on the Desktop via
   `IShellLinkW` (icon = the embedded chain-link mark) and notifies Explorer to refresh.
7. **Real in-place updates, never a second copy.**
   - *Windows:* tray menu -> **Update ChainIt in place...**, or the header **Update** button, calls
     `POST /api/app/update`. The engine reads `releases/version.txt`, compares the published SHA-256 with the
     hash of the `.exe` currently running, downloads and hash-verifies the new binary into
     `%TEMP%\ChainIt-Update`, then spawns a detached `ChainIt-Updater.cmd` that overwrites the *same* file
     path and relaunches it. Settings live in `%APPDATA%\ChainIt`, so they survive the swap.
   - *Android:* `UpdateManager` downloads the APK, reads its `versionCode` (it never downgrades and never
     re-installs an identical build) and installs through `UpdateFileProvider`, which upgrades the existing
     `com.chainit.android` package in place — same icon slot, same permissions, same Sibling key.
   - After rebuilding either binary, run `python3 scripts/publish_release_manifest.py` so
     `releases/version.txt` keeps matching what is committed.

> **One-time note for phones that already run the v1.1.0 build:** the release signing key is now committed at
> `android/keystore/chainit-release.p12` (the old key's password was unrecoverable). Android refuses to
> upgrade across different signatures, so uninstall the previous build once. Every update after that is a true
> in-place upgrade.

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
| [`releases/`](./releases) | Compiled final deliverables: `ChainIt-Windows.exe`, `ChainIt-Setup-1.5.1.exe` (installer) & `ChainIt-Android.apk`, plus `version.txt` |
| [`installer/`](./installer) | The Windows setup program: `build-installer.sh` compiles `windows/src/chainit_installer.cpp`, `pack_installer.py` appends the payload + 52-byte SHA-256 trailer |
| [`windows/src/chainit_installer.cpp`](./windows/src/chainit_installer.cpp) | Self-contained setup stub: reads its own trailer, verifies SHA-256, installs per-user, writes shortcuts + Apps & features uninstaller, `/S` silent mode |
| [`ARCHITECTURE.md`](./ARCHITECTURE.md) | Complete protocol (`CHNP/1.0`), transport scoring formula, Sibling crypto, and VFS design |
| [`windows/src/chainit_win_main.cpp`](./windows/src/chainit_win_main.cpp) | Native Win32 C++17 single-binary source (**native hub window + AppUserModelID taskbar identity**, System Tray, UDP discovery, **`AF_BTH` RFCOMM client + published SDP receiver + `WSALookupService` paired-device discovery**, multipath WebDAV `Z:\` mounter with phone-name labelling, **parallel 2 MiB chunked transfers with per-chunk SHA-256**, verified desktop-shortcut writer with an MS-SHLLINK fallback, `/uninstall` mode, embedded UI server) |
| [`android/`](./android) | Native Kotlin Android source (`SiblingDaemonService`, **`PhoneHttpAndWebDavServer` with mandatory Pairing Key, brute-force lockout and SHA-256 range/upload verification**, **`BluetoothRfcommTransport`**, `UdpSiblingDiscovery`, `AndroidStorageVfsProvider`, `MainActivity`) |
| [`crates/`](./crates) | Shared Rust core (`chainit-core`) & Windows library (`chainit-windows`) |
| [`app/`](./app) | ChainIt File Manager React/TypeScript UI + Node live preview server |
| [`scripts/`](./scripts) | PowerShell `Z:\` drive helpers (`mount-z-drive.ps1`, `unmount-z-drive.ps1`), asset embedder (`gen_embedded_assets.py`) & release manifest publisher |
| [`windows/build.sh`](./windows/build.sh) | Reproducible cross-compile of the exe with `zig c++` (no Visual Studio), then republishes `releases/version.txt` |
