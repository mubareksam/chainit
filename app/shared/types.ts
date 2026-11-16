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
  op: 'copy' | 'move' | 'upload' | 'webdav' | 'edit';
  direction: 'pc_to_phone' | 'phone_to_pc' | 'internal_phone' | 'internal_pc';
  transportUsed: TransportId;
  promotedHotspot: boolean;
  bytes: number;
  durationMs: number;
  effectiveSpeedMBps: number;
  status: 'completed' | 'awaiting_approval';
  timestampMs: number;
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
  bluetooth?: BluetoothLinkInfo;
}
