export type DriveTarget = 'pc' | 'phone';

export type TransportId =
  | 'usb_rndis'
  | 'wifi_lan'
  | 'local_hotspot'
  | 'bluetooth'
  | 'online_p2p';

export type SelectionMode = 'auto' | 'manual';

export interface VfsItem {
  name: string;
  path: string; // POSIX-normalized virtual path starting with "/"
  kind: 'file' | 'directory';
  sizeBytes: number;
  modifiedMs: number;
  mimeType: string;
  ext: string;
}

export interface DriveStats {
  target: DriveTarget;
  driveLetter: string;
  label: string;
  totalCapacityBytes: number;
  usedBytes: number;
  fileCount: number;
  folderCount: number;
}

export interface TransportTelemetry {
  id: TransportId;
  name: string;
  shortName: string;
  category: 'Wired Offline' | 'LAN Offline' | 'Direct Offline' | 'WPAN Offline' | 'Pluggable Online';
  isOffline: boolean;
  available: boolean;
  canBootstrapOnDemand: boolean;
  speedMBps: number;
  rttMs: number;
  packetLossRatio: number;
  signalPct: number;
  powerCostIndex: number;
  score: number;
  isActive: boolean;
  statusNote: string;
}

export interface EnvironmentSimulation {
  usbPlugged: boolean;
  sharedWifiAvailable: boolean;
  localHotspotActive: boolean;
  bluetoothEnabled: boolean;
  onlineP2pEnabled: boolean;
  phoneScreenLocked: boolean;
  phoneBatteryPct: number;
  /**
   * Battery detail the phone reports from v1.7.0 on (BatteryManager + the sticky
   * ACTION_BATTERY_CHANGED broadcast). A percentage alone does not say whether a link survives the
   * night, so the plug and the temperature travel with it. `phoneSeenMs === 0` means no phone has
   * ever reported, and the UI must then say so instead of showing a plausible-looking number.
   */
  phoneCharging?: boolean;
  phoneChargeSource?: 'ac' | 'usb' | 'wireless' | 'none' | 'unknown';
  phoneTemperatureC?: number;
  phoneSeenMs?: number;
  /** False when the last report is older than three minutes - the number is then history, not live. */
  phoneTelemetryLive?: boolean;
}

/**
 * One volume on the PC (v1.7.0).
 *
 * The PC's `/` is its profile folder, not the whole machine: a second partition, a card reader or a
 * mapped share is addressed as `/D:`, `/E:` ... exactly as the Windows File Manager addresses it.
 * `ready: false` means the volume is listed but has no media (or is a remote share the engine
 * deliberately does not probe, because that call can block for seconds), so the sizes stay zero
 * rather than being invented.
 */
export interface PcVolume {
  letter: string;
  /** What a client puts in `path` to reach this volume: `/` for the profile root, `/D:` otherwise. */
  virtualPath: string;
  label: string;
  rootPath: string;
  kind: 'profile' | 'fixed' | 'removable' | 'cdrom' | 'network' | 'ramdisk' | 'unknown';
  isPcRoot: boolean;
  ready: boolean;
  totalBytes: number;
  freeBytes: number;
}

export interface SiblingConfig {
  enabled: boolean;
  connected: boolean;
  manualConnectedWhenNotSibling: boolean;
  autoConnectOnRange: boolean;
  autoMountZDrive: boolean;
  zeroPromptTransfers: boolean;
  allowScreenOffVfs: boolean;
  zDriveMounted: boolean;
  peerDeviceName: string;
  peerModel: string;
  peerAndroidVersion: string;
  ed25519Fingerprint: string;
  currentBleBeaconHex: string;
  beaconSecondsRemaining: number;
}

export interface PendingApproval {
  id: string;
  op: 'copy' | 'move';
  fromTarget: DriveTarget;
  fromPaths: string[];
  toTarget: DriveTarget;
  toDir: string;
  totalBytes: number;
  itemNames: string[];
  createdAtMs: number;
}

export interface TransferRecord {
  id: string;
  label: string;
  op: 'copy' | 'move' | 'upload' | 'webdav' | 'edit' | 'download';
  direction: 'pc_to_phone' | 'phone_to_pc' | 'internal_phone' | 'internal_pc';
  transportUsed: TransportId;
  promotedHotspot: boolean;
  bytes: number;
  durationMs: number;
  effectiveSpeedMBps: number;
  status: 'completed' | 'awaiting_approval';
  timestampMs: number;
  /**
   * Recent-shared-files provenance (v1.7.0): who moved the bytes, and where they are on the PC.
   * `virtualPath` is the address the VFS understands (`/Downloads/x.pdf`, or `/D:/Games/x.zip` on
   * another volume) so a client can fetch the file again or ask the PC to reveal it; `localPath` is
   * the native path, for display on the PC only.
   */
  source?: 'ui' | 'api' | 'phone-upload' | 'phone-download' | 'webdav' | 'preview';
  localPath?: string;
  virtualPath?: string;
}

export interface ProtocolLogEntry {
  id: string;
  timestampMs: number;
  layer: 'SIBLING' | 'TRANSPORT' | 'VFS' | 'WEBDAV' | 'BLUETOOTH';
  summary: string;
  detail: string;
}

/** Live status of the native Bluetooth RFCOMM transport (Windows AF_BTH <-> Android BluetoothServerSocket). */
export interface BluetoothLinkInfo {
  nativeRfcomm: boolean;
  radioPresent: boolean;
  receiverListening: boolean;
  receiverRfcommChannel: number;
  phoneServerUuid: string;
  pcReceiverUuid: string;
  activeLink: string;
  pairedDevices: string[];
  /** Persistent keep-alive RFCOMM channels parked and ready to reuse. */
  pooledChannels?: number;
  maxChannels?: number;
  /** RFCOMM channel number cached from the first SDP query (skips later lookups). */
  cachedRfcommChannel?: number;
  requestsServed?: number;
  redials?: number;
  lastRttMs?: number;
  keepAlive?: boolean;
}

export interface SystemSnapshot {
  selectionMode: SelectionMode;
  manualTransportId: TransportId;
  activeTransportId: TransportId | null;
  transports: TransportTelemetry[];
  environment: EnvironmentSimulation;
  sibling: SiblingConfig;
  drives: {
    pc: DriveStats;
    phone: DriveStats;
  };
  pendingApprovals: PendingApproval[];
  transfers: TransferRecord[];
  logs: ProtocolLogEntry[];
  lanAddresses: string[];
  serverPort: number;
  /** Every volume the PC reports, and whether `/` may name one of them (v1.7.0). */
  pcVolumes?: PcVolume[];
  allowOtherDrives?: boolean;
  bluetooth?: BluetoothLinkInfo;
  security?: SecurityPosture;
  /** Non-empty when the sibling link is up but not actually serving files - shown verbatim. */
  linkProblem?: string;
}

export interface SecurityPosture {
  keyAuth: string;
  lockoutAfter: number;
  lockoutWindowSec: number;
  lockoutCooldownSec: number;
  integrity: string;
  chunkBytes: number;
  parallelWorkers: number;
  chunksVerified: number;
  integrityFailures: number;
}
