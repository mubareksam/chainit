import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { DriveTarget, SelectionMode, TransportId } from '../shared/types.js';
import { engine } from './engineState.js';
import { ensureSeededStorage } from './seedStorage.js';
import {
  createNewFolder,
  deleteVfsItems,
  getMimeType,
  isEditableTextFile,
  listDirectory,
  renameVfsItem,
  resolveSafePath,
  toVfsItem,
} from './vfsService.js';
import { handleWebDavRequest } from './webdavHandler.js';

ensureSeededStorage();

const PORT = Number(process.env.PORT || 4173);
engine.serverPort = PORT;

const app = express();

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
    engine.addLog(
      'BLUETOOTH',
      `Native Bluetooth RFCOMM link pinned -> ${btLink}`,
      'AF_BTH socket to phone UUID c4a1b170-0001-4000-8000-00805f9b34fb; Z:\\ VFS + WebDAV now run over Bluetooth alone.'
    );
    engine.broadcast('snapshot');
    res.json(engine.getSnapshot());
    return;
  }

  engine.environment.sharedWifiAvailable = true;
  engine.manualPhoneEndpoint = phoneIp.trim();
  engine.recomputeState();
  engine.addLog(
    'TRANSPORT',
    `Linked to Android Phone IP ${phoneIp.trim()}:49180`,
    'Verified Sibling VFS endpoint.'
  );
  engine.broadcast('snapshot');
  res.json(engine.getSnapshot());
});

app.post('/api/environment/update', (req, res) => {
  const patch = req.body || {};
  const prevEnv = { ...engine.environment };
  Object.assign(engine.environment, patch);

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
    const listing = listDirectory(target, dirPath);
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
    if (req.query.download === '1') {
      res.setHeader(
        'Content-Disposition',
        `attachment; filename="${encodeURIComponent(path.basename(resolved.fullPath))}"`
      );
    }
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

    engine.transfers.unshift({
      id: crypto.randomUUID().slice(0, 8),
      label: safeName,
      op: 'upload',
      direction: target === 'phone' ? 'pc_to_phone' : 'phone_to_pc',
      transportUsed,
      promotedHotspot,
      bytes,
      durationMs: 2,
      effectiveSpeedMBps: link?.speedMBps || 85,
      status: 'completed',
      timestampMs: Date.now(),
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
