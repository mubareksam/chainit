import React, { useEffect, useRef, useState } from 'react';
import {
  ArrowLeft,
  BatteryCharging,
  Check,
  Download,
  FileText,
  Folder,
  Laptop,
  Lock,
  Send,
  ShieldCheck,
  Smartphone,
  Unlock,
  Wifi,
  X,
} from 'lucide-react';
import { SystemSnapshot, VfsItem } from '../../shared/types';
import { api } from '../api/client';
import { batteryKnown, batteryLabel, batteryTint, batteryTooltip, chargeLabel, formatBytes, telemetryCarrierLabel } from '../utils/format';

interface Props {
  snapshot: SystemSnapshot;
  onSnapshot: (s: SystemSnapshot) => void;
  refreshToken: number;
  onBackToDesktop: () => void;
  notify: (msg: string, kind?: 'success' | 'error' | 'info') => void;
}

export const MobileCompanionPage: React.FC<Props> = ({
  snapshot,
  onSnapshot,
  refreshToken,
  onBackToDesktop,
  notify,
}) => {
  const [pcDir, setPcDir] = useState('/Desktop');
  const [pcItems, setPcItems] = useState<VfsItem[]>([]);
  const fileRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    api
      .listDir('pc', pcDir)
      .then((res) => setPcItems(res.items))
      .catch(() => {});
  }, [pcDir, refreshToken]);

  const activeLink = snapshot.transports.find((t) => t.isActive);

  const handleSendToPc = async (files: FileList | null) => {
    if (!files || files.length === 0) return;
    try {
      for (const f of Array.from(files)) {
        const res = await api.uploadFile('pc', '/Desktop', f);
        onSnapshot(res.snapshot);
      }
      notify(`Sent ${files.length} file(s) instantly to Windows C:\\Desktop!`, 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Send failed', 'error');
    }
  };

  return (
    <div className="min-h-screen bg-slate-950 text-slate-100 flex flex-col items-center p-4">
      <div className="w-full max-w-md space-y-4">
        {/* Top Navigation Back to Desktop Preview */}
        <div className="flex items-center justify-between">
          <button
            onClick={onBackToDesktop}
            className="inline-flex items-center gap-1.5 ci-body hover:text-slate-200"
          >
            <ArrowLeft className="w-3.5 h-3.5" /> Switch to Windows Desktop View
          </button>
          <span className="text-[11px] font-mono text-emerald-400">Android Companion</span>
        </div>

        {/* Phone Frame Card */}
        <div className="rounded-3xl border-2 border-slate-800 bg-slate-900 shadow-2xl overflow-hidden">
          <div className="px-5 py-4 bg-slate-950 border-b border-slate-800 flex items-center justify-between">
            <div className="flex items-center gap-2.5">
              <div className="p-2 rounded-xl bg-emerald-500/15 text-emerald-400">
                <Smartphone className="w-5 h-5" />
              </div>
              <div>
                <h1 className="text-sm font-bold">{snapshot.sibling.peerDeviceName}</h1>
                <p className="text-[11px] text-slate-400">
                  {activeLink
                    ? `Connected via ${activeLink.shortName} (${activeLink.speedMBps} MB/s)`
                    : 'BLE Standby — Scanning for Sibling PC'}
                </p>
              </div>
            </div>
            <Wifi className="w-4 h-4 text-emerald-400" />
          </div>

          <div className="p-4 space-y-4">
            {/* Battery: the number the PC draws, and whether it is a live report (v1.7.0) */}
            <div
              className="rounded-2xl border border-slate-800 bg-slate-950 p-3.5"
              title={batteryTooltip(snapshot.environment)}
            >
              <div className="flex items-center justify-between gap-2">
                <div className="flex items-center gap-2">
                  <BatteryCharging
                    className={`w-4 h-4 ${batteryTint(snapshot.environment)}`}
                  />
                  <span className="text-xs font-semibold">Phone battery</span>
                </div>
                <span className={`text-sm font-mono ${batteryTint(snapshot.environment)}`}>
                  {batteryLabel(snapshot.environment)}
                </span>
              </div>
              <div className="text-[10px] opacity-80 mt-1">
                {batteryKnown(snapshot.environment) ? chargeLabel(snapshot.environment) : 'no battery report'}
                {batteryKnown(snapshot.environment) &&
                typeof snapshot.environment.phoneTemperatureC === 'number' &&
                snapshot.environment.phoneTemperatureC > 0
                  ? ` · ${snapshot.environment.phoneTemperatureC.toFixed(1)} °C`
                  : ''}
                {snapshot.environment.phoneSeenMs
                  ? snapshot.environment.phoneTelemetryLive === false
                    ? ' · stale report'
                    : ' · live'
                  : ' · awaiting the phone\u2019s first report'}
                {telemetryCarrierLabel(snapshot.environment)
                  ? ` · ${telemetryCarrierLabel(snapshot.environment)}`
                  : ''}
              </div>
            </div>

            {/* Sibling & Screen State Controls */}
            <div className="grid grid-cols-2 gap-2.5">
              <button
                onClick={async () => {
                  const next = await api.updateSibling({ enabled: !snapshot.sibling.enabled });
                  onSnapshot(next);
                }}
                className={`p-3 rounded-2xl border text-left transition ${
                  snapshot.sibling.enabled
                    ? 'bg-emerald-500/15 border-emerald-500/40 text-emerald-200'
                    : 'bg-slate-950 border-slate-800 text-slate-400'
                }`}
              >
                <ShieldCheck className="w-4 h-4 mb-1.5" />
                <div className="text-xs font-semibold">Sibling Mode</div>
                <div className="text-[10px] opacity-80">
                  {snapshot.sibling.enabled ? 'ON (Zero-Touch)' : 'OFF (Manual)'}
                </div>
              </button>

              <button
                onClick={async () => {
                  const next = await api.updateEnvironment({
                    phoneScreenLocked: !snapshot.environment.phoneScreenLocked,
                  });
                  onSnapshot(next);
                }}
                className={`p-3 rounded-2xl border text-left transition ${
                  snapshot.environment.phoneScreenLocked
                    ? 'bg-amber-500/15 border-amber-500/40 text-amber-200'
                    : 'bg-cyan-500/15 border-cyan-500/40 text-cyan-200'
                }`}
              >
                {snapshot.environment.phoneScreenLocked ? (
                  <Lock className="w-4 h-4 mb-1.5" />
                ) : (
                  <Unlock className="w-4 h-4 mb-1.5" />
                )}
                <div className="text-xs font-semibold">Phone Screen</div>
                <div className="text-[10px] opacity-80">
                  {snapshot.environment.phoneScreenLocked ? 'OFF (In Pocket)' : 'Awake & Unlocked'}
                </div>
              </button>
            </div>

            {/* Pending Approvals (When Sibling Mode is OFF) */}
            {snapshot.pendingApprovals.length > 0 && (
              <div className="rounded-2xl border border-amber-500/40 bg-amber-950/30 p-3.5 space-y-2.5">
                <div className="text-xs font-semibold text-amber-200">
                  Incoming Request (Sibling Mode is OFF)
                </div>
                {snapshot.pendingApprovals.map((ap) => (
                  <div
                    key={ap.id}
                    className="flex items-center justify-between gap-2 bg-slate-950/70 rounded-xl p-2.5 text-xs"
                  >
                    <span className="truncate text-slate-200">{ap.itemNames.join(', ')}</span>
                    <div className="flex items-center gap-1.5 shrink-0">
                      <button
                        onClick={async () => {
                          const r = await api.resolveApproval(ap.id, true);
                          onSnapshot(r.snapshot);
                          notify('Transfer accepted', 'success');
                        }}
                        className="p-1.5 rounded-lg bg-emerald-500 text-slate-950"
                      >
                        <Check className="w-3.5 h-3.5" />
                      </button>
                      <button
                        onClick={async () => {
                          const r = await api.resolveApproval(ap.id, false);
                          onSnapshot(r.snapshot);
                        }}
                        className="p-1.5 rounded-lg bg-rose-500/20 text-rose-300"
                      >
                        <X className="w-3.5 h-3.5" />
                      </button>
                    </div>
                  </div>
                ))}
              </div>
            )}

            {/* Instant Send to Windows PC */}
            <input
              ref={fileRef}
              type="file"
              multiple
              className="hidden"
              onChange={(e) => {
                void handleSendToPc(e.target.files);
                e.target.value = '';
              }}
            />
            <button
              onClick={() => fileRef.current?.click()}
              className="w-full py-3 rounded-2xl bg-gradient-to-r from-emerald-500 to-cyan-500 text-slate-950 font-bold text-xs flex items-center justify-center gap-2 shadow-lg"
            >
              <Send className="w-4 h-4" />
              Instant Send File to Windows Desktop (C:\Desktop)
            </button>

            {/* Browse PC Files from Phone */}
            <div className="rounded-2xl border border-slate-800 bg-slate-950/60 p-3.5 space-y-2.5">
              <div className="flex items-center justify-between">
                <span className="text-xs font-semibold text-slate-200 flex items-center gap-1.5">
                  <Laptop className="w-3.5 h-3.5 text-cyan-400" /> Browse Windows Sibling (C:{pcDir.replace(/\//g, '\\')})
                </span>
                {pcDir !== '/' && (
                  <button
                    onClick={() => setPcDir('/')}
                    className="text-[11px] text-cyan-400 hover:underline"
                  >
                    Root C:\
                  </button>
                )}
              </div>

              <div className="max-h-56 overflow-y-auto space-y-1">
                {pcItems.map((item) => (
                  <div
                    key={item.path}
                    onClick={() => {
                      if (item.kind === 'directory') setPcDir(item.path);
                    }}
                    className="flex items-center justify-between px-2.5 py-2 rounded-xl hover:bg-slate-800/70 text-xs cursor-pointer"
                  >
                    <div className="flex items-center gap-2 min-w-0">
                      {item.kind === 'directory' ? (
                        <Folder className="w-4 h-4 text-amber-400 shrink-0" />
                      ) : (
                        <FileText className="w-4 h-4 text-cyan-400 shrink-0" />
                      )}
                      <span className="truncate">{item.name}</span>
                    </div>
                    {item.kind === 'file' ? (
                      <a
                        href={api.rawUrl('pc', item.path, true)}
                        download={item.name}
                        onClick={(e) => e.stopPropagation()}
                        className="p-1 text-slate-400 hover:text-cyan-300"
                      >
                        <Download className="w-3.5 h-3.5" />
                      </a>
                    ) : (
                      <span className="text-[10px] text-slate-500">Folder</span>
                    )}
                  </div>
                ))}
                {pcItems.length === 0 && (
                  <p className="text-center text-[11px] text-slate-500 py-4">Empty folder ({formatBytes(0)})</p>
                )}
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};
