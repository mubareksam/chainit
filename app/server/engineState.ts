import crypto from 'node:crypto';
import os from 'node:os';
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
} from '../shared/types.js';
import { executeCopyOrMove, getDriveStats } from './vfsService.js';

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
  };

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
    };

    this.transfers.unshift(record);
    if (this.transfers.length > 40) this.transfers.length = 40;

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

  getSnapshot(): SystemSnapshot {
    this.recomputeState();
    const transports = this.buildTransportTelemetry().map((t) => ({
      ...t,
      isActive: t.id === this.activeTransportId,
    }));

    return {
      selectionMode: this.selectionMode,
      manualTransportId: this.manualTransportId,
      activeTransportId: this.activeTransportId,
      transports,
      environment: { ...this.environment },
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
      },
    };
  }
}

export const engine = new ChainItEngine();
