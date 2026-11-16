import { DriveTarget } from '../../shared/types';

export function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const idx = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const val = bytes / Math.pow(1024, idx);
  return `${val < 10 && idx > 0 ? val.toFixed(1) : Math.round(val)} ${units[idx]}`;
}

export function toWindowsDisplayPath(target: DriveTarget, virtualPath: string): string {
  const drive = target === 'phone' ? 'Z:' : 'C:';
  if (!virtualPath || virtualPath === '/') return `${drive}\\`;
  return `${drive}${virtualPath.replace(/\//g, '\\')}`;
}

export function formatShortTime(tsMs: number): string {
  return new Date(tsMs).toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export function isImageMime(mime: string, ext: string): boolean {
  return (
    mime.startsWith('image/') ||
    ['.svg', '.png', '.jpg', '.jpeg', '.webp', '.gif'].includes(ext.toLowerCase())
  );
}
