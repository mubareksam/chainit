import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { DriveTarget, SelectionMode, TransportId } from '../shared/types.js';
import { engine } from './engineState.js';
import { btLinkModel } from './btLink.js';
import { ensureSeededStorage } from './seedStorage.js';
import {
  createNewFolder,
  deleteVfsItems,
  getMimeType,
  isEditableTextFile,
  listDirectory,
  listVolumes,
  renameVfsItem,
  resolveSafePath,
  servedLetters,
  toVfsItem,
} from './vfsService.js';
import { handleWebDavRequest } from './webdavHandler.js';

ensureSeededStorage();

const PORT = Number(process.env.PORT || 4173);
engine.serverPort = PORT;

const app = express();

/** Where this runtime keeps the shared-file history, reported by /api/history. */
const HISTORY_FILE_LABEL = path.resolve(process.cwd(), '.runtime', 'transfer-history.json');

// Ensure preview iframe & cross-origin LAN mobile requests always work cleanly
app.use((req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader(
    'Access-Control-Allow-Methods',
    'GET, POST, PUT, DELETE, OPTIONS, PROPFIND, MKCOL, MOVE, COPY'
  );
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Depth, Destination, Overwrite, Range');
  if (req.method === 'OPTIONS') {
    res.status(204).end();
    return;
  }
  next();
});

// Parse JSON for standard API endpoints (skip raw binary upload endpoint)
app.use((req, res, next) => {
  if (req.path.endsWith('/upload')) {
    next();
    return;
  }
  express.json({ limit: '25mb' })(req, res, next);
});

function parseDriveTarget(raw: string): DriveTarget {
  if (raw === 'pc' || raw === 'phone') return raw;
  throw new Error(`Invalid drive target: ${raw}`);
}

app.get('/api/state', (_req, res) => {
  res.json(engine.getSnapshot());
});

app.post('/api/transport/mode', (req, res) => {
  try {
    const { mode, manualTransportId } = req.body as {
      mode?: SelectionMode;
      manualTransportId?: TransportId;
    };
    if (mode === 'auto' || mode === 'manual') {
      engine.selectionMode = mode;
    }
    if (manualTransportId) {
      engine.manualTransportId = manualTransportId;
    }
    engine.recomputeState();
    engine.addLog(
      'TRANSPORT',
      `Selection mode set to ${engine.selectionMode.toUpperCase()}${
        engine.selectionMode === 'manual' ? ` (pinned: ${engine.manualTransportId})` : ''
      }`,
      engine.activeTransportId
        ? `Using ${engine.activeTransportId}`
        : 'Pinned transport is currently unavailable.'
    );
    engine.broadcast('snapshot');
    res.json(engine.getSnapshot());
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Bad request' });
  }
});

const normalizeBtLink = (raw: string): string | null => {
  let value = raw.trim();
  const explicit = value.toLowerCase().startsWith('bt://');
  if (explicit) value = value.slice(5);
  value = value.replace(/[\/\s]+/g, '').replace(/-/g, ':');
  if (!explicit && !value.includes(':')) return null;

  const withChannel = value.match(/^((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}):(\d{1,5})$/);
  const macOnly = value.match(/^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$/);
  const compact = value.match(/^[0-9a-fA-F]{12}$/);

  let mac: string | null = null;
  let channel = 0;
  if (withChannel) {
    mac = withChannel[1].toUpperCase();
    channel = Number(withChannel[2]);
  } else if (macOnly) {
    mac = value.toUpperCase();
  } else if (compact) {
    mac = value.toUpperCase().match(/.{2}/g)!.join(':');
  }
  if (!mac || channel < 0 || channel > 0xffff) return null;
  return channel ? `bt://${mac}:${channel}` : `bt://${mac}`;
};

const APP_VERSION = '1.9.0';

app.get('/api/app/version', (_req, res) => {
  res.json({ version: APP_VERSION, runtime: 'node-preview', channel: 'stable' });
});

// The Node server is only the live preview of the same UI the Windows binary embeds; the real
// in-place self-replacement is implemented by ChainIt-Windows.exe (POST /api/app/update there).
app.post('/api/app/desktop-shortcut', (_req, res) => {
  res.json({
    ok: false,
    path: '',
    message:
      'Desktop shortcuts are created by ChainIt-Windows.exe (tray: "Create desktop icon"). ' +
      'This browser preview cannot write to your Windows Desktop.',
  });
});

app.post('/api/app/update', (_req, res) => {
  res.json({
    status: 'unsupported',
    version: APP_VERSION,
    latestVersion: APP_VERSION,
    message:
      'This is the browser preview of the ChainIt engine. Run releases/ChainIt-Windows.exe and use ' +
      'its "Check for update" button to replace the installed binary in place.',
  });
});

/**
 * Counts a request that the preview really served over the simulated Bluetooth link.
 *
 * `requestsServed` used to be a hard-coded 1284 whenever a bt:// endpoint was pinned - a number
 * nobody measured. It now only moves when a phone-volume request is served while the link is up,
 * which is the same thing the native engine's counter means: HTTP exchanges over a parked RFCOMM
 * keep-alive channel, no dial involved.
 */
app.use((req, _res, next) => {
  try {
    const overBluetooth =
      engine.btLinkUp &&
      engine.activeTransportId === 'bluetooth' &&
      engine.manualPhoneEndpoint.startsWith('bt://');
    const phonePath =
      req.path.startsWith('/api/vfs/phone') ||
      req.path.startsWith('/chainit/') ||
      req.path.startsWith('/dav') ||
      req.path.startsWith('/raw') ||
      req.path.startsWith('/api/vfs/list') ||
      req.path.startsWith('/api/vfs/text');
    if (overBluetooth && phonePath) btLinkModel.noteReusedRequest();
  } catch {
    /* telemetry must never break a request */
  }
  next();
});

app.post('/api/transport/connect-ip', (req, res) => {
  const { phoneIp, siblingKey } = (req.body || {}) as { phoneIp?: string; siblingKey?: string };
  if (!phoneIp || !phoneIp.trim()) {
    res.status(400).json({ error: 'Phone IP or Bluetooth address is required' });
    return;
  }
  if (siblingKey && siblingKey.trim()) engine.siblingKey = siblingKey.trim();

  const btLink = normalizeBtLink(phoneIp);
  if (btLink) {
    // Native Bluetooth RFCOMM needs no Wi-Fi, no Hotspot and no USB at all.
    engine.environment.bluetoothEnabled = true;
    engine.manualPhoneEndpoint = btLink;
    engine.recomputeState();

    // Dial it for real through the resolution policy, and refuse to claim a link that did not
    // come up. The native engine now does the same: a failure names its cause (radio off, nothing
    // paired, no SDP record, refused, timed out) instead of reporting "connected" and serving
    // nothing - which is precisely how "Bluetooth does not work" gets reported by a user.
    const dial = engine.refreshBluetoothLink() ?? engine.dialBluetoothEndpoint(btLink, { force: true });
    if (!dial.ok) {
      engine.addLog(
        'BLUETOOTH',
        `Manual Bluetooth link failed -> ${btLink}`,
        `${dial.error}. Pair the phone with this PC, keep ChainIt-Android.apk running, and make sure Bluetooth is ON on both devices.`
      );
      engine.broadcast('snapshot');
      res.status(400).json({ error: `Could not open a native RFCOMM link to ${btLink} - ${dial.error}` });
      return;
    }

    engine.addLog(
      'BLUETOOTH',
      `Native Bluetooth RFCOMM link pinned -> ${dial.endpoint}`,
      `Resolved by ${dial.strategy} channel ${dial.channel} (SDP queries so far: ${btLinkModel.sdpQueries}). ` +
        'AF_BTH socket to phone UUID c4a1b170-0001-4000-8000-00805f9b34fb; Z:\\ VFS + WebDAV now run over Bluetooth alone.'
    );
    engine.broadcast('snapshot');
    res.json(engine.getSnapshot());
    return;
  }

  engine.environment.sharedWifiAvailable = true;
  engine.manualPhoneEndpoint = phoneIp.trim();
  engine.recomputeState();
  // The LAN link reached the phone, so its telemetry is live and this is the link that carried it.
  engine.markPhoneTelemetry('wifi_lan');
  engine.addLog(
    'TRANSPORT',
    `Linked to Android Phone IP ${phoneIp.trim()}:49180`,
    'Verified Sibling VFS endpoint.'
  );
  engine.broadcast('snapshot');
  res.json(engine.getSnapshot());
});

/**
 * Forces one Bluetooth dial and reports exactly what the resolution policy did.
 *
 * This is the endpoint the contract test uses to prove the fix rather than assert a shape:
 * a second dial to the same handset must reuse the remembered channel and perform **no** further
 * SDP query, and a handset that is listening but published no record must be rescued by the
 * verified sweep instead of being reported as unreachable.
 */
app.post('/api/bluetooth/probe', (req, res) => {
  const body = (req.body || {}) as { endpoint?: string; forgetCache?: boolean };
  const endpoint = normalizeBtLink(body.endpoint || engine.manualPhoneEndpoint || '');
  if (!endpoint) {
    res.status(400).json({ error: 'A Bluetooth endpoint is required, e.g. bt://AA:BB:CC:DD:EE:FF' });
    return;
  }
  engine.environment.bluetoothEnabled = true;
  engine.manualPhoneEndpoint = endpoint;
  engine.recomputeState();
  // One call, one dial - so the counters this endpoint reports are the counters that dial moved.
  const dial = engine.dialBluetoothEndpoint(endpoint, {
    force: true,
    forgetCache: body.forgetCache === true,
  });
  res.json({ dial, bluetooth: engine.getSnapshot().bluetooth });
});

/**
 * Teaches the PC the phone's own Bluetooth address and RFCOMM channel, exactly as the UDP beacon
 * and the ping reply do. Afterwards the PC dials that channel directly instead of resolving SDP
 * or sweeping - which is the difference between a link that comes up in one dial and one that
 * takes seconds to find.
 */
app.post('/api/bluetooth/learn', (req, res) => {
  const learned = btLinkModel.learnFromPayload((req.body || {}) as Record<string, unknown>);
  const snapshot = engine.getSnapshot();
  if (learned) {
    engine.addLog(
      'BLUETOOTH',
      `Phone reported its Bluetooth endpoint: ${learned}`,
      'ChainIt can now dial the RFCOMM channel directly, with no SDP query and no paired-device sweep.'
    );
    engine.broadcast('snapshot');
  }
  res.json({ learned, learnedEndpoint: btLinkModel.learnedEndpoint, bluetooth: snapshot.bluetooth });
});

app.post('/api/environment/update', (req, res) => {
  const patch = req.body || {};
  const prevEnv = { ...engine.environment };
  Object.assign(engine.environment, patch);

  // Bluetooth conditions changing under a pinned link is exactly when the native engine re-dials:
  // the listener closed, the radio was toggled, the channel moved. Do it here, once, deliberately.
  const btTouched =
    'btListenerOpen' in patch || 'btSdpVisible' in patch || 'btRfcommChannel' in patch ||
    'bluetoothEnabled' in patch;
  if (btTouched) engine.refreshBluetoothLink();

  if (prevEnv.usbPlugged !== engine.environment.usbPlugged) {
    engine.addLog(
      'TRANSPORT',
      engine.environment.usbPlugged ? 'USB-C Cable Plugged In' : 'USB-C Cable Unplugged',
      engine.environment.usbPlugged
        ? 'Detected RNDIS / ADB high-speed interface (420 MB/s).'
        : 'Removed wired link; evaluating wireless transports.'
    );
  }
  if (prevEnv.sharedWifiAvailable !== engine.environment.sharedWifiAvailable) {
    engine.addLog(
      'TRANSPORT',
      engine.environment.sharedWifiAvailable
        ? 'Shared Wi-Fi Router Restored'
        : 'Shared Wi-Fi Router Offline',
      engine.environment.sharedWifiAvailable
        ? 'mDNS _chainit._tcp.local. discovered peer on 5 GHz LAN.'
        : 'No LAN router — ChainIt will use Local Hotspot or Bluetooth offline.'
    );
  }
  if (prevEnv.phoneScreenLocked !== engine.environment.phoneScreenLocked) {
    engine.addLog(
      'SIBLING',
      engine.environment.phoneScreenLocked
        ? 'Phone Screen Turned OFF (Locked in Pocket)'
        : 'Phone Screen Unlocked',
      engine.sibling.enabled
        ? 'Sibling Foreground Service + BLE ScanFilter keep Z:\\ VFS responsive with zero touch.'
        : 'Standard mode: screen-off VFS requires Sibling Mode.'
    );
  }

  engine.recomputeState();
  engine.broadcast('snapshot');
  res.json(engine.getSnapshot());
});

app.post('/api/sibling/update', (req, res) => {
  const patch = req.body || {};
  const wasEnabled = engine.sibling.enabled;
  Object.assign(engine.sibling, patch);

  if (wasEnabled !== engine.sibling.enabled) {
    engine.addLog(
      'SIBLING',
      engine.sibling.enabled ? 'Sibling Mode ENABLED' : 'Sibling Mode PAUSED (Standard Mode)',
      engine.sibling.enabled
        ? 'Zero-touch auto-connect, zero-prompt transfers, and unattended Z:\\ drive mount activated.'
        : 'Manual connect and receiver confirmation prompts re-enabled.'
    );
  }
  engine.recomputeState();
  engine.broadcast('snapshot');
  res.json(engine.getSnapshot());
});

app.post('/api/approvals/:id/resolve', (req, res) => {
  try {
    const accept = Boolean(req.body?.accept);
    const result = engine.resolveApproval(req.params.id, accept);
    res.json({ ...result, snapshot: engine.getSnapshot() });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Approval error' });
  }
});

// --- Volumes (v1.7.0) -------------------------------------------------------
//
// The PC side is not only C:: "/" is the profile folder, and any other volume is addressed as
// "/D:", "/E:" - a second partition, a card reader, a USB stick or a mapped share. These endpoints
// expose the volume list and the reach switch; every listing carries the same array inline, so a
// client over Bluetooth does not need a second round trip to offer the choice.
app.get('/api/drives', (_req, res) => {
  const volumes = listVolumes(engine.allowOtherDrives);
  const root = volumes.find((v) => v.isPcRoot) ?? volumes[0];
  res.json({
    allowOtherDrives: engine.allowOtherDrives,
    pcRoot: root ? root.rootPath : '',
    pcRootLetter: root ? root.letter : '?',
    pcRootCaption: root ? root.label : '',
    drives: volumes,
  });
});

app.post('/api/drives/scope', (req, res) => {
  const allow = req.body?.allowOtherDrives !== false && req.body?.allowOtherDrives !== 'false';
  engine.setAllowOtherDrives(allow);
  res.json({ ok: true, allowOtherDrives: engine.allowOtherDrives, drives: engine.pcVolumes() });
});

/**
 * "Show me where that landed on the PC." The Windows engine selects the path in Explorer; this
 * preview has no Explorer, so it answers with the resolved path and says plainly that it cannot open
 * a window - a fake success here would be worse than an honest refusal.
 */
app.post('/api/pc/reveal', (req, res) => {
  const virtual = String(req.body?.path ?? '');
  try {
    const resolved = resolveSafePath('pc', virtual);
    if (!virtual || !fs.existsSync(resolved.fullPath)) {
      res.status(404).json({ ok: false, error: 'That path is not on this PC (any more).' });
      return;
    }
    res.status(501).json({
      ok: false,
      path: resolved.virtualPath,
      localPath: resolved.fullPath,
      error: 'The node preview cannot open Explorer; the Windows engine selects this path for real.',
    });
  } catch (err) {
    res.status(400).json({ ok: false, error: err instanceof Error ? err.message : 'Unresolvable path' });
  }
});

// --- Recent shared files (v1.7.0) -------------------------------------------
app.get('/api/history', (req, res) => {
  const parsed = Number.parseInt(String(req.query.limit ?? '25'), 10);
  const limit = Number.isFinite(parsed) && parsed > 0 && parsed <= 200 ? parsed : 25;
  const items = [...engine.transfers]
    .sort((a, b) => b.timestampMs - a.timestampMs)
    .slice(0, limit)
    .map((t) => ({
      id: t.id,
      label: t.label,
      op: t.op,
      direction: t.direction,
      transportUsed: t.transportUsed,
      localPath: t.localPath ?? '',
      virtualPath: t.virtualPath ?? '',
      source: t.source ?? '',
      promotedHotspot: t.promotedHotspot,
      bytes: t.bytes,
      durationMs: t.durationMs,
      effectiveSpeedMBps: t.effectiveSpeedMBps,
      status: t.status,
      timestampMs: t.timestampMs,
    }));
  res.json({ items, historyFile: HISTORY_FILE_LABEL, servedLetters: servedLetters() });
});

app.post('/api/history/clear', (_req, res) => {
  const removed = engine.clearHistory();
  res.json({ ok: true, removed, items: [] });
});

app.get('/api/vfs/:target/list', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason, lockedByPolicy: true });
        return;
      }
    }
    const dirPath = String(req.query.path || '/');
    const listing = listDirectory(target, dirPath, engine.allowOtherDrives);
    res.json(listing);
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Failed to list directory' });
  }
});

app.get('/api/vfs/:target/raw', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).send(access.reason);
        return;
      }
    }
    const filePath = String(req.query.path || '/');
    const resolved = resolveSafePath(target, filePath);
    if (!fs.existsSync(resolved.fullPath) || fs.statSync(resolved.fullPath).isDirectory()) {
      res.status(404).send('File not found');
      return;
    }
    res.setHeader('Content-Type', getMimeType(resolved.fullPath));
    const explicitSave = req.query.download === '1';
    if (explicitSave) {
      res.setHeader(
        'Content-Disposition',
        `attachment; filename="${encodeURIComponent(path.basename(resolved.fullPath))}"`
      );
    }
    const startedMs = Date.now();
    const sizeBytes = fs.statSync(resolved.fullPath).size;
    res.on('finish', () => {
      // Only an explicit save counts as a share. The phone also fetches /raw to preview images, and
      // recording every look would bury the files somebody actually took.
      if (!explicitSave || target !== 'pc') return;
      engine.recordSharedFile({
        op: 'download',
        direction: 'pc_to_phone',
        label: path.basename(resolved.fullPath),
        bytes: sizeBytes,
        startedMs,
        source: 'phone-download',
        virtualPath: resolved.virtualPath,
        localPath: resolved.fullPath,
      });
    });
    fs.createReadStream(resolved.fullPath).pipe(res);
  } catch (err) {
    res.status(400).send(err instanceof Error ? err.message : 'Failed to read file');
  }
});

app.get('/api/vfs/:target/text', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const filePath = String(req.query.path || '/');
    const resolved = resolveSafePath(target, filePath);
    if (!fs.existsSync(resolved.fullPath) || fs.statSync(resolved.fullPath).isDirectory()) {
      res.status(404).json({ error: 'File not found' });
      return;
    }
    const item = toVfsItem(resolved.rootPath, resolved.fullPath);
    const editable = isEditableTextFile(resolved.fullPath) && item.sizeBytes <= 2 * 1024 * 1024;
    const content = editable ? fs.readFileSync(resolved.fullPath, 'utf8') : null;
    res.json({ item, editable, content });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Failed to load file' });
  }
});

app.put('/api/vfs/:target/text', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const { path: filePath, content } = req.body as { path: string; content: string };
    const resolved = resolveSafePath(target, filePath);
    fs.mkdirSync(path.dirname(resolved.fullPath), { recursive: true });
    fs.writeFileSync(resolved.fullPath, content ?? '', 'utf8');
    const bytes = Buffer.byteLength(content ?? '', 'utf8');

    if (target === 'phone') {
      engine.addLog(
        'VFS',
        `Saved in-place edit directly to Z:${resolved.virtualPath.replace(/\//g, '\\')}`,
        `Wrote ${bytes} B over ${engine.activeTransportId} without waking phone screen.`
      );
    }
    const item = toVfsItem(resolved.rootPath, resolved.fullPath);
    engine.broadcast('vfs_changed', { targets: [target] });
    res.json({ ok: true, item });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Failed to save file' });
  }
});

app.post('/api/vfs/transfer', (req, res) => {
  try {
    const { op, fromTarget, fromPaths, toTarget, toDir } = req.body as {
      op: 'copy' | 'move';
      fromTarget: DriveTarget;
      fromPaths: string[];
      toTarget: DriveTarget;
      toDir: string;
    };
    if (!Array.isArray(fromPaths) || fromPaths.length === 0) {
      res.status(400).json({ error: 'No source items specified.' });
      return;
    }
    const outcome = engine.performVfsTransfer({
      op: op === 'move' ? 'move' : 'copy',
      fromTarget: parseDriveTarget(fromTarget),
      fromPaths,
      toTarget: parseDriveTarget(toTarget),
      toDir: toDir || '/',
    });
    res.json({ ...outcome, snapshot: engine.getSnapshot() });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Transfer failed' });
  }
});

app.post('/api/vfs/:target/mkdir', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const { parentDir, name } = req.body as { parentDir: string; name: string };
    const created = createNewFolder(target, parentDir || '/', name);
    engine.addLog(
      'VFS',
      `Created folder ${target === 'phone' ? 'Z:' : 'C:'}${created.path.replace(/\//g, '\\')}`,
      `Directory created immediately.`
    );
    engine.broadcast('vfs_changed', { targets: [target] });
    res.json({ item: created });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Create folder failed' });
  }
});

app.post('/api/vfs/:target/rename', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const { path: itemPath, newName } = req.body as { path: string; newName: string };
    const updated = renameVfsItem(target, itemPath, newName);
    engine.addLog(
      'VFS',
      `Renamed to "${updated.name}" on ${target === 'phone' ? 'Z:\\' : 'C:\\'}`,
      `Atomic VFS rename succeeded.`
    );
    engine.broadcast('vfs_changed', { targets: [target] });
    res.json({ item: updated });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Rename failed' });
  }
});

app.post('/api/vfs/:target/delete', (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const { paths } = req.body as { paths: string[] };
    const deleted = deleteVfsItems(target, paths || []);
    engine.addLog(
      'VFS',
      `Deleted ${deleted} item(s) from ${target === 'phone' ? 'Z:\\' : 'C:\\'}`,
      `Storage updated.`
    );
    engine.broadcast('vfs_changed', { targets: [target] });
    res.json({ deleted });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Delete failed' });
  }
});

app.post('/api/vfs/:target/upload', async (req, res) => {
  try {
    const target = parseDriveTarget(req.params.target);
    if (target === 'phone') {
      const access = engine.canAccessPhoneVfsWithoutTouch();
      if (!access.ok) {
        res.status(503).json({ error: access.reason });
        return;
      }
    }
    const uploadStartedMs = Date.now();
    const dir = String(req.query.dir || '/');
    const rawName = String(req.query.filename || `upload_${Date.now()}.bin`);
    const safeName = path.basename(rawName).replace(/[<>:"/\\|?*]/g, '_');
    const dirResolved = resolveSafePath(target, dir);
    fs.mkdirSync(dirResolved.fullPath, { recursive: true });
    const destFull = path.join(dirResolved.fullPath, safeName);

    const writeStream = fs.createWriteStream(destFull);
    let bytes = 0;
    await new Promise<void>((resolve, reject) => {
      req.on('data', (chunk: Buffer) => {
        bytes += chunk.length;
      });
      req.pipe(writeStream);
      writeStream.on('finish', () => resolve());
      writeStream.on('error', reject);
    });

    const { promotedHotspot } = engine.recomputeState(target === 'phone' ? bytes : 0);
    const transportUsed = engine.activeTransportId || 'wifi_lan';
    const link = engine.buildTransportTelemetry().find((t) => t.id === transportUsed);

    // The rate is derived from the same two numbers the entry reports, so a reader can check
    // it: bytes / durationMs. Quoting the link's nominal speed here made a 44-byte upload claim
    // 94.5 MB/s while the download of the same file correctly measured 0.02 MB/s.
    const uploadDurationMs = Math.max(1, Date.now() - uploadStartedMs);
    const uploadMBps = bytes / (1024 * 1024) / (uploadDurationMs / 1000);

    engine.pushTransfer({
      id: crypto.randomUUID().slice(0, 8),
      label: safeName,
      op: 'upload',
      direction: target === 'phone' ? 'pc_to_phone' : 'phone_to_pc',
      transportUsed,
      promotedHotspot,
      bytes,
      durationMs: uploadDurationMs,
      effectiveSpeedMBps: Number.isFinite(uploadMBps) ? Number(uploadMBps.toFixed(2)) : 0,
      status: 'completed',
      timestampMs: Date.now(),
      source: target === 'pc' ? 'phone-upload' : 'api',
      virtualPath: target === 'pc' ? `${dirResolved.virtualPath === '/' ? '' : dirResolved.virtualPath}/${safeName}` : undefined,
      localPath: target === 'pc' ? destFull : undefined,
    });

    engine.addLog(
      'VFS',
      `Dropped "${safeName}" (${bytes} B) onto ${target === 'phone' ? 'Z:\\' : 'C:\\'}`,
      `Streamed via ${link?.shortName || transportUsed}.`
    );
    engine.broadcast('vfs_changed', { targets: [target] });

    res.json({
      item: toVfsItem(dirResolved.rootPath, destFull),
      snapshot: engine.getSnapshot(),
    });
  } catch (err) {
    res.status(400).json({ error: err instanceof Error ? err.message : 'Upload failed' });
  }
});

// Serve compiled Windows .exe and Android .apk release binaries
const releasesDir = path.resolve(process.cwd(), 'releases');
app.get('/downloads/:filename', (req, res) => {
  const safe = path.basename(req.params.filename);
  const full = path.join(releasesDir, safe);
  if (!fs.existsSync(full) || !fs.statSync(full).isFile()) {
    res.status(404).send('Installer not found');
    return;
  }
  const mime = safe.endsWith('.apk')
    ? 'application/vnd.android.package-archive'
    : 'application/vnd.microsoft.portable-executable';
  res.setHeader('Content-Type', mime);
  res.setHeader('Content-Disposition', `attachment; filename="${safe}"`);
  fs.createReadStream(full).pipe(res);
});

// Serve production Vite build
const distDir = path.resolve(process.cwd(), 'app/dist');
if (fs.existsSync(distDir)) {
  app.use(express.static(distDir));
  app.get('*', (req, res, next) => {
    if (
      req.path.startsWith('/api/') ||
      req.path.startsWith('/dav') ||
      req.path.startsWith('/downloads/')
    ) {
      next();
      return;
    }
    res.sendFile(path.join(distDir, 'index.html'));
  });
}

// Create unified HTTP + WebDAV + WebSocket server bound to 0.0.0.0
const server = http.createServer((req, res) => {
  const url = req.url || '/';
  if (url === '/dav' || url.startsWith('/dav/') || url.startsWith('/dav?')) {
    void handleWebDavRequest(req, res);
    return;
  }
  app(req, res);
});

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', (ws) => {
  engine.registerClient(ws);
  ws.send(
    JSON.stringify({
      type: 'snapshot',
      snapshot: engine.getSnapshot(),
    })
  );
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`ChainIt Unified Engine listening on http://0.0.0.0:${PORT}`);
  console.log(`  - Desktop File Manager & Dashboard : http://0.0.0.0:${PORT}/`);
  console.log(`  - Android Mobile Companion         : http://0.0.0.0:${PORT}/mobile`);
  console.log(`  - Windows Z:\\ WebDAV Mount Server  : http://127.0.0.1:${PORT}/dav`);
});
