import {
  DriveTarget,
  EnvironmentSimulation,
  SelectionMode,
  SiblingConfig,
  SystemSnapshot,
  TransportId,
  VfsItem,
} from '../../shared/types';

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
      parseJsonOrThrow<{ currentPath: string; items: VfsItem[] }>(r)
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
