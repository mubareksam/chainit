import fs from 'node:fs';
import path from 'node:path';

export const RUNTIME_ROOT = path.resolve(process.cwd(), '.runtime');
export const PC_DRIVE_ROOT = path.join(RUNTIME_ROOT, 'pc-drive');
export const PHONE_DRIVE_ROOT = path.join(RUNTIME_ROOT, 'phone-drive');

function writeIfMissing(baseDir: string, relPath: string, content: string) {
  const full = path.join(baseDir, relPath);
  fs.mkdirSync(path.dirname(full), { recursive: true });
  if (!fs.existsSync(full)) {
    fs.writeFileSync(full, content, 'utf8');
  }
}

function ensureDir(baseDir: string, relPath: string) {
  fs.mkdirSync(path.join(baseDir, relPath), { recursive: true });
}

export function ensureSeededStorage() {
  // 1. Seed Android Phone Storage (Z:\ -> /storage/emulated/0)
  ensureDir(PHONE_DRIVE_ROOT, 'DCIM/Camera');
  ensureDir(PHONE_DRIVE_ROOT, 'Pictures/Screenshots');
  ensureDir(PHONE_DRIVE_ROOT, 'Download');
  ensureDir(PHONE_DRIVE_ROOT, 'Documents');
  ensureDir(PHONE_DRIVE_ROOT, 'Music');
  ensureDir(PHONE_DRIVE_ROOT, 'ChainIt_Inbox');

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'DCIM/Camera/IMG_20261116_Dusk_Horizon.svg',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 500">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#0f172a"/>
      <stop offset="55%" stop-color="#1e3a8a"/>
      <stop offset="85%" stop-color="#f97316"/>
      <stop offset="100%" stop-color="#fde047"/>
    </linearGradient>
  </defs>
  <rect width="800" height="500" fill="url(#sky)"/>
  <circle cx="610" cy="290" r="54" fill="#fef08a" opacity="0.92"/>
  <polygon points="0,390 180,270 340,365 530,245 710,350 800,310 800,500 0,500" fill="#0f172a" opacity="0.85"/>
  <rect x="85" y="330" width="38" height="170" fill="#020617"/>
  <rect x="135" y="305" width="48" height="195" fill="#020617"/>
  <rect x="196" y="350" width="34" height="150" fill="#020617"/>
  <text x="32" y="468" fill="#94a3b8" font-family="sans-serif" font-size="16">Shot on Galaxy S24 Ultra • Accessed directly via ChainIt Z:\\ Sibling VFS</text>
</svg>`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'DCIM/Camera/IMG_20261115_Mountain_Trail.svg',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 500">
  <defs>
    <linearGradient id="dawn" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#064e3b"/>
      <stop offset="50%" stop-color="#0f766e"/>
      <stop offset="100%" stop-color="#38bdf8"/>
    </linearGradient>
  </defs>
  <rect width="800" height="500" fill="url(#dawn)"/>
  <polygon points="0,420 230,180 450,380 640,150 800,340 800,500 0,500" fill="#022c22"/>
  <polygon points="120,500 360,240 590,500" fill="#065f46" opacity="0.85"/>
  <circle cx="170" cy="115" r="36" fill="#e0f2fe" opacity="0.85"/>
  <text x="32" y="468" fill="#a7f3d0" font-family="sans-serif" font-size="16">RAW Preview • Zero-Prompt Sibling Mount (Z:\\DCIM\\Camera)</text>
</svg>`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'Pictures/Screenshots/Screenshot_Sibling_Service.svg',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 420 740">
  <rect width="420" height="740" rx="28" fill="#090d16" stroke="#334155" stroke-width="4"/>
  <rect x="28" y="56" width="364" height="96" rx="16" fill="#0f172a" stroke="#06b6d4" stroke-width="1.5"/>
  <text x="48" y="95" fill="#22d3ee" font-family="sans-serif" font-size="18" font-weight="bold">ChainIt Sibling Active</text>
  <text x="48" y="124" fill="#cbd5e1" font-family="sans-serif" font-size="13">Screen-Off VFS Ready • BLE Beacon Rotating</text>
  <rect x="28" y="176" width="364" height="120" rx="16" fill="#111827"/>
  <text x="48" y="212" fill="#f8fafc" font-family="sans-serif" font-size="15" font-weight="bold">Windows Z:\\ Drive Link</text>
  <text x="48" y="240" fill="#10b981" font-family="monospace" font-size="13">Auto-Connected • 5 GHz Wi-Fi LAN</text>
  <text x="48" y="268" fill="#94a3b8" font-family="sans-serif" font-size="12">Zero confirmation prompts enabled</text>
</svg>`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'Documents/Field_Notes_Offline_Sync.md',
    `# Field Notes — ChainIt Sibling Mode

- **Phone Screen Off Test**: Verified that browsing \`Z:\\Documents\` from Windows Laptop does NOT require unlocking the phone.
- **Offline Routerless Test**: Disabled home Wi-Fi router -> ChainIt automatically triggered 5 GHz Local Hotspot via BLE GATT and resumed at chunk boundary.
- **Edit Directly in Place**: You can edit this markdown file right from the ChainIt File Manager on your PC and hit **Save to Z:\\**!
`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'Download/Travel_Receipt_2026.txt',
    `RECEIPT #CHN-2026-1116
Item: High-Speed Portable SSD & USB-C 3.2 Cable
Status: Paid
Notes: Grabbed directly from Phone Download folder without touching phone!
`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'Download/Hotspot_Profile_Backup.json',
    JSON.stringify(
      {
        profile: 'ChainIt-Direct-5GHz',
        band: '5GHz',
        channel: 149,
        bootstrapVia: 'BLE_GATT_ENCRYPTED',
        siblingOnly: true,
      },
      null,
      2
    )
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'Music/Playlist_Focus_Session.txt',
    `01. Mulatu Astatke - Yekermo Sew (FLAC)
02. Hailu Mergia - Wede Harer Guzo (FLAC)
03. Emahoy Tsegue-Maryam Guebrou - Homesickness (WAV)
`
  );

  writeIfMissing(
    PHONE_DRIVE_ROOT,
    'ChainIt_Inbox/Welcome_Sibling_Mode.md',
    `# Welcome to Z:\\ (Your Android Sibling Drive)

Files dropped here from your Windows PC—either via \`explorer.exe Z:\\\` or the **ChainIt File Manager**—arrive instantaneously with zero popups on your phone.
`
  );

  // 2. Seed Windows Laptop Storage (C:\ -> Local PC Workspace)
  ensureDir(PC_DRIVE_ROOT, 'Desktop');
  ensureDir(PC_DRIVE_ROOT, 'Documents');
  ensureDir(PC_DRIVE_ROOT, 'Downloads');
  ensureDir(PC_DRIVE_ROOT, 'Pictures');
  ensureDir(PC_DRIVE_ROOT, 'Projects');

  writeIfMissing(
    PC_DRIVE_ROOT,
    'Desktop/Laptop_Quick_Drop_Guide.md',
    `# ChainIt Desktop Workspace (C:\\)

Try any of these local-drive actions right now:
1. **Drag & Drop** any file or folder between **C:\\ (Windows PC)** and **Z:\\ (Android Sibling Phone)**.
2. Select files and press **Ctrl+C** (Copy) or **Ctrl+X** (Cut), switch panes, and press **Ctrl+V** (Paste).
3. Double-click any file on **Z:\\** to preview images or edit text/code directly on the phone's storage—even while the phone screen is locked!
4. Switch to **Smart Connectivity** to test unplugging Wi-Fi or plugging in USB and watch Automatic Mode adapt instantaneously.
`
  );

  writeIfMissing(
    PC_DRIVE_ROOT,
    'Desktop/Sprint_Tasks.txt',
    `[x] Pair Windows 11 Laptop <-> Android Phone via Ed25519 QR
[x] Enable Sibling Mode (Auto-connect + Zero-prompt + Z:\\ VFS)
[x] Test Copy/Paste & Cut/Move between C:\\ and Z:\\
[ ] Drag presentation graphic into Phone Download folder
`
  );

  writeIfMissing(
    PC_DRIVE_ROOT,
    'Pictures/Architecture_Diagram_Card.svg',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 760 400">
  <rect width="760" height="400" fill="#090d16"/>
  <rect x="40" y="110" width="210" height="180" rx="18" fill="#0f172a" stroke="#38bdf8" stroke-width="2"/>
  <text x="72" y="165" fill="#f8fafc" font-family="sans-serif" font-size="18" font-weight="bold">Windows 11 PC</text>
  <text x="72" y="198" fill="#38bdf8" font-family="monospace" font-size="14">Z:\\ Explorer Mount</text>
  <text x="72" y="226" fill="#94a3b8" font-family="sans-serif" font-size="13">ChainIt File Manager</text>
  <line x1="250" y1="200" x2="510" y2="200" stroke="#10b981" stroke-width="4" stroke-dasharray="8 6"/>
  <rect x="295" y="162" width="170" height="76" rx="12" fill="#064e3b" stroke="#10b981" stroke-width="1.5"/>
  <text x="316" y="195" fill="#ecfdf5" font-family="sans-serif" font-size="13" font-weight="bold">Auto Smart Link</text>
  <text x="313" y="218" fill="#6ee7b7" font-family="monospace" font-size="11">USB • LAN • Hotspot • BLE</text>
  <rect x="510" y="110" width="210" height="180" rx="18" fill="#0f172a" stroke="#a855f7" stroke-width="2"/>
  <text x="542" y="165" fill="#f8fafc" font-family="sans-serif" font-size="18" font-weight="bold">Android Sibling</text>
  <text x="542" y="198" fill="#c084fc" font-family="monospace" font-size="14">Screen-Off VFS</text>
  <text x="542" y="226" fill="#94a3b8" font-family="sans-serif" font-size="13">Zero-Touch Standby</text>
</svg>`
  );

  writeIfMissing(
    PC_DRIVE_ROOT,
    'Documents/Project_Budget_2026.csv',
    `Category,Item,Quantity,UnitCostUSD,Status
Hardware,USB-C 10Gbps Braided Cable,2,18.50,Ready
Infrastructure,Optional Self-Hosted Relay VPS,1,5.00,Phase2
Storage,NVMe Workspace Cache,1,79.00,Active
`
  );

  writeIfMissing(
    PC_DRIVE_ROOT,
    'Projects/sibling_policy.json',
    JSON.stringify(
      {
        siblingAutoConnect: true,
        mountDriveLetter: 'Z',
        preferredTransports: ['usb_rndis', 'wifi_lan', 'local_hotspot', 'bluetooth'],
        autoPromoteHotspotAboveBytes: 1048576,
      },
      null,
      2
    )
  );
}
