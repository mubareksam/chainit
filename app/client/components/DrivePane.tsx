import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  ArrowDown as ArrowDownIcon,
  ArrowLeft,
  ArrowLeftRight,
  ArrowRight,
  ArrowUp,
  ArrowUp as ArrowUpIcon,
  Check,
  Clipboard,
  Copy,
  Download,
  Edit3,
  FileText,
  Folder,
  FolderPlus,
  HardDrive,
  Image as ImageIcon,
  LayoutGrid,
  List as ListIcon,
  Lock,
  MoveRight,
  RefreshCw,
  Scissors,
  Search,
  ShieldCheck,
  Smartphone,
  Trash2,
  Upload,
  X,
} from 'lucide-react';
import { DriveStats, DriveTarget, SystemSnapshot, VfsItem } from '../../shared/types';
import { api } from '../api/client';
import { formatBytes, isImageMime, toWindowsDisplayPath } from '../utils/format';
import { SORT_LABELS, SortState, sortItems } from '../utils/sort';
import { SortMenu } from './SortMenu';

export interface ClipboardState {
  op: 'copy' | 'move';
  fromTarget: DriveTarget;
  paths: string[];
  names: string[];
}

interface Props {
  target: DriveTarget;
  otherTarget: DriveTarget;
  otherCurrentDir: string;
  currentDir: string;
  onNavigate: (dir: string) => void;
  stats: DriveStats;
  snapshot: SystemSnapshot;
  refreshToken: number;
  isFocused: boolean;
  onFocusPane: () => void;
  clipboard: ClipboardState | null;
  onSetClipboard: (clip: ClipboardState) => void;
  onPasteInto: (toTarget: DriveTarget, toDir: string) => Promise<void>;
  onDirectTransfer: (params: {
    op: 'copy' | 'move';
    fromTarget: DriveTarget;
    fromPaths: string[];
    toTarget: DriveTarget;
    toDir: string;
  }) => Promise<void>;
  onOpenFile: (target: DriveTarget, item: VfsItem) => void;
  onQuickEnableSibling: () => void;
  notify: (msg: string, kind?: 'success' | 'error' | 'info') => void;
  sort: SortState;
  onSortChange: (next: SortState) => void;
}

const PHONE_SHORTCUTS = [
  { label: 'Root (Z:\\)', path: '/' },
  { label: 'Camera', path: '/DCIM/Camera' },
  { label: 'Screenshots', path: '/Pictures/Screenshots' },
  { label: 'Download', path: '/Download' },
  { label: 'Documents', path: '/Documents' },
  { label: 'Music', path: '/Music' },
  { label: 'Inbox', path: '/ChainIt_Inbox' },
];

const PC_SHORTCUTS = [
  { label: 'Root (C:\\)', path: '/' },
  { label: 'Desktop', path: '/Desktop' },
  { label: 'Documents', path: '/Documents' },
  { label: 'Downloads', path: '/Downloads' },
  { label: 'Pictures', path: '/Pictures' },
  { label: 'Projects', path: '/Projects' },
];

export const DrivePane: React.FC<Props> = ({
  target,
  otherTarget,
  otherCurrentDir,
  currentDir,
  onNavigate,
  stats,
  snapshot,
  refreshToken,
  isFocused,
  onFocusPane,
  clipboard,
  onSetClipboard,
  onPasteInto,
  onDirectTransfer,
  onOpenFile,
  onQuickEnableSibling,
  notify,
  sort,
  onSortChange,
}) => {
  const [items, setItems] = useState<VfsItem[]>([]);
  const [loading, setLoading] = useState(false);
  const [unavailableError, setUnavailableError] = useState<string | null>(null);
  const [selectedPaths, setSelectedPaths] = useState<string[]>([]);
  const [viewMode, setViewMode] = useState<'list' | 'grid'>('list');
  const [search, setSearch] = useState('');
  const [isDragOverPane, setIsDragOverPane] = useState(false);
  const [dragOverFolderPath, setDragOverFolderPath] = useState<string | null>(null);

  // Inline dialogs (zero browser prompt() dependencies)
  const [creatingFolder, setCreatingFolder] = useState(false);
  const [newFolderName, setNewFolderName] = useState('');
  const [renamingPath, setRenamingPath] = useState<string | null>(null);
  const [renameValue, setRenameValue] = useState('');

  // Context menu
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number; item: VfsItem | null } | null>(
    null
  );

  const fileInputRef = useRef<HTMLInputElement>(null);

  const loadDirectory = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listDir(target, currentDir);
      setItems(res.items);
      setUnavailableError(null);
    } catch (err) {
      const message = err instanceof Error ? err.message : 'Unable to read directory';
      if (
        currentDir !== '/' &&
        message.toLowerCase().includes('does not exist')
      ) {
        onNavigate('/');
        return;
      }
      setUnavailableError(message);
      setItems([]);
    } finally {
      setLoading(false);
    }
  }, [target, currentDir, onNavigate]);

  useEffect(() => {
    void loadDirectory();
  }, [loadDirectory, refreshToken]);

  useEffect(() => {
    setSelectedPaths([]);
    setRenamingPath(null);
    setCreatingFolder(false);
  }, [currentDir]);

  useEffect(() => {
    const closeMenu = () => setCtxMenu(null);
    window.addEventListener('click', closeMenu);
    return () => window.removeEventListener('click', closeMenu);
  }, []);

  const filteredItems = items.filter((i) =>
    search.trim() ? i.name.toLowerCase().includes(search.trim().toLowerCase()) : true
  );

  /** Explorer-grade ordering (name / size / date / type, direction, folders first). */
  const shownItems = useMemo(() => sortItems(filteredItems, sort), [filteredItems, sort]);

  const selectedBytes = useMemo(
    () => items.filter((i) => selectedPaths.includes(i.path)).reduce((acc, i) => acc + i.sizeBytes, 0),
    [items, selectedPaths]
  );

  // Keyboard shortcuts when pane is focused
  useEffect(() => {
    if (!isFocused) return;
    const onKeyDown = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') return;

      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a') {
        e.preventDefault();
        setSelectedPaths(shownItems.map((i) => i.path));
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'c' && selectedPaths.length > 0) {
        e.preventDefault();
        const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
        onSetClipboard({ op: 'copy', fromTarget: target, paths: selectedPaths, names });
        notify(`Copied ${selectedPaths.length} item(s) from ${target === 'phone' ? 'Z:\\' : 'C:\\'}`, 'info');
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'x' && selectedPaths.length > 0) {
        e.preventDefault();
        const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
        onSetClipboard({ op: 'move', fromTarget: target, paths: selectedPaths, names });
        notify(`Cut ${selectedPaths.length} item(s) — press Ctrl+V on destination drive to move`, 'info');
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'v' && clipboard) {
        e.preventDefault();
        void onPasteInto(target, currentDir);
      } else if (e.key === 'Delete' && selectedPaths.length > 0) {
        e.preventDefault();
        void handleDelete(selectedPaths);
      } else if (e.key === 'F2' && selectedPaths.length === 1) {
        e.preventDefault();
        const item = items.find((i) => i.path === selectedPaths[0]);
        if (item) {
          setRenamingPath(item.path);
          setRenameValue(item.name);
        }
      } else if (e.key === 'Enter' && selectedPaths.length >= 1) {
        // Explorer: Enter opens the focused item - a folder drills in, a file opens.
        e.preventDefault();
        const item = shownItems.find((i) => i.path === selectedPaths[0]);
        if (item) handleDoubleClick(item);
      } else if ((e.key === 'Backspace' || (e.altKey && e.key === 'ArrowUp')) && currentDir !== '/') {
        e.preventDefault();
        const parts = currentDir.split('/').filter(Boolean);
        parts.pop();
        onNavigate(parts.length === 0 ? '/' : '/' + parts.join('/'));
      } else if (e.key === 'F5') {
        e.preventDefault();
        void loadDirectory();
      } else if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key.toLowerCase() === 'n') {
        e.preventDefault();
        setNewFolderName('');
        setCreatingFolder(true);
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'e') {
        // Explorer's "jump to the search box" - focus this pane and its filter.
        e.preventDefault();
        onFocusPane();
        const box = document.getElementById(`ci-search-${target}`) as HTMLInputElement | null;
        box?.focus();
      } else if (e.key === 'Escape') {
        setCtxMenu(null);
        setSelectedPaths([]);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [
    isFocused,
    items,
    shownItems,
    selectedPaths,
    clipboard,
    target,
    currentDir,
    onSetClipboard,
    onPasteInto,
    onNavigate,
    onFocusPane,
    loadDirectory,
    notify,
  ]);

  const handleSelectItem = (item: VfsItem, e: React.MouseEvent) => {
    onFocusPane();
    if (e.ctrlKey || e.metaKey) {
      setSelectedPaths((prev) =>
        prev.includes(item.path) ? prev.filter((p) => p !== item.path) : [...prev, item.path]
      );
    } else {
      setSelectedPaths([item.path]);
    }
  };

  const handleDoubleClick = (item: VfsItem) => {
    if (item.kind === 'directory') {
      onNavigate(item.path);
    } else {
      onOpenFile(target, item);
    }
  };

  const goUp = () => {
    if (currentDir === '/') return;
    const parts = currentDir.split('/').filter(Boolean);
    parts.pop();
    onNavigate(parts.length === 0 ? '/' : '/' + parts.join('/'));
  };

  const handleCreateFolder = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!newFolderName.trim()) return;
    try {
      await api.mkdir(target, currentDir, newFolderName.trim());
      setNewFolderName('');
      setCreatingFolder(false);
      await loadDirectory();
      notify(`Created folder "${newFolderName.trim()}"`, 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Failed to create folder', 'error');
    }
  };

  const handleRenameSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!renamingPath || !renameValue.trim()) return;
    try {
      await api.rename(target, renamingPath, renameValue.trim());
      setRenamingPath(null);
      setRenameValue('');
      await loadDirectory();
      notify('Renamed successfully', 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Rename failed', 'error');
    }
  };

  const handleDelete = async (pathsToDelete: string[]) => {
    if (pathsToDelete.length === 0) return;
    try {
      await api.deleteItems(target, pathsToDelete);
      setSelectedPaths((prev) => prev.filter((p) => !pathsToDelete.includes(p)));
      await loadDirectory();
      notify(`Deleted ${pathsToDelete.length} item(s)`, 'info');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Delete failed', 'error');
    }
  };

  const handleExternalFilesUpload = async (fileList: FileList | null, destDir: string) => {
    if (!fileList || fileList.length === 0) return;
    const files = Array.from(fileList);
    try {
      for (const file of files) {
        await api.uploadFile(target, destDir, file);
      }
      await loadDirectory();
      notify(
        `Dropped ${files.length} file(s) onto ${toWindowsDisplayPath(target, destDir)}`,
        'success'
      );
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Upload failed', 'error');
    }
  };

  const handleDragStartItem = (e: React.DragEvent, item: VfsItem) => {
    const dragPaths = selectedPaths.includes(item.path) ? selectedPaths : [item.path];
    if (!selectedPaths.includes(item.path)) {
      setSelectedPaths([item.path]);
    }
    e.dataTransfer.effectAllowed = 'copyMove';
    e.dataTransfer.setData(
      'application/x-chainit-items',
      JSON.stringify({ fromTarget: target, paths: dragPaths })
    );
  };

  const handleDropOnDir = async (e: React.DragEvent, destDir: string) => {
    e.preventDefault();
    e.stopPropagation();
    setIsDragOverPane(false);
    setDragOverFolderPath(null);

    const customPayload = e.dataTransfer.getData('application/x-chainit-items');
    if (customPayload) {
      try {
        const parsed = JSON.parse(customPayload) as {
          fromTarget: DriveTarget;
          paths: string[];
        };
        if (parsed.fromTarget === target && destDir === currentDir) {
          return; // Dropped back into same folder
        }
        const op: 'copy' | 'move' = e.shiftKey ? 'move' : 'copy';
        await onDirectTransfer({
          op,
          fromTarget: parsed.fromTarget,
          fromPaths: parsed.paths,
          toTarget: target,
          toDir: destDir,
        });
      } catch (err) {
        notify(err instanceof Error ? err.message : 'Drop failed', 'error');
      }
      return;
    }

    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      await handleExternalFilesUpload(e.dataTransfer.files, destDir);
    }
  };

  const toggleSortKey = (key: SortState['key']) =>
    onSortChange(
      sort.key === key ? { ...sort, desc: !sort.desc } : { ...sort, key, desc: false }
    );

  const segments = currentDir.split('/').filter(Boolean);
  const shortcuts = target === 'phone' ? PHONE_SHORTCUTS : PC_SHORTCUTS;
  const isPhone = target === 'phone';
  const otherLabel = otherTarget === 'phone' ? 'Z:\\' : 'C:\\';

  const renderItemIcon = (item: VfsItem) => {
    if (item.kind === 'directory') {
      return (
        <Folder
          className={`w-4 h-4 shrink-0 ${
            isPhone ? 'text-emerald-400 fill-emerald-500/20' : 'text-amber-400 fill-amber-500/20'
          }`}
        />
      );
    }
    if (isImageMime(item.mimeType, item.ext)) {
      return <ImageIcon className="w-4 h-4 shrink-0 text-purple-400" />;
    }
    return <FileText className="w-4 h-4 shrink-0 text-cyan-400" />;
  };

  return (
    <div
      onClick={onFocusPane}
      onDragOver={(e) => {
        e.preventDefault();
        setIsDragOverPane(true);
      }}
      onDragLeave={(e) => {
        if (e.currentTarget === e.target) setIsDragOverPane(false);
      }}
      onDrop={(e) => void handleDropOnDir(e, currentDir)}
      className={`ci-card-flush relative flex flex-col transition-all min-h-[540px] ${
        isDragOverPane
          ? 'border-[var(--ci-accent)] ring-2 ring-[rgba(16,185,129,0.28)]'
          : isFocused
            ? isPhone
              ? 'border-[rgba(16,185,129,0.45)] shadow-[var(--ci-shadow)]'
              : 'border-[rgba(34,211,238,0.42)] shadow-[var(--ci-shadow)]'
            : ''
      }`}
    >
      {/* Drive Header */}
      <div className="ci-card-head flex-wrap">
        <div className="flex items-center gap-2.5 min-w-0">
          <div
            className={`p-2 rounded-xl border ${
              isPhone
                ? 'bg-emerald-500/10 border-emerald-500/30 text-emerald-400'
                : 'bg-cyan-500/10 border-cyan-500/30 text-cyan-400'
            }`}
          >
            {isPhone ? <Smartphone className="w-4 h-4" /> : <HardDrive className="w-4 h-4" />}
          </div>
          <div className="min-w-0">
            <div className="flex items-center gap-2">
              <span className="ci-pill-off font-mono normal-case tracking-normal text-[11px]">
                {stats.driveLetter}\
              </span>
              <h2 className="ci-h2 truncate">{stats.label}</h2>
            </div>
            <p className="ci-muted">
              {stats.folderCount} folders • {stats.fileCount} files •{' '}
              {isPhone ? 'Mounted via Sibling VFS (/dav)' : 'Local NVMe Workspace'}
            </p>
          </div>
        </div>

        {/* Screen-off badge on Phone Drive */}
        {isPhone && snapshot.sibling.zDriveMounted && (
          <div className="ci-pill-ok">
            {snapshot.environment.phoneScreenLocked ? (
              <>
                <Lock className="w-3 h-3" />
                <span>Screen OFF • Zero-Touch Active</span>
              </>
            ) : (
              <>
                <ShieldCheck className="w-3.5 h-3.5" />
                <span>Sibling Auto-Mounted</span>
              </>
            )}
          </div>
        )}
      </div>

      {/* Windows-style Path Bar & Search */}
      <div className="flex flex-wrap items-center gap-2 px-3 py-2.5 border-b border-[var(--ci-border-soft)] bg-[var(--ci-surface)]">
        <button
          onClick={goUp}
          disabled={currentDir === '/'}
          className="ci-btn-icon border border-[var(--ci-border-soft)] bg-[var(--ci-bg-soft)] disabled:opacity-40"
          title="Up one folder"
        >
          <ArrowUp className="w-3.5 h-3.5" />
        </button>
        <button
          onClick={() => void loadDirectory()}
          className="ci-btn-icon border border-[var(--ci-border-soft)] bg-[var(--ci-bg-soft)]"
          title="Refresh directory"
        >
          <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
        </button>

        {/* Breadcrumbs */}
        <div className="ci-scroll flex-1 min-w-[180px] flex items-center gap-1 px-2 py-1 rounded-xl bg-[var(--ci-bg-soft)] border border-[var(--ci-border)] overflow-x-auto text-xs font-mono min-h-[34px]">
          <button
            onClick={() => onNavigate('/')}
            className="ci-crumb shrink-0" data-active={false}
          >
            {stats.driveLetter}\
          </button>
          {segments.map((seg, idx) => {
            const targetPath = '/' + segments.slice(0, idx + 1).join('/');
            return (
              <React.Fragment key={targetPath}>
                <span className="text-slate-600">\</span>
                <button
                  onClick={() => onNavigate(targetPath)}
                  className="ci-crumb truncate max-w-[150px]"
                >
                  {seg}
                </button>
              </React.Fragment>
            );
          })}
        </div>

        {/* Search + View Toggle */}
        <div className="relative w-32 sm:w-40">
          <Search className="w-3.5 h-3.5 text-slate-500 absolute left-2.5 top-1/2 -translate-y-1/2" />
          <input
            id={`ci-search-${target}`}
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder="Filter..."
            className="ci-input pl-8 text-xs min-h-[34px] py-1.5"
          />
        </div>

      </div>

      {/* Quick Folder Chips */}
      <div className="flex items-center gap-1.5 px-3 py-1.5 bg-slate-950/40 border-b border-slate-800/60 overflow-x-auto">
        {shortcuts.map((sc) => {
          const active = currentDir === sc.path;
          return (
            <button
              key={sc.path}
              onClick={() => onNavigate(sc.path)}
              className={`px-2 py-0.5 rounded-md text-[11px] font-medium shrink-0 transition ${
                active
                  ? isPhone
                    ? 'bg-emerald-500/20 text-emerald-300 border border-emerald-500/40'
                    : 'bg-cyan-500/20 text-cyan-300 border border-cyan-500/40'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/70'
              }`}
            >
              {sc.label}
            </button>
          );
        })}
      </div>

      {/* Action Toolbar — icon-first with tooltips */}
      <div className="flex flex-wrap items-center justify-between gap-2 px-3 py-2 border-b border-[var(--ci-border-soft)] bg-[rgba(10,15,25,0.6)]">
        <div className="flex flex-wrap items-center gap-2">
          <div className="ci-tool-group">
            <button
              type="button"
              className="ci-btn-icon"
              title="New folder (Ctrl+Shift+N)"
              onClick={() => {
                setCreatingFolder(true);
                setNewFolderName('');
              }}
            >
              <FolderPlus className="w-4 h-4 text-[var(--ci-amber)]" />
            </button>

            <input
              ref={fileInputRef}
              type="file"
              multiple
              className="hidden"
              onChange={(e) => {
                void handleExternalFilesUpload(e.target.files, currentDir);
                e.target.value = '';
              }}
            />
            <button
              type="button"
              className="ci-btn-icon"
              title="Upload files from this computer (or drop them on a folder)"
              onClick={() => fileInputRef.current?.click()}
            >
              <Upload className="w-4 h-4 text-[var(--ci-info)]" />
            </button>

            <span className="ci-vdiv" />

            <button
              type="button"
              className="ci-btn-icon"
              disabled={selectedPaths.length === 0}
              title="Copy (Ctrl+C)"
              onClick={() => {
                const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
                onSetClipboard({ op: 'copy', fromTarget: target, paths: selectedPaths, names });
                notify(`Copied ${selectedPaths.length} item(s)`, 'info');
              }}
              style={selectedPaths.length === 0 ? { opacity: 0.35 } : undefined}
            >
              <Copy className="w-4 h-4" />
            </button>

            <button
              type="button"
              className="ci-btn-icon"
              disabled={selectedPaths.length === 0}
              title="Cut / move (Ctrl+X)"
              onClick={() => {
                const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
                onSetClipboard({ op: 'move', fromTarget: target, paths: selectedPaths, names });
                notify(`Cut ${selectedPaths.length} item(s)`, 'info');
              }}
              style={selectedPaths.length === 0 ? { opacity: 0.35 } : undefined}
            >
              <Scissors className="w-4 h-4" />
            </button>

            <button
              type="button"
              className="ci-btn-icon"
              disabled={!clipboard}
              title={`Paste here (Ctrl+V)${clipboard ? ` — ${clipboard.paths.length} item(s)` : ''}`}
              onClick={() => void onPasteInto(target, currentDir)}
              data-active={Boolean(clipboard)}
              style={!clipboard ? { opacity: 0.35 } : undefined}
            >
              <Clipboard className={`w-4 h-4 ${clipboard ? 'text-[var(--ci-accent)]' : ''}`} />
            </button>

            <button
              type="button"
              className="ci-btn-icon"
              disabled={selectedPaths.length !== 1}
              title="Rename (F2)"
              onClick={() => {
                const picked = items.find((i) => i.path === selectedPaths[0]);
                if (picked) {
                  setRenamingPath(picked.path);
                  setRenameValue(picked.name);
                }
              }}
              style={selectedPaths.length !== 1 ? { opacity: 0.35 } : undefined}
            >
              <Edit3 className="w-4 h-4" />
            </button>

            <button
              type="button"
              className="ci-btn-icon hover:!text-[var(--ci-danger)]"
              disabled={selectedPaths.length === 0}
              title="Delete selected (Del)"
              onClick={() => void handleDelete(selectedPaths)}
              style={selectedPaths.length === 0 ? { opacity: 0.35 } : undefined}
            >
              <Trash2 className="w-4 h-4" />
            </button>
          </div>

          <div className="ci-tool-group">
            <button
              type="button"
              className="ci-btn-icon"
              disabled={selectedPaths.length === 0}
              title={`Copy selected to ${otherLabel}${otherCurrentDir.replace(/\//g, '\\')}`}
              onClick={() =>
                void onDirectTransfer({
                  op: 'copy',
                  fromTarget: target,
                  fromPaths: selectedPaths,
                  toTarget: otherTarget,
                  toDir: otherCurrentDir,
                })
              }
              style={selectedPaths.length === 0 ? { opacity: 0.35 } : undefined}
            >
              <ArrowLeftRight className={`w-4 h-4 ${isPhone ? 'text-[var(--ci-info)]' : 'text-[var(--ci-accent)]'}`} />
            </button>
            <button
              type="button"
              className="ci-btn-icon"
              disabled={selectedPaths.length === 0}
              title={`Move selected to ${otherLabel}`}
              onClick={() =>
                void onDirectTransfer({
                  op: 'move',
                  fromTarget: target,
                  fromPaths: selectedPaths,
                  toTarget: otherTarget,
                  toDir: otherCurrentDir,
                })
              }
              style={selectedPaths.length === 0 ? { opacity: 0.35 } : undefined}
            >
              <MoveRight className="w-4 h-4" />
            </button>
          </div>

          {selectedPaths.length > 0 && (
            <span className="ci-chip" data-on>
              <Check className="w-3 h-3" />
              {selectedPaths.length} selected
            </span>
          )}
        </div>

        <div className="flex items-center gap-2">
          <SortMenu value={sort} onChange={onSortChange} />

          <div className="ci-tool-group">
            <button
              type="button"
              className="ci-btn-icon"
              data-active={viewMode === 'list'}
              title="List view"
              onClick={() => setViewMode('list')}
            >
              <ListIcon className="w-4 h-4" />
            </button>
            <button
              type="button"
              className="ci-btn-icon"
              data-active={viewMode === 'grid'}
              title="Grid view"
              onClick={() => setViewMode('grid')}
            >
              <LayoutGrid className="w-4 h-4" />
            </button>
          </div>

          <button
            type="button"
            className="ci-btn-icon"
            title="Refresh this folder (F5)"
            onClick={() => void loadDirectory()}
          >
            <RefreshCw className={`w-4 h-4 ${loading ? 'animate-spin' : ''}`} />
          </button>
        </div>
      </div>

      {/* Inline New Folder Bar */}
      {creatingFolder && (
        <form
          onSubmit={handleCreateFolder}
          className="flex items-center gap-2 px-3 py-2 bg-slate-800/80 border-b border-slate-700"
        >
          <Folder className="w-4 h-4 text-amber-400 shrink-0" />
          <input
            autoFocus
            value={newFolderName}
            onChange={(e) => setNewFolderName(e.target.value)}
            placeholder="New folder name..."
            className="flex-1 px-2.5 py-1 rounded bg-slate-950 border border-slate-700 text-xs text-slate-100 focus:outline-none focus:border-cyan-400"
          />
          <button
            type="submit"
            className="px-2.5 py-1 rounded bg-emerald-500 text-slate-950 font-semibold text-xs"
          >
            Create
          </button>
          <button
            type="button"
            onClick={() => setCreatingFolder(false)}
            className="p-1 text-slate-400 hover:text-slate-200"
          >
            <X className="w-3.5 h-3.5" />
          </button>
        </form>
      )}

      {/* Main Directory Listing */}
      <div
        className="flex-1 overflow-y-auto p-2 select-none"
        onContextMenu={(e) => {
          e.preventDefault();
          setCtxMenu({ x: e.clientX, y: e.clientY, item: null });
        }}
      >
        {unavailableError ? (
          <div className="h-full flex flex-col items-center justify-center text-center p-6 space-y-3">
            <div className="p-3 rounded-2xl bg-amber-500/10 border border-amber-500/30 text-amber-300">
              <Lock className="w-6 h-6" />
            </div>
            <h3 className="text-sm font-semibold text-slate-100">Z:\ Sibling VFS Paused</h3>
            <p className="text-xs text-slate-400 max-w-sm leading-relaxed">{unavailableError}</p>
            <button
              onClick={onQuickEnableSibling}
              className="ci-btn-primary"
            >
              Enable Sibling Mode & Restore Zero-Touch Z:\
            </button>
          </div>
        ) : shownItems.length === 0 ? (
          <div className="h-64 flex flex-col items-center justify-center gap-3 px-6 text-center">
            <span className="ci-logo-mark w-14 h-14 opacity-70">
              <Folder className="w-6 h-6 text-[var(--ci-text-3)]" />
            </span>
            <div>
              <p className="ci-h2">This folder is empty</p>
              <p className="ci-muted mt-1">Drag &amp; drop files here, or press Ctrl+V to paste from the other drive.</p>
            </div>
          </div>
        ) : viewMode === 'list' ? (
          <div className="space-y-0.5">
            <div className="ci-section-label grid grid-cols-12 px-2.5 py-1.5 border-b border-[var(--ci-border-soft)]">
              <div className="col-span-6">
                <button type="button" className="ci-th" data-active={sort.key === 'name'} onClick={() => toggleSortKey('name')} title="Sort by name">
                  <span>Name</span>
                  {sort.key === 'name' && (sort.desc ? <ArrowDownIcon className="w-3 h-3" /> : <ArrowUpIcon className="w-3 h-3" />)}
                </button>
              </div>
              <div className="col-span-2 flex justify-end">
                <button type="button" className="ci-th" data-active={sort.key === 'size'} onClick={() => toggleSortKey('size')} title="Sort by size">
                  <span>Size</span>
                  {sort.key === 'size' && (sort.desc ? <ArrowDownIcon className="w-3 h-3" /> : <ArrowUpIcon className="w-3 h-3" />)}
                </button>
              </div>
              <div className="col-span-2 flex justify-end pr-1">
                <button type="button" className="ci-th" data-active={sort.key === 'type'} onClick={() => toggleSortKey('type')} title="Sort by extension / type">
                  <span>Type</span>
                  {sort.key === 'type' && (sort.desc ? <ArrowDownIcon className="w-3 h-3" /> : <ArrowUpIcon className="w-3 h-3" />)}
                </button>
              </div>
              <div className="col-span-2 flex justify-end pr-1">
                <button type="button" className="ci-th" data-active={sort.key === 'modified'} onClick={() => toggleSortKey('modified')} title="Sort by date modified">
                  <span>Modified</span>
                  {sort.key === 'modified' && (sort.desc ? <ArrowDownIcon className="w-3 h-3" /> : <ArrowUpIcon className="w-3 h-3" />)}
                </button>
              </div>
            </div>
            {shownItems.map((item) => {
              const selected = selectedPaths.includes(item.path);
              const isDropFolder = dragOverFolderPath === item.path;
              const isCut =
                clipboard?.op === 'move' &&
                clipboard.fromTarget === target &&
                clipboard.paths.includes(item.path);

              return (
                <div
                  key={item.path}
                  draggable={renamingPath !== item.path}
                  onDragStart={(e) => handleDragStartItem(e, item)}
                  onDragOver={(e) => {
                    if (item.kind === 'directory') {
                      e.preventDefault();
                      e.stopPropagation();
                      setDragOverFolderPath(item.path);
                    }
                  }}
                  onDragLeave={() => {
                    if (dragOverFolderPath === item.path) setDragOverFolderPath(null);
                  }}
                  onDrop={(e) => {
                    if (item.kind === 'directory') {
                      void handleDropOnDir(e, item.path);
                    }
                  }}
                  onClick={(e) => handleSelectItem(item, e)}
                  onDoubleClick={() => handleDoubleClick(item)}
                  onContextMenu={(e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    if (!selectedPaths.includes(item.path)) {
                      setSelectedPaths([item.path]);
                    }
                    setCtxMenu({ x: e.clientX, y: e.clientY, item });
                  }}
                  className={`ci-file-row grid grid-cols-12 items-center px-2.5 py-1.5 text-xs ${
                    isDropFolder
                      ? 'bg-[rgba(16,185,129,0.22)] ring-1 ring-[var(--ci-accent)]'
                      : selected
                        ? isPhone
                          ? 'bg-[rgba(16,185,129,0.14)] text-emerald-50 ring-1 ring-[rgba(16,185,129,0.35)]'
                          : 'bg-[rgba(34,211,238,0.12)] text-cyan-50 ring-1 ring-[rgba(34,211,238,0.32)]'
                        : 'text-[var(--ci-text-2)]'
                  } ${isCut ? 'opacity-50' : ''}`}
                >
                  <div className="col-span-6 flex items-center gap-2 min-w-0 pr-2">
                    {renderItemIcon(item)}
                    {renamingPath === item.path ? (
                      <form onSubmit={handleRenameSubmit} className="flex items-center gap-1 flex-1">
                        <input
                          autoFocus
                          value={renameValue}
                          onChange={(e) => setRenameValue(e.target.value)}
                          onClick={(e) => e.stopPropagation()}
                          className="w-full px-1.5 py-0.5 rounded bg-slate-950 border border-cyan-500 text-xs text-slate-100"
                        />
                        <button type="submit" className="text-emerald-400">
                          <Check className="w-3.5 h-3.5" />
                        </button>
                      </form>
                    ) : (
                      <span className="truncate font-medium">{item.name}</span>
                    )}
                  </div>
                  <div className="col-span-2 text-right font-mono text-[11px] text-slate-400">
                    {item.kind === 'directory' ? '—' : formatBytes(item.sizeBytes)}
                  </div>
                  <div className="col-span-2 text-right text-[11px] text-slate-500 truncate pl-2">
                    {item.kind === 'directory' ? 'Folder' : item.ext.replace('.', '').toUpperCase() || 'FILE'}
                  </div>
                  <div className="col-span-2 text-right text-[11px] text-slate-500 truncate pl-2" title={new Date(item.modifiedMs).toLocaleString()}>
                    {item.kind === 'directory' ? '—' : new Date(item.modifiedMs).toLocaleDateString() + ' ' + new Date(item.modifiedMs).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
                  </div>
                </div>
              );
            })}
          </div>
        ) : (
          <div className="grid grid-cols-2 sm:grid-cols-3 gap-2 p-1">
            {shownItems.map((item) => {
              const selected = selectedPaths.includes(item.path);
              const isImg = item.kind === 'file' && isImageMime(item.mimeType, item.ext);
              return (
                <div
                  key={item.path}
                  draggable
                  onDragStart={(e) => handleDragStartItem(e, item)}
                  onClick={(e) => handleSelectItem(item, e)}
                  onDoubleClick={() => handleDoubleClick(item)}
                  onContextMenu={(e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    setSelectedPaths([item.path]);
                    setCtxMenu({ x: e.clientX, y: e.clientY, item });
                  }}
                  className={`flex flex-col items-center p-3 rounded-xl border text-center cursor-pointer transition ${
                    selected
                      ? 'bg-cyan-500/15 border-cyan-500/50'
                      : 'bg-slate-950/50 border-slate-800/80 hover:border-slate-700'
                  }`}
                >
                  {isImg ? (
                    <img
                      src={api.rawUrl(target, item.path)}
                      alt={item.name}
                      className="w-full h-20 object-cover rounded-lg mb-2 bg-slate-900"
                    />
                  ) : (
                    <div className="h-20 flex items-center justify-center mb-2">
                      {item.kind === 'directory' ? (
                        <Folder className="w-10 h-10 text-amber-400 fill-amber-500/20" />
                      ) : (
                        <FileText className="w-9 h-9 text-cyan-400" />
                      )}
                    </div>
                  )}
                  <span className="text-xs font-medium text-slate-200 truncate w-full">
                    {item.name}
                  </span>
                  <span className="text-[10px] text-slate-500 mt-0.5">
                    {item.kind === 'directory' ? 'Folder' : formatBytes(item.sizeBytes)}
                  </span>
                </div>
              );
            })}
          </div>
        )}
      </div>

      {/* Footer Status */}
      <div className="flex items-center justify-between px-3.5 py-1.5 bg-slate-950/80 border-t border-slate-800/80 text-[11px] text-slate-400">
        <span className="flex items-center gap-2">
          <span>
            {shownItems.length} item(s)
            {selectedPaths.length > 0 ? ` • ${selectedPaths.length} selected (${formatBytes(selectedBytes)})` : ''}
          </span>
          <span className="ci-chip" title="Active sorting">
            {SORT_LABELS[sort.key]} {sort.desc ? '↓' : '↑'}{sort.foldersFirst ? ' · folders first' : ''}
          </span>
        </span>
        <span className="text-slate-500 hidden sm:inline">
          Drag between C:\ & Z:\ • Enter open • F2 rename • Del delete • F5 refresh •
          Backspace up • Ctrl+A/C/X/V • Ctrl+Shift+N new folder • Ctrl+E filter
        </span>
      </div>

      {/* Right-Click Context Menu */}
      {ctxMenu && (
        <div
          style={{
            position: 'fixed',
            top: Math.min(ctxMenu.y, window.innerHeight - 260),
            left: Math.min(ctxMenu.x, window.innerWidth - 220),
          }}
          className="z-50 w-52 rounded-xl border border-slate-700 bg-slate-900/95 backdrop-blur-md shadow-2xl py-1.5 text-xs text-slate-200"
          onClick={(e) => e.stopPropagation()}
        >
          {ctxMenu.item && (
            <>
              <button
                onClick={() => {
                  const it = ctxMenu.item!;
                  setCtxMenu(null);
                  handleDoubleClick(it);
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left font-medium"
              >
                <Edit3 className="w-3.5 h-3.5 text-cyan-400" />
                {ctxMenu.item.kind === 'directory' ? 'Open Folder' : 'Preview / Edit in Place'}
              </button>
              <button
                onClick={() => {
                  setCtxMenu(null);
                  void onDirectTransfer({
                    op: 'copy',
                    fromTarget: target,
                    fromPaths: selectedPaths,
                    toTarget: otherTarget,
                    toDir: otherCurrentDir,
                  });
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
              >
                <ArrowRight className="w-3.5 h-3.5 text-emerald-400" />
                Copy to {otherLabel} ({otherCurrentDir})
              </button>
              <button
                onClick={() => {
                  setCtxMenu(null);
                  void onDirectTransfer({
                    op: 'move',
                    fromTarget: target,
                    fromPaths: selectedPaths,
                    toTarget: otherTarget,
                    toDir: otherCurrentDir,
                  });
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
              >
                <MoveRight className="w-3.5 h-3.5 text-amber-400" />
                Move to {otherLabel}
              </button>
              {ctxMenu.item!.kind === 'file' && (
                <button
                  onClick={() => {
                    const it = ctxMenu.item!;
                    setCtxMenu(null);
                    const a = document.createElement('a');
                    a.href = api.rawUrl(target, it.path, true);
                    a.download = it.name;
                    a.click();
                  }}
                  className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
                >
                  <Download className="w-3.5 h-3.5 text-cyan-400" />
                  Download to this PC
                </button>
              )}
              <div className="my-1 border-t border-slate-800" />
              <button
                onClick={() => {
                  const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
                  onSetClipboard({ op: 'copy', fromTarget: target, paths: selectedPaths, names });
                  setCtxMenu(null);
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
              >
                <Copy className="w-3.5 h-3.5" /> Copy (Ctrl+C)
              </button>
              <button
                onClick={() => {
                  const names = items.filter((i) => selectedPaths.includes(i.path)).map((i) => i.name);
                  onSetClipboard({ op: 'move', fromTarget: target, paths: selectedPaths, names });
                  setCtxMenu(null);
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
              >
                <Scissors className="w-3.5 h-3.5" /> Cut (Ctrl+X)
              </button>
              {ctxMenu.item.kind === 'file' && (
                <a
                  href={api.rawUrl(target, ctxMenu.item.path, true)}
                  download={ctxMenu.item.name}
                  onClick={() => setCtxMenu(null)}
                  className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
                >
                  <Download className="w-3.5 h-3.5" /> Download to Computer
                </a>
              )}
              <button
                onClick={() => {
                  const it = ctxMenu.item!;
                  setRenamingPath(it.path);
                  setRenameValue(it.name);
                  setCtxMenu(null);
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-left"
              >
                <Edit3 className="w-3.5 h-3.5" /> Rename (F2)
              </button>
              <button
                onClick={() => {
                  setCtxMenu(null);
                  void handleDelete(selectedPaths);
                }}
                className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-rose-500/20 text-rose-300 text-left"
              >
                <Trash2 className="w-3.5 h-3.5" /> Delete
              </button>
            </>
          )}

          {clipboard && (
            <button
              onClick={() => {
                setCtxMenu(null);
                void onPasteInto(target, currentDir);
              }}
              className="w-full flex items-center gap-2 px-3 py-1.5 hover:bg-slate-800 text-emerald-300 text-left"
            >
              <Clipboard className="w-3.5 h-3.5" /> Paste Here ({clipboard.paths.length})
            </button>
          )}
        </div>
      )}
    </div>
  );
};
