import React, { useState } from 'react';
import {
  Bluetooth,
  Cable,
  CheckCircle2,
  Cpu,
  Globe,
  Link2,
  Lock,
  Radio,
  Sliders,
  Sparkles,
  Wifi,
  WifiOff,
  Zap,
  ShieldCheck,
} from 'lucide-react';
import { SystemSnapshot, TransportId, TransportTelemetry } from '../../shared/types';
import { api } from '../api/client';

interface Props {
  snapshot: SystemSnapshot;
  onSnapshot: (s: SystemSnapshot) => void;
  notify: (msg: string, kind?: 'success' | 'error' | 'info') => void;
}

export const ConnectivityView: React.FC<Props> = ({ snapshot, onSnapshot, notify }) => {
  const [phoneIp, setPhoneIp] = useState('');
  const [siblingKey, setSiblingKey] = useState('CHAINIT-SIBLING');
  const renderTransportIcon = (id: TransportId) => {
    switch (id) {
      case 'usb_rndis':
        return <Cable className="w-5 h-5" />;
      case 'wifi_lan':
        return <Wifi className="w-5 h-5" />;
      case 'local_hotspot':
        return <Radio className="w-5 h-5" />;
      case 'bluetooth':
        return <Bluetooth className="w-5 h-5" />;
      case 'online_p2p':
        return <Globe className="w-5 h-5" />;
    }
  };

  const handleModeSwitch = async (mode: 'auto' | 'manual', manualId?: TransportId) => {
    try {
      const next = await api.setTransportMode(mode, manualId);
      onSnapshot(next);
      notify(
        mode === 'auto'
          ? 'Switched to Automatic Smart Link selection'
          : `Pinned Manual transport: ${manualId}`,
        'info'
      );
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Mode update failed', 'error');
    }
  };

  const toggleEnv = async (key: keyof SystemSnapshot['environment']) => {
    const current = snapshot.environment[key];
    if (typeof current !== 'boolean') return;
    try {
      const next = await api.updateEnvironment({ [key]: !current });
      onSnapshot(next);
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Simulation update failed', 'error');
    }
  };

  const runRouterlessHotspotBoostDemo = async () => {
    try {
      // 1. Ensure Auto mode, unplug USB & turn off shared Wi-Fi router, start with Hotspot off & Bluetooth on
      await api.setTransportMode('auto');
      await api.updateEnvironment({
        usbPlugged: false,
        sharedWifiAvailable: false,
        localHotspotActive: false,
        bluetoothEnabled: true,
      });
      // 2. Perform a file transfer -> triggers BLE -> 5 GHz Local Hotspot auto-promotion!
      const res = await api.transfer({
        op: 'copy',
        fromTarget: 'pc',
        fromPaths: ['/Pictures/Architecture_Diagram_Card.svg'],
        toTarget: 'phone',
        toDir: '/ChainIt_Inbox',
      });
      onSnapshot(res.snapshot);
      notify(
        'Routerless Demo: Wi-Fi router OFF -> BLE woke Android 5 GHz Local Hotspot (72.4 MB/s) automatically!',
        'success'
      );
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Demo failed', 'error');
    }
  };

  const activeTransport = snapshot.transports.find((t) => t.isActive) || null;
  const bt = snapshot.bluetooth;

  return (
    <div className="space-y-6">
      {/* Top Mode Banner */}
      <div className="ci-card-pad flex flex-col lg:flex-row lg:items-center lg:justify-between gap-4">
        <div className="space-y-1">
          <div className="flex items-center gap-2">
            <Cpu className="w-4 h-4 text-cyan-400" />
            <h2 className="text-base font-semibold text-slate-100">
              Multi-Transport Engine — Offline-First + Pluggable Online
            </h2>
          </div>
          <p className="ci-body max-w-2xl leading-relaxed">
            In <strong className="text-slate-200">Automatic</strong> mode, ChainIt continuously scores bandwidth,
            RTT latency, signal stability, and battery cost (with +6.0 hysteresis to prevent link flapping), and
            transparently promotes Bluetooth to a router-free 5 GHz Local Hotspot for bulk transfers.
          </p>
        </div>

        {/* Segmented Control */}
        <div className="inline-flex rounded-xl border border-slate-800 bg-slate-950 p-1 shrink-0 self-start">
          <button
            onClick={() => void handleModeSwitch('auto')}
            className={`inline-flex items-center gap-2 px-4 py-2 rounded-lg text-xs font-semibold transition ${
              snapshot.selectionMode === 'auto'
                ? 'bg-emerald-500 text-slate-950 shadow'
                : 'text-slate-400 hover:text-slate-200'
            }`}
          >
            <Sparkles className="w-3.5 h-3.5" />
            Automatic (Smart Select)
          </button>
          <button
            onClick={() =>
              void handleModeSwitch('manual', snapshot.activeTransportId || snapshot.manualTransportId)
            }
            className={`inline-flex items-center gap-2 px-4 py-2 rounded-lg text-xs font-semibold transition ${
              snapshot.selectionMode === 'manual'
                ? 'bg-cyan-500 text-slate-950 shadow'
                : 'text-slate-400 hover:text-slate-200'
            }`}
          >
            <Sliders className="w-3.5 h-3.5" />
            Manual (Pinned Link)
          </button>
        </div>
      </div>

      {/* Transport Scorecards Grid */}
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-5 gap-4">
        {snapshot.transports.map((t: TransportTelemetry) => {
          const isPinnedManual =
            snapshot.selectionMode === 'manual' && snapshot.manualTransportId === t.id;
          return (
            <div
              key={t.id}
              className={`rounded-2xl border p-4 flex flex-col justify-between transition ${
                t.isActive
                  ? 'border-emerald-500/70 bg-emerald-950/20 shadow-lg shadow-emerald-950/30'
                  : 'border-slate-800 bg-slate-900/80 hover:border-slate-700'
              }`}
            >
              <div>
                <div className="flex items-start justify-between gap-2 mb-3">
                  <div
                    className={`p-2.5 rounded-xl ${
                      t.isActive
                        ? 'bg-emerald-500/20 text-emerald-300'
                        : t.available
                          ? 'bg-slate-800 text-cyan-300'
                          : 'bg-slate-800/60 text-slate-500'
                    }`}
                  >
                    {renderTransportIcon(t.id)}
                  </div>
                  <div className="flex flex-col items-end gap-1">
                    <span
                      className={`text-[10px] font-semibold uppercase tracking-wider px-2 py-0.5 rounded-full ${
                        t.isOffline
                          ? 'bg-slate-800 text-slate-300'
                          : 'bg-purple-500/15 text-purple-300 border border-purple-500/30'
                      }`}
                    >
                      {t.isOffline ? '100% Offline' : 'Pluggable Online'}
                    </span>
                    {t.isActive && (
                      <span className="inline-flex items-center gap-1 text-[11px] font-semibold text-emerald-400">
                        <CheckCircle2 className="w-3.5 h-3.5" /> Active Pipe
                      </span>
                    )}
                  </div>
                </div>

                <h3 className="ci-h2">{t.shortName}</h3>
                <p className="text-[11px] text-slate-400 mt-0.5 min-h-[32px]">{t.statusNote}</p>

                {/* Metrics */}
                <div className="mt-4 pt-3 border-t border-slate-800/80 grid grid-cols-2 gap-2 text-xs">
                  <div>
                    <div className="text-[10px] text-slate-500 uppercase">Throughput</div>
                    <div className="font-mono font-semibold text-slate-200">{t.speedMBps} MB/s</div>
                  </div>
                  <div>
                    <div className="text-[10px] text-slate-500 uppercase">Latency</div>
                    <div className="font-mono font-semibold text-slate-200">{t.rttMs} ms</div>
                  </div>
                </div>

                {/* Score Bar */}
                <div className="mt-3">
                  <div className="flex items-center justify-between text-[11px] mb-1">
                    <span className="text-slate-400">Smart Link Score</span>
                    <span className="font-mono font-bold text-slate-100">{t.score} / 100</span>
                  </div>
                  <div className="h-1.5 rounded-full bg-slate-800 overflow-hidden">
                    <div
                      className={`h-full rounded-full transition-all duration-300 ${
                        t.isActive
                          ? 'bg-emerald-400'
                          : t.available
                            ? 'bg-cyan-400'
                            : 'bg-slate-700'
                      }`}
                      style={{ width: `${Math.max(4, t.score)}%` }}
                    />
                  </div>
                </div>
              </div>

              <button
                onClick={() => void handleModeSwitch('manual', t.id)}
                className={`mt-4 w-full py-1.5 rounded-lg text-xs font-semibold transition ${
                  isPinnedManual
                    ? 'bg-cyan-500 text-slate-950'
                    : 'bg-slate-800 hover:bg-slate-700 text-slate-200'
                }`}
              >
                {isPinnedManual ? 'Pinned (Manual)' : 'Pin Manually'}
              </button>
            </div>
          );
        })}
      </div>

      {/* Security & transfer integrity */}
      <div className="ci-card-pad space-y-3">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="ci-h2 flex items-center gap-2">
              <ShieldCheck className="w-4 h-4 text-[var(--ci-accent)]" />
              Sibling security &amp; transfer integrity
            </h3>
            <p className="ci-body">
              Every request to the phone carries the Pairing Key — missing or wrong keys are refused and
              lock the VFS port after 5 tries. Files travel as simultaneous 2 MiB chunks, each one
              SHA-256 verified in flight, and the finished file is hash-checked against the phone's
              whole-file digest before it replaces anything on disk. A compromised device therefore
              cannot silently poison the other side's files.
            </p>
          </div>
          <span className={`ci-pill-${(snapshot.security?.integrityFailures ?? 0) === 0 ? 'ok' : 'warn'}`}>
            {(snapshot.security?.integrityFailures ?? 0) === 0 ? 'INTEGRITY OK' : 'HASH FAILURES'}
          </span>
        </div>

        <div className="grid grid-cols-2 md:grid-cols-4 gap-2">
          <div className="ci-tile">
            <div className="ci-section-label">Key auth</div>
            <div className="ci-h2 mt-1 flex items-center gap-2">
              <span className="ci-dot ci-dot-ok" />
              {snapshot.security?.keyAuth ?? 'required'}
            </div>
          </div>
          <div className="ci-tile">
            <div className="ci-section-label">Brute-force brake</div>
            <div className="ci-body mt-1">
              {snapshot.security?.lockoutAfter ?? 5} tries / {snapshot.security?.lockoutWindowSec ?? 60}s →{' '}
              {snapshot.security?.lockoutCooldownSec ?? 30}s lock
            </div>
          </div>
          <div className="ci-tile">
            <div className="ci-section-label">Chunks verified</div>
            <div className="ci-h2 mt-1 ci-mono">{snapshot.security?.chunksVerified ?? 0}</div>
          </div>
          <div className="ci-tile">
            <div className="ci-section-label">Parallel streams</div>
            <div className="ci-h2 mt-1 ci-mono">
              {snapshot.security?.parallelWorkers ?? 4} × {Math.round((snapshot.security?.chunkBytes ?? 2097152) / 1048576)} MiB
            </div>
          </div>
        </div>
      </div>

      {/* Physical Scenario Lab */}
      <div className="ci-card-pad space-y-4">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="ci-h2">
              Interactive Hardware & Network Environment Simulator
            </h3>
            <p className="ci-body">
              Toggle cables, routers, or phone screen lock below to watch ChainIt fail over and adapt in real time.
            </p>
          </div>

          <button
            onClick={() => void runRouterlessHotspotBoostDemo()}
            className="inline-flex items-center gap-2 px-3.5 py-2 rounded-xl bg-gradient-to-r from-emerald-500 to-cyan-500 text-slate-950 font-semibold text-xs hover:opacity-95 transition shadow"
          >
            <Zap className="w-4 h-4" />
            Test Routerless Offline Transfer (BLE → 5GHz Hotspot Boost)
          </button>
        </div>

        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
          <EnvSwitchCard
            title="USB-C Cable Plugged In"
            subtitle="Simulates USB RNDIS / ADB wired connection (420 MB/s)"
            active={snapshot.environment.usbPlugged}
            onClick={() => void toggleEnv('usbPlugged')}
            icon={<Cable className="w-4 h-4" />}
          />
          <EnvSwitchCard
            title="Shared Wi-Fi Router Online"
            subtitle="Turn OFF to test 100% routerless offline connectivity"
            active={snapshot.environment.sharedWifiAvailable}
            onClick={() => void toggleEnv('sharedWifiAvailable')}
            icon={
              snapshot.environment.sharedWifiAvailable ? (
                <Wifi className="w-4 h-4" />
              ) : (
                <WifiOff className="w-4 h-4" />
              )
            }
          />
          <EnvSwitchCard
            title="5 GHz Local Hotspot Active"
            subtitle="Direct Android SoftAP (auto-starts over BLE when needed)"
            active={snapshot.environment.localHotspotActive}
            onClick={() => void toggleEnv('localHotspotActive')}
            icon={<Radio className="w-4 h-4" />}
          />
          <EnvSwitchCard
            title="Bluetooth (Native RFCOMM + PAN)"
            subtitle="Direct Bluetooth socket for Z:\ browsing & offline file transfer"
            active={snapshot.environment.bluetoothEnabled}
            onClick={() => void toggleEnv('bluetoothEnabled')}
            icon={<Bluetooth className="w-4 h-4" />}
          />
          <EnvSwitchCard
            title="Phone Screen OFF (Locked in Pocket)"
            subtitle="Proves Z:\ browsing works without touching the phone"
            active={snapshot.environment.phoneScreenLocked}
            onClick={() => void toggleEnv('phoneScreenLocked')}
            icon={<Lock className="w-4 h-4" />}
          />
          <EnvSwitchCard
            title="Pluggable Online P2P Module"
            subtitle="Simulates remote internet QUIC/WebRTC tunnel"
            active={snapshot.environment.onlineP2pEnabled}
            onClick={() => void toggleEnv('onlineP2pEnabled')}
            icon={<Globe className="w-4 h-4" />}
          />
        </div>

        {activeTransport && (
          <div className="ci-inset px-4 py-2.5 flex flex-wrap items-center justify-between gap-2 text-xs">
            <span className="text-slate-300">
              Current Winning Link:{' '}
              <strong className="text-emerald-400">{activeTransport.name}</strong> (
              {activeTransport.speedMBps} MB/s • {activeTransport.rttMs} ms RTT)
            </span>
            <span className="font-mono text-slate-400">
              Mode: {snapshot.selectionMode.toUpperCase()} • Z:\ VFS:{' '}
              {snapshot.sibling.zDriveMounted ? 'MOUNTED' : 'STANDBY'}
            </span>
          </div>
        )}
      </div>

      {/* Direct Phone IP / Bluetooth Link (Fallback when Router AP-Isolation, Bluetooth MAC, or Online VPN is used) */}
      <form
        onSubmit={async (e) => {
          e.preventDefault();
          if (!phoneIp.trim()) return;
          try {
            const next = await api.connectPhoneIp(phoneIp.trim(), siblingKey.trim());
            onSnapshot(next);
            notify(`Linked directly to Android Phone (${phoneIp.trim()})!`, 'success');
          } catch (err) {
            notify(err instanceof Error ? err.message : 'Direct connect failed', 'error');
          }
        }}
        className="ci-card-pad flex flex-col lg:flex-row lg:items-center justify-between gap-4"
      >
        <div className="space-y-1">
          <h3 className="ci-h2 flex items-center gap-2">
            <Link2 className="w-4 h-4 text-emerald-400" />
            Direct Phone IP or Bluetooth MAC Link (Optional — Auto-Discovery Scans Paired Bluetooth & LAN Automatically)
          </h3>
          <p className="ci-body">
            Paired Bluetooth phones & LAN phones connect automatically. You can also enter a Phone IP (e.g.{' '}
            <code className="text-slate-200">192.168.43.1</code>) or Bluetooth address (e.g.{' '}
            <code className="text-slate-200">bt://AA:BB:CC:DD:EE:FF</code>).
          </p>
        </div>

        <div className="flex flex-wrap items-center gap-2 shrink-0">
          <input
            value={phoneIp}
            onChange={(e) => setPhoneIp(e.target.value)}
            placeholder="IP or bt://AA:BB:CC:DD:EE:FF"
            className="w-56 px-3 py-2 ci-inset text-xs font-mono text-slate-100 focus:outline-none focus:border-emerald-400"
          />
          <input
            value={siblingKey}
            onChange={(e) => setSiblingKey(e.target.value)}
            placeholder="Sibling Key"
            className="w-40 px-3 py-2 ci-inset text-xs font-mono text-emerald-300 focus:outline-none focus:border-emerald-400"
          />
          <button
            type="submit"
            className="ci-btn-primary"
          >
            Connect Link
          </button>
        </div>
      </form>

      {/* Native Bluetooth RFCOMM Engine Status (real AF_BTH sockets on Windows, BluetoothServerSocket on Android) */}
      {bt && (
        <div className="rounded-2xl border border-sky-900/60 bg-sky-950/25 p-5 space-y-3">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <h3 className="ci-h2 flex items-center gap-2">
              <Bluetooth className="w-4 h-4 text-sky-400" />
              Native Bluetooth RFCOMM Engine (Zero Wi-Fi • Zero Hotspot • Zero USB)
            </h3>
            <span
              className={`px-2.5 py-1 rounded-full text-[11px] font-bold ${
                bt.activeLink
                  ? 'bg-emerald-500/20 text-emerald-300'
                  : bt.radioPresent
                    ? 'bg-sky-500/20 text-sky-300'
                    : 'bg-slate-700/40 text-slate-400'
              }`}
            >
              {bt.activeLink ? 'RFCOMM LINK LIVE' : bt.radioPresent ? 'ARMED • SCANNING PAIRED DEVICES' : 'RADIO OFF'}
            </span>
          </div>

          <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-2.5 text-[11px]">
            <div className="rounded-xl bg-slate-950/70 border border-slate-800 px-3 py-2">
              <div className="text-slate-500">PC Receiver (SDP published)</div>
              <div className="font-mono text-slate-200">
                {bt.receiverListening ? `LISTENING • RFCOMM ch ${bt.receiverRfcommChannel || '?'}` : 'OFFLINE'}
              </div>
            </div>
            <div className="rounded-xl bg-slate-950/70 border border-slate-800 px-3 py-2">
              <div className="text-slate-500">Phone Server UUID</div>
              <div className="font-mono text-sky-300 break-all">{bt.phoneServerUuid}</div>
            </div>
            <div className="rounded-xl bg-slate-950/70 border border-slate-800 px-3 py-2">
              <div className="text-slate-500">PC Receiver UUID</div>
              <div className="font-mono text-sky-300 break-all">{bt.pcReceiverUuid}</div>
            </div>
            <div className="rounded-xl bg-slate-950/70 border border-slate-800 px-3 py-2">
              <div className="text-slate-500">Paired devices discovered</div>
              <div className="font-mono text-slate-200 break-all">
                {bt.pairedDevices.length > 0 ? bt.pairedDevices.join('  ') : 'Pair phone + PC once in Bluetooth settings'}
              </div>
            </div>
          </div>

          <p className="text-[11px] text-slate-400 leading-relaxed">
            ChainIt opens <code className="text-slate-200">socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)</code> on Windows and{' '}
            <code className="text-slate-200">listenUsingRfcommWithServiceRecord()</code> on Android, then runs the exact same
            VFS + WebDAV protocol over that stream — so <strong className="text-slate-200">Z:\</strong> in File Explorer, the
            dual-pane File Manager, inline previews and Phone → PC pushes (files land on your Windows Desktop) all keep working
            with Wi-Fi, Hotspot, mobile data and the USB cable completely off.
          </p>
        </div>
      )}
    </div>
  );
};

const EnvSwitchCard: React.FC<{
  title: string;
  subtitle: string;
  active: boolean;
  onClick: () => void;
  icon: React.ReactNode;
}> = ({ title, subtitle, active, onClick, icon }) => (
  <button
    onClick={onClick}
    className={`text-left p-3.5 rounded-xl border transition flex items-start justify-between gap-3 ${
      active
        ? 'border-cyan-500/40 bg-cyan-950/20'
        : 'border-slate-800 bg-slate-950/50 hover:border-slate-700'
    }`}
  >
    <div className="flex items-start gap-2.5">
      <div
        className={`p-2 rounded-lg mt-0.5 ${
          active ? 'bg-cyan-500/20 text-cyan-300' : 'bg-slate-800 text-slate-500'
        }`}
      >
        {icon}
      </div>
      <div>
        <div className="text-xs font-semibold text-slate-100">{title}</div>
        <div className="text-[11px] text-slate-400 mt-0.5">{subtitle}</div>
      </div>
    </div>
    <span
      className={`px-2 py-0.5 rounded-full text-[10px] font-bold shrink-0 ${
        active ? 'bg-emerald-500/20 text-emerald-300' : 'bg-slate-800 text-slate-500'
      }`}
    >
      {active ? 'ON' : 'OFF'}
    </span>
  </button>
);
