import {
  DriveTarget,
  EnvironmentSimulation,
  PcVolume,
  SelectionMode,
  SiblingConfig,
  SystemSnapshot,
  TransferRecord,
  TransportId,
  VfsItem,
} from '../../shared/types';

/** What `/api/vfs/:target/list` answers from v1.7.0 on: rows plus the volumes they live on. */
export interface VfsListing {
  currentPath: string;
  items: VfsItem[];
  /** Every volume the PC reports, so a client can offer "switch to D:" without another round trip. */
  drives?: PcVolume[];
  allowOtherDrives?: boolean;
  pcRoot?: string;
  /**
   * Set when the PC refused the path: a volume with no media, an offline share, or reach switched
   * off. Showing this sentence beats showing an empty folder that looks like a bug.
   */
  error?: string;
}

async function parseJsonOrThrow<T>(res: Response): Promise<T> {
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error((data && data.error) || `Request failed (${res.status})`);
  }
  return data as T;
}

export const api = {
  getState: () => fetch('/api/state').then((r) => parseJsonOrThrow<SystemSnapshot>(r)),

  setTransportMode: (mode: SelectionMode, manualTransportId?: TransportId) =>
    fetch('/api/transport/mode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode, manualTransportId }),
    }).then((r) => parseJsonOrThrow<SystemSnapshot>(r)),

  updateEnvironment: (patch: Partial<EnvironmentSimulation>) =>
    fetch('/api/environment/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    }).then((r) => parseJsonOrThrow<SystemSnapshot>(r)),

  updateSibling: (patch: Partial<SiblingConfig>) =>
    fetch('/api/sibling/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    }).then((r) => parseJsonOrThrow<SystemSnapshot>(r)),

  resolveApproval: (id: string, accept: boolean) =>
    fetch(`/api/approvals/${encodeURIComponent(id)}/resolve`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ accept }),
    }).then((r) => parseJsonOrThrow<{ accepted: boolean; snapshot: SystemSnapshot }>(r)),

  listDir: (target: DriveTarget, dirPath: string) =>
    fetch(`/api/vfs/${target}/list?path=${encodeURIComponent(dirPath)}`).then((r) =>
      parseJsonOrThrow<VfsListing>(r)
    ),

  /** The PC's volumes and whether "/" may name one of them. */
  listDrives: () =>
    fetch('/api/drives').then((r) =>
      parseJsonOrThrow<{
        allowOtherDrives: boolean;
        pcRoot: string;
        pcRootLetter: string;
        pcRootCaption: string;
        drives: PcVolume[];
      }>(r)
    ),

  /** Widens or narrows what the PC shares; persisted on the PC, not in this tab. */
  setDriveScope: (allowOtherDrives: boolean) =>
    fetch('/api/drives/scope', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ allowOtherDrives }),
    }).then((r) =>
      parseJsonOrThrow<{ ok: boolean; allowOtherDrives: boolean; drives: PcVolume[] }>(r)
    ),

  /** Recent shared files, newest first, as the PC records them. */
  history: (limit = 25) =>
    fetch(`/api/history?limit=${limit}`).then((r) =>
      parseJsonOrThrow<{ items: TransferRecord[]; historyFile: string; servedLetters?: string[] }>(r)
    ),

  clearHistory: () =>
    fetch('/api/history/clear', { method: 'POST' }).then((r) =>
      parseJsonOrThrow<{ ok: boolean; removed: number; items: TransferRecord[] }>(r)
    ),

  /** Asks the PC to select a path in Explorer - "show me where that landed". */
  revealOnPc: (virtualPath: string) =>
    fetch('/api/pc/reveal', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: virtualPath }),
    }).then((r) =>
      parseJsonOrThrow<{ ok: boolean; path?: string; localPath?: string; error?: string }>(r)
    ),

  readText: (target: DriveTarget, filePath: string) =>
    fetch(`/api/vfs/${target}/text?path=${encodeURIComponent(filePath)}`).then((r) =>
      parseJsonOrThrow<{ item: VfsItem; editable: boolean; content: string | null }>(r)
    ),

  saveText: (target: DriveTarget, filePath: string, content: string) =>
    fetch(`/api/vfs/${target}/text`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: filePath, content }),
    }).then((r) => parseJsonOrThrow<{ ok: boolean; item: VfsItem }>(r)),

  transfer: (params: {
    op: 'copy' | 'move';
    fromTarget: DriveTarget;
    fromPaths: string[];
    toTarget: DriveTarget;
    toDir: string;
  }) =>
    fetch('/api/vfs/transfer', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(params),
    }).then((r) =>
      parseJsonOrThrow<{
        status: 'completed' | 'awaiting_approval';
        snapshot: SystemSnapshot;
      }>(r)
    ),

  mkdir: (target: DriveTarget, parentDir: string, name: string) =>
    fetch(`/api/vfs/${target}/mkdir`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ parentDir, name }),
    }).then((r) => parseJsonOrThrow<{ item: VfsItem }>(r)),

  rename: (target: DriveTarget, path: string, newName: string) =>
    fetch(`/api/vfs/${target}/rename`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, newName }),
    }).then((r) => parseJsonOrThrow<{ item: VfsItem }>(r)),

  deleteItems: (target: DriveTarget, paths: string[]) =>
    fetch(`/api/vfs/${target}/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ paths }),
    }).then((r) => parseJsonOrThrow<{ deleted: number }>(r)),

  uploadFile: (target: DriveTarget, dir: string, file: File) =>
    fetch(
      `/api/vfs/${target}/upload?dir=${encodeURIComponent(dir)}&filename=${encodeURIComponent(
        file.name
      )}`,
      {
        method: 'POST',
        headers: { 'Content-Type': file.type || 'application/octet-stream' },
        body: file,
      }
    ).then((r) => parseJsonOrThrow<{ item: VfsItem; snapshot: SystemSnapshot }>(r)),

  rawUrl: (target: DriveTarget, filePath: string, download = false) =>
    `/api/vfs/${target}/raw?path=${encodeURIComponent(filePath)}${download ? '&download=1' : ''}`,

  connectPhoneIp: (phoneIp: string, siblingKey?: string) =>
    fetch('/api/transport/connect-ip', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ phoneIp, siblingKey }),
    }).then((r) => parseJsonOrThrow<SystemSnapshot>(r)),

  /** Version of the engine currently serving this UI (Windows exe or Node preview). */
  appVersion: () =>
    fetch('/api/app/version').then((r) =>
      parseJsonOrThrow<{ version: string; runtime: string; channel: string }>(r)
    ),

  /**
   * Downloads the newest release and replaces the running binary in place.
   * On Windows this swaps ChainIt-Windows.exe and relaunches it (nothing is installed twice).
   */
  /** Asks the Windows engine to place a ChainIt.lnk (chain-link icon) on the Desktop. */
  createDesktopShortcut: () =>
    fetch('/api/app/desktop-shortcut', { method: 'POST' }).then((r) =>
      parseJsonOrThrow<{ ok: boolean; path: string; message: string }>(r)
    ),

  checkForUpdate: () =>
    fetch('/api/app/update', { method: 'POST' }).then((r) =>
      parseJsonOrThrow<{
        status: 'updated' | 'up_to_date' | 'unsupported' | 'error';
        message: string;
        version?: string;
        latestVersion?: string;
      }>(r)
    ),

  probeWebDavRoot: async (): Promise<{ status: number; xml: string; durationMs: number }> => {
    const start = performance.now();
    const res = await fetch('/dav/', {
      method: 'PROPFIND',
      headers: { Depth: '1' },
    });
    const xml = await res.text();
    return {
      status: res.status,
      xml,
      durationMs: Math.max(1, Math.round(performance.now() - start)),
    };
  },
};
