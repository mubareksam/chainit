import React, { useEffect, useRef, useState } from 'react';
import {
  ArrowDown,
  ArrowDown01,
  ArrowDownAZ,
  ArrowUp,
  CalendarDays,
  Check,
  ChevronDown,
  FileType2,
  FolderClosed,
} from 'lucide-react';
import { SORT_LABELS, SortKey, SortState } from '../utils/sort';

interface Props {
  value: SortState;
  onChange: (next: SortState) => void;
}

const KEY_ICONS: Record<SortKey, React.FC<{ className?: string }>> = {
  name: ArrowDownAZ,
  size: ArrowDown01,
  modified: CalendarDays,
  type: FileType2,
};

/** Compact, icon-led sort control used by both drive panes. */
export const SortMenu: React.FC<Props> = ({ value, onChange }) => {
  const [open, setOpen] = useState(false);
  const wrapRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (wrapRef.current && !wrapRef.current.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false);
    };
    document.addEventListener('mousedown', onDown);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDown);
      document.removeEventListener('keydown', onKey);
    };
  }, [open]);

  return (
    <div className="ci-menu-wrap" ref={wrapRef}>
      <button
        type="button"
        className="ci-btn-quiet min-h-[30px] px-2.5 py-1 text-[11.5px]"
        onClick={() => setOpen((v) => !v)}
        title={`Sort by ${SORT_LABELS[value.key]} (${value.desc ? 'descending' : 'ascending'})`}
        aria-haspopup="menu"
        aria-expanded={open}
      >
        <ArrowDownAZ className="w-3.5 h-3.5 text-[var(--ci-text-3)]" />
        <span className="hidden md:inline">{SORT_LABELS[value.key]}</span>
        {value.desc ? (
          <ArrowDown className="w-3 h-3 text-[var(--ci-text-4)]" />
        ) : (
          <ArrowUp className="w-3 h-3 text-[var(--ci-text-4)]" />
        )}
        <ChevronDown className="w-3 h-3 text-[var(--ci-text-4)]" />
      </button>

      {open && (
        <div className="ci-menu" role="menu">
          <div className="ci-menu-label">Sort by</div>
          {(Object.keys(SORT_LABELS) as SortKey[]).map((key) => {
            const Icon = KEY_ICONS[key];
            return (
              <button
                key={key}
                type="button"
                role="menuitemradio"
                aria-checked={value.key === key}
                data-active={value.key === key}
                className="ci-menu-item"
                onClick={() => onChange({ ...value, key })}
              >
                <Icon className="w-3.5 h-3.5" />
                <span className="flex-1">{SORT_LABELS[key]}</span>
                {value.key === key && <Check className="w-3.5 h-3.5" />}
              </button>
            );
          })}

          <div className="ci-vdiv my-1 !mx-0 !w-full" style={{ height: 1 }} />

          <button
            type="button"
            role="menuitemcheckbox"
            aria-checked={value.desc}
            className="ci-menu-item"
            onClick={() => onChange({ ...value, desc: !value.desc })}
          >
            {value.desc ? (
              <ArrowDown className="w-3.5 h-3.5" />
            ) : (
              <ArrowUp className="w-3.5 h-3.5" />
            )}
            <span className="flex-1">{value.desc ? 'Descending (Z → A)' : 'Ascending (A → Z)'}</span>
            {value.desc && <Check className="w-3.5 h-3.5" />}
          </button>

          <button
            type="button"
            role="menuitemcheckbox"
            aria-checked={value.foldersFirst}
            className="ci-menu-item"
            onClick={() => onChange({ ...value, foldersFirst: !value.foldersFirst })}
          >
            <FolderClosed className="w-3.5 h-3.5" />
            <span className="flex-1">Folders first</span>
            {value.foldersFirst && <Check className="w-3.5 h-3.5" />}
          </button>
        </div>
      )}
    </div>
  );
};
