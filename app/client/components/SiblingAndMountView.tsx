import React, { useState } from 'react';
import {
  Check,
  Copy,
  ExternalLink,
  HardDrive,
  KeyRound,
  Lock,
  Play,
  Radio,
  ShieldCheck,
  Smartphone,
  Terminal,
} from 'lucide-react';
import { SystemSnapshot } from '../../shared/types';
import { api } from '../api/client';

interface Props {
  snapshot: SystemSnapshot;
  onSnapshot: (s: SystemSnapshot) => void;
  notify: (msg: string, kind?: 'success' | 'error' | 'info') => void;
}

export const SiblingAndMountView: React.FC<Props> = ({ snapshot, onSnapshot, notify }) => {
  const [copiedCmd, setCopiedCmd] = useState<string | null>(null);
  const [davProbe, setDavProbe] = useState<{
    status: number;
    xml: string;
    durationMs: number;
  } | null>(null);
  const [probing, setProbing] = useState(false);

  const toggleSiblingField = async (field: keyof SystemSnapshot['sibling']) => {
    const cur = snapshot.sibling[field];
    if (typeof cur !== 'boolean') return;
    try {
      const next = await api.updateSibling({ [field]: !cur });
      onSnapshot(next);
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Update failed', 'error');
    }
  };

  const copyText = async (id: string, text: string) => {
    try {
      await navigator.clipboard.writeText(text);
      setCopiedCmd(id);
      setTimeout(() => setCopiedCmd(null), 1800);
      notify('Command copied to clipboard', 'info');
    } catch {
      notify('Select and copy command manually', 'info');
    }
  };

  const runLiveWebDavProbe = async () => {
    setProbing(true);
    try {
      const res = await api.probeWebDavRoot();
      setDavProbe(res);
      const next = await api.getState();
      onSnapshot(next);
      notify(`WebDAV PROPFIND Z:\\ returned HTTP ${res.status} in ${res.durationMs} ms`, 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'WebDAV probe failed', 'error');
    } finally {
      setProbing(false);
    }
  };

  const psScriptCmd = `powershell -ExecutionPolicy Bypass -File .\\scripts\\mount-z-drive.ps1`;
  const netUseCmd = `net use Z: http://127.0.0.1:${snapshot.serverPort}/dav /persistent:no`;
  const mobileUrl =
    typeof window !== 'undefined' ? `${window.location.origin}/mobile` : '/mobile';

  return (
    <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
      {/* Left: Sibling Mode Cryptographic Trust & Zero-Touch Policies */}
      <div className="ci-card-pad space-y-5">
        <div className="flex items-start justify-between gap-3">
          <div className="flex items-center gap-3">
            <div
              className={`p-2.5 rounded-xl border ${
                snapshot.sibling.enabled
                  ? 'bg-emerald-500/15 border-emerald-500/40 text-emerald-400'
                  : 'bg-slate-800 border-slate-700 text-slate-400'
              }`}
            >
              <ShieldCheck className="w-5 h-5" />
            </div>
            <div>
              <h2 className="text-base font-semibold text-slate-100">
                Sibling Mode — Zero-Touch Device Bond
              </h2>
              <p className="ci-body">
                Paired Peer: <strong className="text-slate-200">{snapshot.sibling.peerDeviceName}</strong> (
                {snapshot.sibling.peerModel} • {snapshot.sibling.peerAndroidVersion})
              </p>
            </div>
          </div>

          <button
            onClick={() => void toggleSiblingField('enabled')}
            className={`px-3.5 py-1.5 rounded-xl text-xs font-bold transition ${
              snapshot.sibling.enabled
                ? 'bg-emerald-500 text-slate-950 hover:bg-emerald-400'
                : 'bg-slate-800 text-slate-300 hover:bg-slate-700'
            }`}
          >
            {snapshot.sibling.enabled ? 'Sibling: ENABLED' : 'Sibling: PAUSED'}
          </button>
        </div>

        {/* Cryptographic Beacon Telemetry */}
        <div className="rounded-xl border border-slate-800 bg-slate-950/70 p-4 space-y-2.5 text-xs">
          <div className="flex items-center justify-between gap-2">
            <span className="inline-flex items-center gap-1.5 text-slate-400">
              <KeyRound className="w-3.5 h-3.5 text-cyan-400" /> Ed25519 Mutual Fingerprint
            </span>
            <span className="font-mono text-[11px] text-slate-200 truncate">
              {snapshot.sibling.ed25519Fingerprint}
            </span>
          </div>
          <div className="flex items-center justify-between gap-2">
            <span className="inline-flex items-center gap-1.5 text-slate-400">
              <Radio className="w-3.5 h-3.5 text-emerald-400" /> Rotating BLE Wakeup HMAC Tag
            </span>
            <span className="font-mono text-[11px] px-2 py-0.5 rounded bg-emerald-500/10 border border-emerald-500/30 text-emerald-300">
              0x{snapshot.sibling.currentBleBeaconHex} (rotates in{' '}
              {Math.ceil(snapshot.sibling.beaconSecondsRemaining / 60)}m)
            </span>
          </div>
        </div>

        {/* Granular Sibling Sub-Policies */}
        <div className="space-y-2.5">
          <PolicyToggle
            title="Always-Standby Auto-Connect"
            desc="Automatically establishes an encrypted session whenever BLE, LAN, Hotspot, or USB sees your phone—zero 'Connect' clicks."
            checked={snapshot.sibling.autoConnectOnRange}
            disabled={!snapshot.sibling.enabled}
            onChange={() => void toggleSiblingField('autoConnectOnRange')}
          />
          <PolicyToggle
            title="Zero-Prompt Instant Transfers"
            desc="Skip 'Accept incoming file?' popups between Siblings; files land immediately."
            checked={snapshot.sibling.zeroPromptTransfers}
            disabled={!snapshot.sibling.enabled}
            onChange={() => void toggleSiblingField('zeroPromptTransfers')}
          />
          <PolicyToggle
            title="Screen-Off Unattended Phone Storage Access"
            desc="Browse, copy, paste, move, and edit phone files from PC while your phone stays locked in your pocket."
            checked={snapshot.sibling.allowScreenOffVfs}
            disabled={!snapshot.sibling.enabled}
            onChange={() => void toggleSiblingField('allowScreenOffVfs')}
          />
          <PolicyToggle
            title="Auto-Mount Windows Z:\ Drive on Connect"
            desc="Exposes phone storage as local drive Z:\ immediately upon Sibling handshake."
            checked={snapshot.sibling.autoMountZDrive}
            disabled={!snapshot.sibling.enabled}
            onChange={() => void toggleSiblingField('autoMountZDrive')}
          />
        </div>

        {/* Open Mobile Companion Link */}
        <div className="rounded-xl border border-cyan-500/25 bg-cyan-950/20 p-3.5 flex items-center justify-between gap-3">
          <div className="flex items-center gap-2.5">
            <Smartphone className="w-4 h-4 text-cyan-400 shrink-0" />
            <div className="text-xs">
              <div className="font-semibold text-slate-100">Live Android Companion View (/mobile)</div>
              <div className="text-slate-400 text-[11px]">
                Open in a second tab or on your phone over Wi-Fi/Hotspot to test live two-way sync!
              </div>
            </div>
          </div>
          <a
            href={mobileUrl}
            target="_blank"
            rel="noreferrer"
            className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-cyan-500 text-slate-950 font-semibold text-xs hover:bg-cyan-400 shrink-0 transition"
          >
            Open Phone UI <ExternalLink className="w-3.5 h-3.5" />
          </a>
        </div>
      </div>

      {/* Right: Windows Z:\ Virtual Drive Mount & Live WebDAV Inspector */}
      <div className="ci-card-pad space-y-5">
        <div className="flex items-start justify-between gap-3">
          <div className="flex items-center gap-3">
            <div className="p-2.5 rounded-xl bg-cyan-500/15 border border-cyan-500/30 text-cyan-400">
              <HardDrive className="w-5 h-5" />
            </div>
            <div>
              <h2 className="text-base font-semibold text-slate-100">
                Windows File Explorer Z:\ Drive Integration
              </h2>
              <p className="ci-body">
                Mount phone storage natively in <code className="text-slate-200">explorer.exe</code> alongside the
                dedicated ChainIt File Manager
              </p>
            </div>
          </div>
          <span
            className={`px-2.5 py-1 rounded-full text-[11px] font-semibold ${
              snapshot.sibling.zDriveMounted
                ? 'bg-emerald-500/15 text-emerald-300 border border-emerald-500/30'
                : 'bg-amber-500/15 text-amber-300 border border-amber-500/30'
            }`}
          >
            {snapshot.sibling.zDriveMounted ? 'Z:\\ Server Ready (/dav)' : 'Z:\\ Paused'}
          </span>
        </div>

        <div className="space-y-3 text-xs">
          <div className="rounded-xl border border-slate-800 bg-slate-950 p-3.5 space-y-2">
            <div className="flex items-center justify-between">
              <span className="font-semibold text-slate-200 flex items-center gap-1.5">
                <Terminal className="w-3.5 h-3.5 text-emerald-400" />
                Option 1: One-Click PowerShell Mounter (Recommended on Windows 10/11)
              </span>
              <button
                onClick={() => void copyText('ps', psScriptCmd)}
                className="inline-flex items-center gap-1 text-[11px] text-cyan-400 hover:text-cyan-300"
              >
                {copiedCmd === 'ps' ? <Check className="w-3.5 h-3.5" /> : <Copy className="w-3.5 h-3.5" />}
                {copiedCmd === 'ps' ? 'Copied' : 'Copy'}
              </button>
            </div>
            <pre className="p-2.5 rounded-lg bg-slate-900 text-emerald-300 font-mono text-[11px] overflow-x-auto">
              {psScriptCmd}
            </pre>
            <p className="text-[11px] text-slate-400">
              Automatically starts Windows <code className="text-slate-300">WebClient</code>, configures 4 GB file
              transfers, labels <code className="text-slate-300">Z:\</code> as{' '}
              <code className="text-slate-300">Galaxy S24 [ChainIt Sibling]</code>, and opens Windows Explorer.
            </p>
          </div>

          <div className="rounded-xl border border-slate-800 bg-slate-950 p-3.5 space-y-2">
            <div className="flex items-center justify-between">
              <span className="font-semibold text-slate-200">
                Option 2: Direct Windows <code className="font-mono">net use Z:</code> Command
              </span>
              <button
                onClick={() => void copyText('net', netUseCmd)}
                className="inline-flex items-center gap-1 text-[11px] text-cyan-400 hover:text-cyan-300"
              >
                {copiedCmd === 'net' ? <Check className="w-3.5 h-3.5" /> : <Copy className="w-3.5 h-3.5" />}
                {copiedCmd === 'net' ? 'Copied' : 'Copy'}
              </button>
            </div>
            <pre className="p-2.5 rounded-lg bg-slate-900 text-cyan-300 font-mono text-[11px] overflow-x-auto">
              {netUseCmd}
            </pre>
          </div>
        </div>

        {/* Live RFC 4918 WebDAV Probe */}
        <div className="pt-1 space-y-3">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold text-slate-200 flex items-center gap-1.5">
              <Lock className="w-3.5 h-3.5 text-purple-400" />
              Verify Live RFC 4918 WebDAV Z:\ Endpoint
            </span>
            <button
              onClick={() => void runLiveWebDavProbe()}
              disabled={probing}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-purple-500 hover:bg-purple-400 text-slate-950 font-semibold text-xs transition"
            >
              <Play className="w-3.5 h-3.5" />
              {probing ? 'Probing /dav/...' : 'Run PROPFIND /dav/ Test'}
            </button>
          </div>

          {davProbe && (
            <div className="rounded-xl border border-slate-800 bg-slate-950 p-3 space-y-1.5">
              <div className="flex items-center justify-between text-[11px]">
                <span className="text-emerald-400 font-mono font-semibold">
                  HTTP {davProbe.status} Multi-Status
                </span>
                <span className="text-slate-400 font-mono">{davProbe.durationMs} ms</span>
              </div>
              <pre className="max-h-44 overflow-auto text-[10px] font-mono text-slate-300 leading-relaxed">
                {davProbe.xml}
              </pre>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

const PolicyToggle: React.FC<{
  title: string;
  desc: string;
  checked: boolean;
  disabled?: boolean;
  onChange: () => void;
}> = ({ title, desc, checked, disabled, onChange }) => (
  <button
    disabled={disabled}
    onClick={onChange}
    className={`w-full text-left p-3 rounded-xl border transition flex items-center justify-between gap-3 ${
      disabled ? 'opacity-45 cursor-not-allowed' : ''
    } ${
      checked
        ? 'border-slate-700 bg-slate-950/70'
        : 'border-slate-800/80 bg-slate-950/30 hover:border-slate-700'
    }`}
  >
    <div>
      <div className="text-xs font-semibold text-slate-100">{title}</div>
      <div className="text-[11px] text-slate-400 mt-0.5">{desc}</div>
    </div>
    <div
      className={`w-9 h-5 rounded-full p-0.5 transition shrink-0 ${
        checked ? 'bg-emerald-500' : 'bg-slate-700'
      }`}
    >
      <div
        className={`w-4 h-4 rounded-full bg-white transition-transform ${
          checked ? 'translate-x-4' : 'translate-x-0'
        }`}
      />
    </div>
  </button>
);
