import { VfsItem } from '../../shared/types';

export type SortKey = 'name' | 'size' | 'modified' | 'type';

export interface SortState {
  key: SortKey;
  desc: boolean;
  foldersFirst: boolean;
}

export const DEFAULT_SORT: SortState = { key: 'name', desc: false, foldersFirst: true };

export const SORT_LABELS: Record<SortKey, string> = {
  name: 'Name',
  size: 'Size',
  modified: 'Date modified',
  type: 'Type',
};

const PREF_KEY = 'chainit.sort.v1';

export function loadSortPref(): SortState {
  try {
    const raw = localStorage.getItem(PREF_KEY);
    if (!raw) return { ...DEFAULT_SORT };
    const parsed = JSON.parse(raw) as Partial<SortState>;
    const key: SortKey =
      parsed.key === 'size' || parsed.key === 'modified' || parsed.key === 'type'
        ? parsed.key
        : 'name';
    return {
      key,
      desc: Boolean(parsed.desc),
      foldersFirst: parsed.foldersFirst === undefined ? true : Boolean(parsed.foldersFirst),
    };
  } catch {
    return { ...DEFAULT_SORT };
  }
}

export function saveSortPref(state: SortState): void {
  try {
    localStorage.setItem(PREF_KEY, JSON.stringify(state));
  } catch {
    // Private mode / quota — sorting still works for this session.
  }
}

/** Explorer-grade ordering: folders first, then the active key with a stable name tiebreak. */
export function sortItems(items: VfsItem[], state: SortState): VfsItem[] {
  const dir = state.desc ? -1 : 1;
  const copy = items.slice();
  copy.sort((a, b) => {
    if (state.foldersFirst && a.kind !== b.kind) {
      return a.kind === 'directory' ? -1 : 1;
    }
    let cmp = 0;
    switch (state.key) {
      case 'size':
        cmp = a.sizeBytes - b.sizeBytes;
        break;
      case 'modified':
        cmp = a.modifiedMs - b.modifiedMs;
        break;
      case 'type':
        cmp = (a.ext || '').localeCompare(b.ext || '') || a.mimeType.localeCompare(b.mimeType);
        break;
      case 'name':
      default:
        cmp = 0;
        break;
    }
    if (cmp === 0) {
      cmp = a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
    }
    return cmp === 0 ? 0 : cmp > 0 ? dir : -dir;
  });
  return copy;
}
