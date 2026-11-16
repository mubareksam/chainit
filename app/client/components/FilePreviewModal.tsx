import React, { useEffect, useState } from 'react';
import {
  ArrowRightLeft,
  Check,
  Download,
  FileCode2,
  Image as ImageIcon,
  Lock,
  Save,
  Smartphone,
  Laptop,
  X,
} from 'lucide-react';
import { DriveTarget, VfsItem } from '../../shared/types';
import { api } from '../api/client';
import { formatBytes, isImageMime, toWindowsDisplayPath } from '../utils/format';

interface Props {
  target: DriveTarget;
  item: VfsItem;
  phoneScreenLocked: boolean;
  onClose: () => void;
  onSaved: () => void;
  onCopyToOtherDrive: (target: DriveTarget, item: VfsItem) => void;
  notify: (msg: string, kind?: 'success' | 'error' | 'info') => void;
}

export const FilePreviewModal: React.FC<Props> = ({
  target,
  item,
  phoneScreenLocked,
  onClose,
  onSaved,
  onCopyToOtherDrive,
  notify,
}) => {
  const [loading, setLoading] = useState(true);
  const [editable, setEditable] = useState(false);
  const [content, setContent] = useState<string>('');
  const [dirty, setDirty] = useState(false);
  const [saving, setSaving] = useState(false);
  const [viewSourceForSvg, setViewSourceForSvg] = useState(false);

  const isImg = isImageMime(item.mimeType, item.ext);
  const winPath = toWindowsDisplayPath(target, item.path);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api
      .readText(target, item.path)
      .then((res) => {
        if (cancelled) return;
        setEditable(res.editable);
        setContent(res.content ?? '');
        setDirty(false);
      })
      .catch((err) => {
        if (!cancelled) notify(err.message, 'error');
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [target, item.path, notify]);

  const handleSave = async () => {
    if (!editable) return;
    setSaving(true);
    try {
      await api.saveText(target, item.path, content);
      setDirty(false);
      onSaved();
      notify(
        `Saved directly to ${target === 'phone' ? 'Z:\\ (Phone)' : 'C:\\ (PC)'} with zero prompts!`,
        'success'
      );
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Save failed', 'error');
    } finally {
      setSaving(false);
    }
  };

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/80 backdrop-blur-sm p-4"
      onClick={onClose}
    >
      <div
        className="w-full max-w-4xl rounded-2xl border border-slate-800 bg-slate-900 shadow-2xl overflow-hidden flex flex-col max-h-[88vh]"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between gap-3 px-5 py-3.5 border-b border-slate-800 bg-slate-950/60">
          <div className="flex items-center gap-3 min-w-0">
            <div
              className={`p-2 rounded-lg ${
                target === 'phone'
                  ? 'bg-emerald-500/15 text-emerald-400'
                  : 'bg-cyan-500/15 text-cyan-400'
              }`}
            >
              {target === 'phone' ? <Smartphone className="w-4 h-4" /> : <Laptop className="w-4 h-4" />}
            </div>
            <div className="min-w-0">
              <div className="flex items-center gap-2">
                <h3 className="text-sm font-semibold text-slate-100 truncate">{item.name}</h3>
                <span className="text-xs px-2 py-0.5 rounded bg-slate-800 text-slate-400 font-mono">
                  {formatBytes(item.sizeBytes)}
                </span>
                {target === 'phone' && phoneScreenLocked && (
                  <span className="hidden sm:inline-flex items-center gap-1 text-[11px] px-2 py-0.5 rounded-full bg-emerald-500/10 border border-emerald-500/30 text-emerald-300">
                    <Lock className="w-3 h-3" /> Opened while phone screen is OFF
                  </span>
                )}
              </div>
              <p className="text-xs font-mono text-slate-400 truncate">{winPath}</p>
            </div>
          </div>

          <div className="flex items-center gap-2 shrink-0">
            {item.ext === '.svg' && (
              <button
                onClick={() => setViewSourceForSvg((v) => !v)}
                className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-200 transition"
              >
                {viewSourceForSvg ? <ImageIcon className="w-3.5 h-3.5" /> : <FileCode2 className="w-3.5 h-3.5" />}
                {viewSourceForSvg ? 'Image Preview' : 'Edit SVG XML'}
              </button>
            )}

            <button
              onClick={() => onCopyToOtherDrive(target, item)}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-200 transition"
            >
              <ArrowRightLeft className="w-3.5 h-3.5 text-cyan-400" />
              Copy to {target === 'phone' ? 'C:\\' : 'Z:\\'}
            </button>

            <a
              href={api.rawUrl(target, item.path, true)}
              download={item.name}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-200 transition"
            >
              <Download className="w-3.5 h-3.5" />
              Download
            </a>

            {editable && (!isImg || viewSourceForSvg) && (
              <button
                onClick={handleSave}
                disabled={saving || !dirty}
                className={`inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-semibold transition ${
                  dirty
                    ? 'bg-emerald-500 hover:bg-emerald-400 text-slate-950'
                    : 'bg-slate-800 text-slate-500 cursor-not-allowed'
                }`}
              >
                {saving ? <Check className="w-3.5 h-3.5" /> : <Save className="w-3.5 h-3.5" />}
                {saving ? 'Saving...' : `Save to ${target === 'phone' ? 'Z:\\' : 'C:\\'}`}
              </button>
            )}

            <button
              onClick={onClose}
              className="p-1.5 rounded-lg text-slate-400 hover:text-slate-100 hover:bg-slate-800 transition"
              title="Close"
            >
              <X className="w-4 h-4" />
            </button>
          </div>
        </div>

        {/* Content */}
        <div className="flex-1 overflow-auto p-5 bg-slate-950/40">
          {loading ? (
            <div className="py-20 text-center text-sm text-slate-400">
              Streaming file from {winPath}...
            </div>
          ) : isImg && !viewSourceForSvg ? (
            <div className="flex flex-col items-center justify-center gap-3">
              <img
                src={`${api.rawUrl(target, item.path)}&t=${item.modifiedMs}`}
                alt={item.name}
                className="max-h-[64vh] w-auto rounded-xl border border-slate-800 shadow-lg bg-slate-950"
              />
              <p className="text-xs text-slate-400">
                Streamed directly via VFS ({winPath}) — no manual send/receive step required.
              </p>
            </div>
          ) : editable ? (
            <textarea
              value={content}
              onChange={(e) => {
                setContent(e.target.value);
                setDirty(true);
              }}
              onKeyDown={(e) => {
                if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') {
                  e.preventDefault();
                  void handleSave();
                }
              }}
              spellCheck={false}
              className="w-full h-[58vh] rounded-xl border border-slate-800 bg-slate-950 p-4 font-mono text-xs sm:text-sm text-slate-100 focus:outline-none focus:border-cyan-500/60 resize-none leading-relaxed"
            />
          ) : (
            <div className="py-16 text-center space-y-3">
              <p className="text-sm text-slate-300">Binary file ({item.mimeType})</p>
              <a
                href={api.rawUrl(target, item.path, true)}
                download={item.name}
                className="inline-flex items-center gap-2 px-4 py-2 rounded-lg bg-cyan-500 text-slate-950 font-semibold text-xs hover:bg-cyan-400 transition"
              >
                <Download className="w-4 h-4" /> Download {item.name}
              </a>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};
