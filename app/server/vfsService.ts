import fs from 'node:fs';
import path from 'node:path';
import { DriveStats, DriveTarget, PcVolume, VfsItem } from '../shared/types.js';
import { PC_DRIVE_ROOT, PHONE_DRIVE_ROOT } from './seedStorage.js';

const MIME_BY_EXT: Record<string, string> = {
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.webp': 'image/webp',
  '.gif': 'image/gif',
  '.md': 'text/markdown',
  '.txt': 'text/plain',
  '.json': 'application/json',
  '.csv': 'text/csv',
  '.ts': 'text/plain',
  '.rs': 'text/plain',
  '.kt': 'text/plain',
  '.xml': 'application/xml',
  '.pdf': 'application/pdf',
  '.mp3': 'audio/mpeg',
  '.wav': 'audio/wav',
  '.mp4': 'video/mp4',
};

export function getRootForTarget(target: DriveTarget): string {
  return target === 'pc' ? PC_DRIVE_ROOT : PHONE_DRIVE_ROOT;
}

export function normalizeVirtualPath(virtualPath: string | undefined): string {
  if (!virtualPath) return '/';
  const unified = virtualPath.replace(/\\/g, '/').trim();
  const normalized = path.posix.normalize('/' + unified);
  return normalized === '' ? '/' : normalized;
}

/**
 * A virtual path may name a volume outright ("/D:/Games"), which is how the PC side reaches a second
 * partition, a card reader or a mapped share instead of only the profile folder on C: (v1.7.0).
 * Returns the letter and the remainder, or null when the path is an ordinary root-relative one.
 */
export function parseDrivePrefix(virtualPath: string | undefined): { letter: string; rest: string } | null {
  if (!virtualPath) return null;
  const m = /^\/([A-Za-z]):(?:\/.*)?$/.exec(virtualPath.replace(/\\/g, '/'));
  if (!m) return null;
  const unified = virtualPath.replace(/\\/g, '/');
  return { letter: m[1].toUpperCase(), rest: unified.slice(m[1].length + 2) };
}

/**
 * The volumes this runtime can serve.
 *
 * The Windows engine enumerates real volumes with GetLogicalDrives/GetDriveTypeW. This node preview
 * has exactly one seeded volume, and it says so instead of inventing D: and E: - an unavailable drive
 * must produce the same honest "not present" answer a client gets from the engine.
 */
export function listVolumes(allowOtherDrives = true): PcVolume[] {
  const rootPath = path.resolve(PC_DRIVE_ROOT);
  fs.mkdirSync(rootPath, { recursive: true });

  const onWindows = process.platform === 'win32';
  const letter = onWindows ? (path.parse(rootPath).root.replace(/\\$/, '').toUpperCase() || 'C') : 'C';
  const kind = onWindows ? (process.env.SystemRoot ? 'fixed' : 'fixed') : 'fixed';

  let totalBytes = 0;
  let freeBytes = 0;
  try {
    // statfsSync exists on Node 18.15+/20+; when the platform refuses it, the sizes stay 0 rather
    // than being guessed.
    const st = (fs as unknown as { statfsSync?: (p: string) => { bsize: number; blocks: number; bfree: number } })
      .statfsSync;
    if (typeof st === 'function') {
      const info = st(rootPath);
      totalBytes = info.bsize * info.blocks;
      freeBytes = info.bsize * info.bfree;
    }
  } catch {
    totalBytes = 0;
    freeBytes = 0;
  }

  const volumes: PcVolume[] = [
    {
      letter,
      virtualPath: '/',
      label: onWindows
        ? `Windows profile folder (${path.basename(rootPath)})`
        : `Preview volume (${path.basename(rootPath)})`,
      rootPath: rootPath.split(path.sep).join('/'),
      kind: kind as PcVolume['kind'],
      isPcRoot: true,
      ready: true,
      totalBytes,
      freeBytes,
    },
  ];
  void allowOtherDrives;
  return volumes;
}

/** The single letter this runtime can actually serve; anything else is honestly "not present". */
export function servedLetters(): string[] {
  return listVolumes().map((v) => v.letter);
}

export function resolveSafePath(target: DriveTarget, virtualPath: string | undefined): {
  fullPath: string;
  virtualPath: string;
  rootPath: string;
} {
  const rootPath = path.resolve(getRootForTarget(target));

  const drive = target === 'pc' ? parseDrivePrefix(virtualPath) : null;
  if (drive) {
    const served = servedLetters();
    if (!served.includes(drive.letter)) {
      throw new Error(
        `Drive ${drive.letter}: is not present on this PC (the node preview serves ${served.join(', ')}: only).`
      );
    }
    // Same volume as the root: "/C:/Documents" is exactly "/Documents".
    const cleanVirtual = normalizeVirtualPath(drive.rest || '/');
    const rel = cleanVirtual.replace(/^\/+/, '');
    const fullPath = path.resolve(rootPath, rel);
    if (fullPath !== rootPath && !fullPath.startsWith(rootPath + path.sep)) {
      throw new Error(`Blocked path traversal attempt: ${virtualPath}`);
    }
    return { fullPath, virtualPath: `/${drive.letter}:${cleanVirtual === '/' ? '' : cleanVirtual}`, rootPath };
  }

  const cleanVirtual = normalizeVirtualPath(virtualPath);
  const rel = cleanVirtual.replace(/^\/+/, '');
  const fullPath = path.resolve(rootPath, rel);

  if (fullPath !== rootPath && !fullPath.startsWith(rootPath + path.sep)) {
    throw new Error(`Blocked path traversal attempt: ${virtualPath}`);
  }
  return { fullPath, virtualPath: cleanVirtual, rootPath };
}

export function getMimeType(filename: string): string {
  const ext = path.extname(filename).toLowerCase();
  return MIME_BY_EXT[ext] || 'application/octet-stream';
}

export function isEditableTextFile(filename: string): boolean {
  const ext = path.extname(filename).toLowerCase();
  return [
    '.md',
    '.txt',
    '.json',
    '.csv',
    '.xml',
    '.svg',
    '.ts',
    '.js',
    '.rs',
    '.kt',
    '.ps1',
    '.yml',
    '.yaml',
    '.ini',
    '.log',
  ].includes(ext);
}

export function toVfsItem(rootPath: string, fullPath: string): VfsItem {
  const stat = fs.statSync(fullPath);
  const rel = path.relative(rootPath, fullPath).split(path.sep).join('/');
  const virtualPath = '/' + rel;
  const name = path.basename(fullPath);
  const ext = stat.isDirectory() ? '' : path.extname(name).toLowerCase();

  return {
    name,
    path: virtualPath === '/' ? '/' : virtualPath,
    kind: stat.isDirectory() ? 'directory' : 'file',
    sizeBytes: stat.isDirectory() ? 0 : stat.size,
    modifiedMs: stat.mtimeMs,
    mimeType: stat.isDirectory() ? 'inode/directory' : getMimeType(name),
    ext,
  };
}

export function listDirectory(
  target: DriveTarget,
  dirVirtualPath: string,
  allowOtherDrives = true
): {
  currentPath: string;
  items: VfsItem[];
  drives?: PcVolume[];
  allowOtherDrives?: boolean;
  pcRoot?: string;
  error?: string;
} {
  const drives = target === 'pc' ? listVolumes(allowOtherDrives) : undefined;

  // A named volume that is not there gets the runtime's own sentence instead of a 400: the client
  // cannot tell "empty drive" from "wrong drive" without it, and over Bluetooth a retry is expensive.
  const drive = target === 'pc' ? parseDrivePrefix(dirVirtualPath) : null;
  if (drive && !servedLetters().includes(drive.letter)) {
    const why = allowOtherDrives
      ? `Drive ${drive.letter}: is not present on this PC.`
      : `Access to drives other than ${servedLetters().join(', ')}: is switched off on the PC.`;
    return {
      currentPath: `/${drive.letter}:`,
      items: [],
      drives,
      allowOtherDrives,
      pcRoot: '/',
      error: why,
    };
  }

  const { fullPath, virtualPath, rootPath } = resolveSafePath(target, dirVirtualPath);
  if (!fs.existsSync(fullPath)) {
    throw new Error(`Directory does not exist: ${virtualPath}`);
  }
  const stat = fs.statSync(fullPath);
  if (!stat.isDirectory()) {
    throw new Error(`Not a directory: ${virtualPath}`);
  }

  const entries = fs.readdirSync(fullPath);
  const items = entries
    .filter((name) => !name.startsWith('.'))
    .map((name) => toVfsItem(rootPath, path.join(fullPath, name)))
    .sort((a, b) => {
      if (a.kind !== b.kind) return a.kind === 'directory' ? -1 : 1;
      return a.name.localeCompare(b.name, undefined, { sensitivity: 'base' });
    });

  return {
    currentPath: virtualPath,
    items: items.slice(0, 400),
    drives,
    allowOtherDrives: target === 'pc' ? allowOtherDrives : undefined,
    pcRoot: target === 'pc' ? '/' : undefined,
  };
}

export function computeSubtreeBytes(fullPath: string): {
  bytes: number;
  files: number;
  folders: number;
} {
  if (!fs.existsSync(fullPath)) return { bytes: 0, files: 0, folders: 0 };
  const stat = fs.statSync(fullPath);
  if (stat.isFile()) {
    return { bytes: stat.size, files: 1, folders: 0 };
  }
  let bytes = 0;
  let files = 0;
  let folders = 1;
  for (const child of fs.readdirSync(fullPath)) {
    if (child.startsWith('.')) continue;
    const sub = computeSubtreeBytes(path.join(fullPath, child));
    bytes += sub.bytes;
    files += sub.files;
    folders += sub.folders;
  }
  return { bytes, files, folders };
}

export function getDriveStats(target: DriveTarget): DriveStats {
  const root = getRootForTarget(target);
  fs.mkdirSync(root, { recursive: true });
  const summary = computeSubtreeBytes(root);
  if (target === 'pc') {
    const vol = listVolumes()[0];
    // Real capacity when the platform reports it; the old fixed 512 GB figure stays only as the
    // fallback for a runtime without statfs, and is then labelled as such by `ready`.
    const total = vol.totalBytes > 0 ? vol.totalBytes : 512 * 1024 * 1024 * 1024;
    const used = vol.totalBytes > 0 ? vol.totalBytes - vol.freeBytes : 184 * 1024 * 1024 * 1024 + summary.bytes;
    return {
      target: 'pc',
      driveLetter: `${vol.letter}:`,
      label: vol.label,
      totalCapacityBytes: total,
      usedBytes: used,
      fileCount: summary.files,
      folderCount: Math.max(0, summary.folders - 1),
    };
  }
  return {
    target: 'phone',
    driveLetter: 'Z:',
    label: 'Galaxy S24 Ultra [Sibling VFS]',
    totalCapacityBytes: 256 * 1024 * 1024 * 1024,
    usedBytes: 92 * 1024 * 1024 * 1024 + summary.bytes,
    fileCount: summary.files,
    folderCount: Math.max(0, summary.folders - 1),
  };
}

function getUniqueCopyDestination(destDirFull: string, baseName: string): string {
  let candidate = path.join(destDirFull, baseName);
  if (!fs.existsSync(candidate)) return candidate;

  const ext = path.extname(baseName);
  const stem = baseName.slice(0, baseName.length - ext.length);
  let index = 1;
  while (true) {
    const suffix = index === 1 ? ' - Copy' : ` - Copy (${index})`;
    candidate = path.join(destDirFull, `${stem}${suffix}${ext}`);
    if (!fs.existsSync(candidate)) return candidate;
    index++;
  }
}

export function executeCopyOrMove(params: {
  op: 'copy' | 'move';
  fromTarget: DriveTarget;
  fromPaths: string[];
  toTarget: DriveTarget;
  toDir: string;
}): { totalBytes: number; count: number; itemNames: string[] } {
  const { op, fromTarget, fromPaths, toTarget, toDir } = params;
  const destResolved = resolveSafePath(toTarget, toDir);
  fs.mkdirSync(destResolved.fullPath, { recursive: true });

  let totalBytes = 0;
  let count = 0;
  const itemNames: string[] = [];

  for (const srcVirtual of fromPaths) {
    const srcResolved = resolveSafePath(fromTarget, srcVirtual);
    if (!fs.existsSync(srcResolved.fullPath)) continue;

    const baseName = path.basename(srcResolved.fullPath);
    const subtree = computeSubtreeBytes(srcResolved.fullPath);

    let targetFullPath = path.join(destResolved.fullPath, baseName);

    // Guard against moving/copying a folder into itself
    if (
      fromTarget === toTarget &&
      (destResolved.fullPath === srcResolved.fullPath ||
        destResolved.fullPath.startsWith(srcResolved.fullPath + path.sep))
    ) {
      throw new Error(`Cannot ${op} "${baseName}" into itself.`);
    }

    if (op === 'copy') {
      if (fromTarget === toTarget && path.dirname(srcResolved.fullPath) === destResolved.fullPath) {
        targetFullPath = getUniqueCopyDestination(destResolved.fullPath, baseName);
      }
      fs.cpSync(srcResolved.fullPath, targetFullPath, { recursive: true, force: true });
    } else {
      // move
      if (srcResolved.fullPath === targetFullPath) {
        continue;
      }
      if (fs.existsSync(targetFullPath)) {
        fs.rmSync(targetFullPath, { recursive: true, force: true });
      }
      try {
        fs.renameSync(srcResolved.fullPath, targetFullPath);
      } catch {
        fs.cpSync(srcResolved.fullPath, targetFullPath, { recursive: true, force: true });
        fs.rmSync(srcResolved.fullPath, { recursive: true, force: true });
      }
    }

    totalBytes += Math.max(subtree.bytes, 512);
    count += 1;
    itemNames.push(path.basename(targetFullPath));
  }

  return { totalBytes, count, itemNames };
}

export function createNewFolder(target: DriveTarget, parentDir: string, folderName: string): VfsItem {
  const sanitized = folderName.replace(/[<>:"/\\|?*]/g, '').trim();
  if (!sanitized) throw new Error('Invalid folder name.');
  const parent = resolveSafePath(target, parentDir);
  const full = path.join(parent.fullPath, sanitized);
  if (fs.existsSync(full)) {
    throw new Error(`"${sanitized}" already exists.`);
  }
  fs.mkdirSync(full, { recursive: true });
  return toVfsItem(parent.rootPath, full);
}

export function renameVfsItem(target: DriveTarget, itemPath: string, newName: string): VfsItem {
  const sanitized = newName.replace(/[<>:"/\\|?*]/g, '').trim();
  if (!sanitized) throw new Error('Invalid file or folder name.');
  const src = resolveSafePath(target, itemPath);
  if (!fs.existsSync(src.fullPath)) throw new Error('Item not found.');
  const dstFull = path.join(path.dirname(src.fullPath), sanitized);
  if (dstFull !== src.fullPath && fs.existsSync(dstFull)) {
    throw new Error(`"${sanitized}" already exists in this folder.`);
  }
  fs.renameSync(src.fullPath, dstFull);
  return toVfsItem(src.rootPath, dstFull);
}

export function deleteVfsItems(target: DriveTarget, itemPaths: string[]): number {
  let deleted = 0;
  for (const p of itemPaths) {
    const resolved = resolveSafePath(target, p);
    if (resolved.fullPath === resolved.rootPath) {
      throw new Error('Cannot delete drive root.');
    }
    if (fs.existsSync(resolved.fullPath)) {
      fs.rmSync(resolved.fullPath, { recursive: true, force: true });
      deleted++;
    }
  }
  return deleted;
}
