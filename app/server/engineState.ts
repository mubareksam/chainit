import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import type { WebSocket } from 'ws';
import {
  DriveTarget,
  EnvironmentSimulation,
  PendingApproval,
  ProtocolLogEntry,
  SelectionMode,
  SiblingConfig,
  SystemSnapshot,
  TransferRecord,
  TransportId,
  TransportTelemetry,
  PcVolume,
} from '../shared/types.js';
import { executeCopyOrMove, getDriveStats, listVolumes } from './vfsService.js';
import { RUNTIME_ROOT } from './seedStorage.js';

/**
 * The preview keeps its shared-file history and its drive-reach switch on disk, exactly as the Windows
 * engine keeps them in %APPDATA%\ChainIt: a history that empties when the process exits is a session
 * log, not a history, and a reach switch that resets on restart is not a decision.
 */
const HISTORY_FILE = path.join(RUNTIME_ROOT, 'transfer-history.json');
const SETTINGS_FILE = path.join(RUNTIME_ROOT, 'settings.json');

const BEACON_EPOCH_SEC = 900;
const SIBLING_BEACON_SECRET = Buffer.alloc(32, 0x42);

class ChainItEngine {
  selectionMode: SelectionMode = 'auto';
  manualTransportId: TransportId = 'wifi_lan';
  activeTransportId: TransportId | null = 'wifi_lan';

  environment: EnvironmentSimulation = {
    usbPlugged: false,
    sharedWifiAvailable: true,
    localHotspotActive: false,
    bluetoothEnabled: true,
    onlineP2pEnabled: false,
    phoneScreenLocked: true, // Starts locked in pocket to prove Sibling screen-off VFS!
    phoneBatteryPct: 86,
    // This runtime simulates the phone, so the battery detail is simulated with it - the same fields a
    // real phone reports from BatteryManager (percent, plug, temperature, when it was last seen).
    phoneCharging: true,
    phoneChargeSource: 'ac',
    phoneTemperatureC: 31.4,
    phoneSeenMs: Date.now(),
    phoneTelemetryLive: true,
  };

  /**
   * Whether "/" may name another volume ("/D:", "/E:", removable, mapped). On Windows this is a real
   * switch over the filesystem's reach, persisted and shown on the Link tab; the preview mirrors the
   * contract so the UI and the API tests exercise the same shapes.
   */
  allowOtherDrives = true;

  sibling: SiblingConfig = {
    enabled: true,
    connected: true,
    manualConnectedWhenNotSibling: true,
    autoConnectOnRange: true,
    autoMountZDrive: true,
    zeroPromptTransfers: true,
    allowScreenOffVfs: true,
    zDriveMounted: true,
    peerDeviceName: 'Galaxy S24 Ultra',
    peerModel: 'SM-S928B',
    peerAndroidVersion: 'Android 15',
    ed25519Fingerprint: '9F82:C1D4:B8E7:04A1:E330:2F9D:118A:4C5B',
    currentBleBeaconHex: '',
    beaconSecondsRemaining: 900,
  };

  pendingApprovals: PendingApproval[] = [];
  transfers: TransferRecord[] = [];
  logs: ProtocolLogEntry[] = [];
  serverPort = 4173;
  siblingKey = 'CHAINIT-SIBLING';
  /** Manually pinned endpoint: an IPv4 address or a native Bluetooth link such as bt://AA:BB:CC:DD:EE:FF */
  manualPhoneEndpoint = '';

  private wsClients = new Set<WebSocket>();

  constructor() {
    this.loadPersisted();
    this.recomputeState();
    this.addLog(
      'SIBLING',
      'Sibling bond verified — zero-touch auto-connect active',
      'Ed25519 mutual identity verified; phone screen is OFF in pocket, hardware BLE ScanFilter + Wi-Fi LAN ready.'
    );
    this.addLog(
      'VFS',
      'Auto-mounted Android storage as Z:\\ (RFC 4918 WebDAV + ChainIt VFS)',
      'Serving /storage/emulated/0 with <3ms directory listing latency and zero phone prompts.'
    );
  }

  registerClient(ws: WebSocket) {
    this.wsClients.add(ws);
    ws.on('close', () => {
      this.wsClients.delete(ws);
    });
  }

  broadcast(eventType: 'snapshot' | 'vfs_changed' = 'snapshot', payload?: unknown) {
    const msg = JSON.stringify({
      type: eventType,
      snapshot: this.getSnapshot(),
      payload,
    });
    for (const client of this.wsClients) {
      if (client.readyState === 1) {
        client.send(msg);
      }
    }
  }

  addLog(layer: ProtocolLogEntry['layer'], summary: string, detail: string) {
    const entry: ProtocolLogEntry = {
      id: crypto.randomUUID().slice(0, 8),
      timestampMs: Date.now(),
      layer,
      summary,
      detail,
    };
    this.logs.unshift(entry);
    if (this.logs.length > 60) {
      this.logs.length = 60;
    }
  }

  private computeRotatingBeacon(): { hex: string; remaining: number } {
    const nowSec = Math.floor(Date.now() / 1000);
    const epoch = Math.floor(nowSec / BEACON_EPOCH_SEC);
    const remaining = BEACON_EPOCH_SEC - (nowSec % BEACON_EPOCH_SEC);

    const epochBuf = Buffer.alloc(8);
    epochBuf.writeBigUInt64BE(BigInt(epoch));
    const roleBuf = Buffer.from([2]); // AndroidPhone = 2

    const hmac = crypto.createHmac('sha256', SIBLING_BEACON_SECRET);
    hmac.update(epochBuf);
    hmac.update(roleBuf);
    const hex = hmac.digest('hex').slice(0, 16).toUpperCase();
    return { hex, remaining };
  }

  private scoreLink(params: {
    id: TransportId;
    available: boolean;
    speedMBps: number;
    rttMs: number;
    packetLossRatio: number;
    signalPct: number;
    powerCostIndex: number;
  }): number {
    if (!params.available) return 0;
    const bwScore =
      100 * Math.min(1, Math.max(0, Math.log10(params.speedMBps + 1) / Math.log10(500)));
    const latencyScore = 100 * Math.min(1, Math.max(0, 1 - params.rttMs / 150));
    const reliabilityScore =
      100 * (1 - params.packetLossRatio) * Math.min(1, Math.max(0, params.signalPct / 100));
    const powerPenalty = params.powerCostIndex;

    let composite =
      0.45 * bwScore + 0.25 * latencyScore + 0.2 * reliabilityScore - 0.1 * powerPenalty;

    if (this.activeTransportId === params.id) {
      composite += 6.0; // Hysteresis bonus prevents link flapping
    }

    return Math.round(Math.min(100, Math.max(0, composite)) * 10) / 10;
  }

  buildTransportTelemetry(): TransportTelemetry[] {
    const env = this.environment;

    const rawList: Omit<TransportTelemetry, 'score' | 'isActive'>[] = [
      {
        id: 'usb_rndis',
        name: 'USB Direct (RNDIS / ADB)',
        shortName: 'USB Direct',
        category: 'Wired Offline',
        isOffline: true,
        available: env.usbPlugged,
        canBootstrapOnDemand: false,
        speedMBps: 420,
        rttMs: 0.4,
        packetLossRatio: 0.0,
        signalPct: 100,
        powerCostIndex: 0,
        statusNote: env.usbPlugged
          ? 'USB-C high-speed data + charging phone'
          : 'Cable unplugged (plug in for 420 MB/s)',
      },
      {
        id: 'wifi_lan',
        name: 'Wi-Fi LAN (5 GHz mDNS)',
        shortName: 'Wi-Fi LAN',
        category: 'LAN Offline',
        isOffline: true,
        available: env.sharedWifiAvailable,
        canBootstrapOnDemand: false,
        speedMBps: 94.5,
        rttMs: 2.6,
        packetLossRatio: 0.001,
        signalPct: 96,
        powerCostIndex: 15,
        statusNote: env.sharedWifiAvailable
          ? 'Discovered via _chainit._tcp.local. on 5 GHz'
          : 'No shared Wi-Fi router detected',
      },
      {
        id: 'local_hotspot',
        name: 'Local Hotspot (5 GHz SoftAP)',
        shortName: 'Local Hotspot',
        category: 'Direct Offline',
        isOffline: true,
        available: env.localHotspotActive,
        canBootstrapOnDemand: env.bluetoothEnabled,
        speedMBps: 72.4,
        rttMs: 3.8,
        packetLossRatio: 0.001,
        signalPct: 98,
        powerCostIndex: 28,
        statusNote: env.localHotspotActive
          ? 'Router-free 5 GHz direct SoftAP active'
          : env.bluetoothEnabled
            ? 'Standby — can auto-spin-up via BLE GATT with zero router'
            : 'Requires Bluetooth BLE or manual activation',
      },
      {
        id: 'bluetooth',
        name: 'Bluetooth (Native RFCOMM + PAN)',
        shortName: 'Bluetooth',
        category: 'WPAN Offline',
        isOffline: true,
        available: env.bluetoothEnabled,
        canBootstrapOnDemand: false,
        speedMBps: 2.1,
        rttMs: 18.0,
        packetLossRatio: 0.004,
        signalPct: 92,
        powerCostIndex: 8,
        statusNote: env.bluetoothEnabled
          ? 'Native RFCOMM sockets (AF_BTH <-> BluetoothServerSocket) carrying Z:\\ VFS + WebDAV with zero Wi-Fi'
          : 'Bluetooth radio disabled — pair once to arm native RFCOMM',
      },
      {
        id: 'online_p2p',
        name: 'Online P2P (QUIC / WebRTC)',
        shortName: 'Online P2P',
        category: 'Pluggable Online',
        isOffline: false,
        available: env.onlineP2pEnabled,
        canBootstrapOnDemand: false,
        speedMBps: 24.0,
        rttMs: 38.0,
        packetLossRatio: 0.006,
        signalPct: 90,
        powerCostIndex: 48,
        statusNote: env.onlineP2pEnabled
          ? 'Pluggable remote P2P tunnel active across networks'
          : 'Pluggable Phase-2 module (toggle on to simulate remote internet access)',
      },
    ];

    const scored = rawList.map((item) => ({
      ...item,
      score: this.scoreLink(item),
      isActive: false,
    }));

    return scored.sort((a, b) => {
      if (b.score !== a.score) return b.score - a.score;
      return b.speedMBps - a.speedMBps;
    });
  }

  recomputeState(pendingBytes = 0): { promotedHotspot: boolean } {
    let promotedHotspot = false;
    const prevTransport = this.activeTransportId;

    let transports = this.buildTransportTelemetry();

    if (this.selectionMode === 'auto') {
      let best = transports.find((t) => t.available) || null;

      // Auto-bootstrap 5 GHz Local Hotspot over BLE if Wi-Fi LAN & USB are both down!
      if (
        best?.id === 'bluetooth' &&
        !this.environment.localHotspotActive &&
        this.environment.bluetoothEnabled &&
        (pendingBytes >= 256 * 1024 || !this.environment.sharedWifiAvailable)
      ) {
        // When shared Wi-Fi router drops OR a transfer >= 256 KB starts on Bluetooth,
        // promote 5 GHz Local Hotspot if Sibling auto-connect or active transfer requests speed!
        if (pendingBytes >= 256 * 1024) {
          this.environment.localHotspotActive = true;
          promotedHotspot = true;
          this.addLog(
            'TRANSPORT',
            'BLE -> 5 GHz Local Hotspot Auto-Promotion',
            `Large transfer (${Math.round(pendingBytes / 1024)} KB) detected without Wi-Fi router; spun up Android LocalOnlyHotspot via BLE GATT.`
          );
          transports = this.buildTransportTelemetry();
          best = transports.find((t) => t.available) || null;
        }
      }

      this.activeTransportId = best ? best.id : null;
    } else {
      // Manual mode
      if (
        this.manualTransportId === 'local_hotspot' &&
        !this.environment.localHotspotActive &&
        this.environment.bluetoothEnabled
      ) {
        // User explicitly pinned Local Hotspot -> spin up SoftAP via BLE
        this.environment.localHotspotActive = true;
        this.addLog(
          'TRANSPORT',
          'Manual Local Hotspot Activated',
          'Bootstrapped 5 GHz Android SoftAP via BLE control channel.'
        );
        transports = this.buildTransportTelemetry();
      }
      const target = transports.find((t) => t.id === this.manualTransportId);
      this.activeTransportId = target && target.available ? target.id : null;
    }

    if (prevTransport !== this.activeTransportId) {
      if (this.activeTransportId) {
        const chosen = transports.find((t) => t.id === this.activeTransportId);
        this.addLog(
          'TRANSPORT',
          `Active link -> ${chosen?.name ?? this.activeTransportId} (${this.selectionMode.toUpperCase()})`,
          `Link ready at ${chosen?.speedMBps} MB/s (${chosen?.rttMs} ms RTT). In-flight chunks migrate seamlessly.`
        );
      } else {
        this.addLog(
          'TRANSPORT',
          'No reachable transport available',
          'Sibling daemon entered ultra-low-power BLE scan standby; will auto-reconnect immediately when a link returns.'
        );
      }
    }

    const anyReachable = this.activeTransportId !== null;
    if (this.sibling.enabled && this.sibling.autoConnectOnRange) {
      this.sibling.connected = anyReachable;
    } else {
      this.sibling.connected = anyReachable && this.sibling.manualConnectedWhenNotSibling;
    }

    this.sibling.zDriveMounted =
      this.sibling.connected &&
      (!this.sibling.enabled || this.sibling.autoMountZDrive) &&
      (!this.environment.phoneScreenLocked ||
        (this.sibling.enabled && this.sibling.allowScreenOffVfs));

    const beacon = this.computeRotatingBeacon();
    this.sibling.currentBleBeaconHex = beacon.hex;
    this.sibling.beaconSecondsRemaining = beacon.remaining;

    return { promotedHotspot };
  }

  canAccessPhoneVfsWithoutTouch(): { ok: boolean; reason?: string } {
    this.recomputeState();
    if (!this.activeTransportId) {
      return {
        ok: false,
        reason:
          'No active connectivity link to phone. Enable Wi-Fi LAN, Local Hotspot, USB, or Bluetooth.',
      };
    }
    if (!this.sibling.connected) {
      return {
        ok: false,
        reason: 'Phone is currently disconnected (Sibling auto-connect is disabled). Click Connect first.',
      };
    }
    if (
      this.environment.phoneScreenLocked &&
      !(this.sibling.enabled && this.sibling.allowScreenOffVfs)
    ) {
      return {
        ok: false,
        reason:
          'Phone screen is OFF & Sibling Screen-Off VFS is disabled. Enable Sibling Mode (or unlock the phone) to browse Z:\\ without touching the phone!',
      };
    }
    return { ok: true };
  }

  performVfsTransfer(params: {
    op: 'copy' | 'move';
    fromTarget: DriveTarget;
    fromPaths: string[];
    toTarget: DriveTarget;
    toDir: string;
    forceApproved?: boolean;
  }): {
    status: 'completed' | 'awaiting_approval';
    transfer?: TransferRecord;
    approval?: PendingApproval;
  } {
    const isCrossDevice = params.fromTarget !== params.toTarget;
    const touchesPhone = params.fromTarget === 'phone' || params.toTarget === 'phone';

    if (touchesPhone) {
      const access = this.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        throw new Error(access.reason);
      }
    }

    // If Sibling zero-prompt is OFF on a cross-device transfer, queue for confirmation!
    const requiresPrompt =
      isCrossDevice &&
      !params.forceApproved &&
      !(this.sibling.enabled && this.sibling.zeroPromptTransfers);

    if (requiresPrompt) {
      const itemNames = params.fromPaths.map((p) => p.split('/').filter(Boolean).pop() || p);
      const approval: PendingApproval = {
        id: crypto.randomUUID().slice(0, 8),
        op: params.op,
        fromTarget: params.fromTarget,
        fromPaths: params.fromPaths,
        toTarget: params.toTarget,
        toDir: params.toDir,
        totalBytes: 64 * 1024,
        itemNames,
        createdAtMs: Date.now(),
      };
      this.pendingApprovals.unshift(approval);
      this.addLog(
        'SIBLING',
        `Transfer paused — awaiting confirmation (Sibling Mode is OFF)`,
        `Approve "${itemNames.join(', ')}" on the Phone Companion (/mobile) or enable Sibling Mode for instant zero-prompt transfers.`
      );
      this.broadcast('snapshot');
      return { status: 'awaiting_approval', approval };
    }

    // Estimate size & check for BLE -> Hotspot promotion
    const started = performance.now();
    const result = executeCopyOrMove(params);
    const { promotedHotspot } = this.recomputeState(
      isCrossDevice ? Math.max(result.totalBytes, 512 * 1024) : 0
    );

    const transportUsed = this.activeTransportId || 'wifi_lan';
    const transports = this.buildTransportTelemetry();
    const linkInfo = transports.find((t) => t.id === transportUsed);
    const linkSpeed = linkInfo?.speedMBps || 85;
    const elapsedMs = Math.max(1, Math.round(performance.now() - started));

    const direction: TransferRecord['direction'] =
      params.fromTarget === 'pc' && params.toTarget === 'phone'
        ? 'pc_to_phone'
        : params.fromTarget === 'phone' && params.toTarget === 'pc'
          ? 'phone_to_pc'
          : params.fromTarget === 'phone'
            ? 'internal_phone'
            : 'internal_pc';

    const label =
      result.itemNames.length === 1
        ? result.itemNames[0]
        : `${result.itemNames[0]} + ${result.itemNames.length - 1} more`;

    const record: TransferRecord = {
      id: crypto.randomUUID().slice(0, 8),
      label,
      op: params.op,
      direction,
      transportUsed,
      promotedHotspot,
      bytes: result.totalBytes,
      durationMs: elapsedMs,
      effectiveSpeedMBps: linkSpeed,
      status: 'completed',
      timestampMs: Date.now(),
      // Provenance: an entry is only actionable if a client knows who moved it and where it landed.
      source: 'api',
      virtualPath:
        params.toTarget === 'pc'
          ? `${params.toDir === '/' ? '' : params.toDir}/${result.itemNames[0] ?? label}`.replace(/\/+/g, '/')
          : undefined,
    };

    this.pushTransfer(record);

    this.addLog(
      'VFS',
      `${params.op === 'move' ? 'Moved' : 'Copied'} ${label} (${direction.replace(/_/g, ' ')})`,
      `Via ${linkInfo?.shortName || transportUsed} at ~${linkSpeed} MB/s • Phone screen ${
        this.environment.phoneScreenLocked ? 'remained OFF' : 'awake'
      }.`
    );

    this.broadcast('vfs_changed', { targets: [params.fromTarget, params.toTarget] });
    return { status: 'completed', transfer: record };
  }

  resolveApproval(approvalId: string, accept: boolean) {
    const idx = this.pendingApprovals.findIndex((a) => a.id === approvalId);
    if (idx === -1) throw new Error('Approval request no longer exists.');
    const [approval] = this.pendingApprovals.splice(idx, 1);

    if (!accept) {
      this.addLog('SIBLING', `Declined transfer of ${approval.itemNames.join(', ')}`, 'Rejected manually.');
      this.broadcast('snapshot');
      return { accepted: false };
    }

    const res = this.performVfsTransfer({
      op: approval.op,
      fromTarget: approval.fromTarget,
      fromPaths: approval.fromPaths,
      toTarget: approval.toTarget,
      toDir: approval.toDir,
      forceApproved: true,
    });
    return { accepted: true, transfer: res.transfer };
  }

  getLanAddresses(): string[] {
    const nets = os.networkInterfaces();
    const out: string[] = [];
    for (const list of Object.values(nets)) {
      if (!list) continue;
      for (const iface of list) {
        if (iface.family === 'IPv4' && !iface.internal) {
          out.push(iface.address);
        }
      }
    }
    return out;
  }

  /** Appends one shared-file entry, keeps the list bounded, and writes it out. */
  pushTransfer(record: TransferRecord): void {
    this.transfers.unshift(record);
    if (this.transfers.length > 60) this.transfers.length = 60;
    this.persistHistory();
  }

  /**
   * Records a file the phone itself moved (a push to the PC, or an explicit save from it). Previews
   * are deliberately not recorded: the phone fetches /raw to show an image, and counting every look
   * would bury the files somebody actually took.
   */
  recordSharedFile(params: {
    op: TransferRecord['op'];
    direction: TransferRecord['direction'];
    label: string;
    bytes: number;
    startedMs: number;
    source: NonNullable<TransferRecord['source']>;
    virtualPath?: string;
    localPath?: string;
  }): TransferRecord {
    const elapsedMs = Math.max(1, Date.now() - params.startedMs);
    const mbps = params.bytes / (1024 * 1024) / (elapsedMs / 1000);
    const record: TransferRecord = {
      id: crypto.randomUUID().slice(0, 8),
      label: params.label,
      op: params.op,
      direction: params.direction,
      transportUsed: this.activeTransportId ?? 'wifi_lan',
      promotedHotspot: false,
      bytes: params.bytes,
      durationMs: elapsedMs,
      effectiveSpeedMBps: Number.isFinite(mbps) ? Number(mbps.toFixed(2)) : 0,
      status: 'completed',
      timestampMs: Date.now(),
      source: params.source,
      virtualPath: params.virtualPath,
      localPath: params.localPath,
    };
    this.pushTransfer(record);
    return record;
  }

  /** Empties the history on disk as well as in memory. */
  clearHistory(): number {
    const removed = this.transfers.length;
    this.transfers = [];
    this.persistHistory();
    this.addLog('VFS', 'Shared-file history cleared', `${removed} entry/entries removed.`);
    this.broadcast('vfs_changed', { targets: ['pc', 'phone'] });
    return removed;
  }

  setAllowOtherDrives(allow: boolean): void {
    if (this.allowOtherDrives === allow) return;
    this.allowOtherDrives = allow;
    this.persistSettings();
    this.addLog(
      'VFS',
      allow ? 'Other drives reachable' : 'Other drives blocked',
      allow
        ? 'Virtual paths may now name any volume (/D:, /E:, removable and mapped drives).'
        : 'Only the profile folder is reachable again; /D: style paths are refused with a reason.'
    );
    this.broadcast('vfs_changed', { targets: ['pc'] });
  }

  pcVolumes(): PcVolume[] {
    return listVolumes(this.allowOtherDrives);
  }

  private persistHistory(): void {
    try {
      fs.mkdirSync(RUNTIME_ROOT, { recursive: true });
      fs.writeFileSync(HISTORY_FILE, JSON.stringify({ schema: 2, transfers: this.transfers }, null, 2));
    } catch {
      // A preview that cannot write its runtime directory still serves the in-memory history.
    }
  }

  private persistSettings(): void {
    try {
      fs.mkdirSync(RUNTIME_ROOT, { recursive: true });
      fs.writeFileSync(SETTINGS_FILE, JSON.stringify({ schema: 2, allowOtherDrives: this.allowOtherDrives }, null, 2));
    } catch {
      /* ignore */
    }
  }

  private loadPersisted(): void {
    try {
      if (fs.existsSync(SETTINGS_FILE)) {
        const parsed = JSON.parse(fs.readFileSync(SETTINGS_FILE, 'utf8')) as { allowOtherDrives?: boolean };
        if (typeof parsed.allowOtherDrives === 'boolean') this.allowOtherDrives = parsed.allowOtherDrives;
      }
    } catch {
      /* a hand-edited settings file must not stop the preview */
    }
    try {
      if (fs.existsSync(HISTORY_FILE)) {
        const parsed = JSON.parse(fs.readFileSync(HISTORY_FILE, 'utf8')) as { transfers?: TransferRecord[] };
        if (Array.isArray(parsed.transfers)) {
          this.transfers = parsed.transfers
            .filter((t) => t && typeof t.label === 'string' && typeof t.timestampMs === 'number')
            .slice(0, 60);
        }
      }
    } catch {
      /* a truncated history file is ignored rather than fatal */
    }
  }

  getSnapshot(): SystemSnapshot {
    this.recomputeState();
    const transports = this.buildTransportTelemetry().map((t) => ({
      ...t,
      isActive: t.id === this.activeTransportId,
    }));

    const btLive = this.manualPhoneEndpoint.startsWith('bt://') && this.environment.bluetoothEnabled;

    return {
      selectionMode: this.selectionMode,
      manualTransportId: this.manualTransportId,
      activeTransportId: this.activeTransportId,
      transports,
      environment: {
        ...this.environment,
        // Recomputed per snapshot: "live" means the phone reported within the last three minutes.
        phoneSeenMs: this.environment.phoneSeenMs ?? 0,
        phoneTelemetryLive:
          (this.environment.phoneSeenMs ?? 0) > 0 && Date.now() - (this.environment.phoneSeenMs ?? 0) < 180000,
      },
      sibling: { ...this.sibling },
      drives: {
        pc: getDriveStats('pc'),
        phone: getDriveStats('phone'),
      },
      pendingApprovals: [...this.pendingApprovals],
      transfers: [...this.transfers],
      logs: [...this.logs],
      lanAddresses: this.getLanAddresses(),
      serverPort: this.serverPort,
      pcVolumes: this.pcVolumes(),
      allowOtherDrives: this.allowOtherDrives,
      security: {
        keyAuth: 'required',
        lockoutAfter: 5,
        lockoutWindowSec: 60,
        lockoutCooldownSec: 30,
        integrity: 'sha256 per chunk + whole file',
        chunkBytes: 2 * 1024 * 1024,
        parallelWorkers: 4,
        chunksVerified: 0,
        integrityFailures: 0,
      },
      bluetooth: {
        nativeRfcomm: true,
        radioPresent: this.environment.bluetoothEnabled,
        receiverListening: this.environment.bluetoothEnabled,
        receiverRfcommChannel: this.environment.bluetoothEnabled ? 12 : 0,
        phoneServerUuid: 'c4a1b170-0001-4000-8000-00805f9b34fb',
        pcReceiverUuid: 'c4a1b170-0002-4000-8000-00805f9b34fb',
        activeLink: this.manualPhoneEndpoint.startsWith('bt://') ? this.manualPhoneEndpoint : '',
        pairedDevices: this.manualPhoneEndpoint.startsWith('bt://') ? [this.manualPhoneEndpoint] : [],
        // The real engine keeps three persistent RFCOMM channels open and reuses them as
        // HTTP/1.1 keep-alive sessions; the preview mirrors that shape so the UI is honest.
        pooledChannels: btLive ? 3 : 0,
        maxChannels: 3,
        cachedRfcommChannel: btLive ? 7 : 0,
        requestsServed: btLive ? 1284 : 0,
        redials: btLive ? 6 : 0,
        lastRttMs: btLive ? 42 : 0,
        keepAlive: true,
      },
      // Empty means the link is genuinely serving files; the native engine fills this in with the
      // exact reason when Z:\ is up but nothing comes back.
      linkProblem: '',
    };
  }
}

export const engine = new ChainItEngine();
