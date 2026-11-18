import React, { useCallback, useEffect, useState } from 'react';
import {
  BatteryCharging,
  Bluetooth,
  ExternalLink,
  HardDrive,
  Lock,
  MonitorDown,
  RefreshCw,
  Smartphone,
  Usb,
  Wifi,
  Zap,
} from 'lucide-react';
import { SystemSnapshot, TransportId } from '../../shared/types';
import { api } from '../api/client';
import { LogoBadge } from './Brand';
import { batteryKnown, batteryLabel, batteryTint, batteryTooltip, chargeLabel, formatBytes } from '../utils/format';

/**
 * Tray mini-panel: a tiny always-handy dashboard opened from the tray icon
 * ("Mini status panel"). Same engine, same data — just a pocket-sized,
 * skeuomorphic control surface.
 */
export const MiniPanel: React.FC = () => {
  const [snapshot, setSnapshot] = useState<SystemSnapshot | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [note, setNote] = useState('');

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const s = await api.getState();
        if (alive) setSnapshot(s);
      } catch {
        /* engine restarting */
      }
    };
    void tick();
    const id = setInterval(tick, 2000);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, []);

  const act = useCallback(async (kind: string, fn: () => Promise<string>) => {
    setBusy(kind);
    setNote('');
    try {
      setNote(await fn());
    } catch (err) {
      setNote(err instanceof Error ? err.message : 'Action failed');
    } finally {
      setBusy(null);
    }
  }, []);

  const siblingOn = snapshot?.sibling.enabled ?? false;
  const connected = snapshot?.sibling.connected ?? false;
  const zMounted = snapshot?.sibling.zDriveMounted ?? false;
  const active: TransportId | null = snapshot?.activeTransportId ?? null;
  const battery = snapshot ? batteryLabel(snapshot.environment) : '—';
  const locked = snapshot?.environment.phoneScreenLocked ?? false;
  const last = snapshot?.transfers[0];

  const led = (on: boolean, color: 'ok' | 'info' | 'warn') => (
    <span className={`ci-dot ${on ? `ci-dot-${color}` : ''}`} style={{ width: 10, height: 10 }} />
  );

  const transports: { id: TransportId; label: string; icon: React.ReactNode }[] = [
    { id: 'usb_rndis', label: 'USB', icon: <Usb className="w-3.5 h-3.5" /> },
    { id: 'wifi_lan', label: 'Wi-Fi', icon: <Wifi className="w-3.5 h-3.5" /> },
    { id: 'local_hotspot', label: 'Hotspot', icon: <Zap className="w-3.5 h-3.5" /> },
    { id: 'bluetooth', label: 'BT', icon: <Bluetooth className="w-3.5 h-3.5" /> },
  ];

  return (
    <div className="ci-shell min-h-screen flex flex-col" style={{ maxWidth: 460, margin: '0 auto' }}>
      <header className="ci-header px-4 py-3">
        <div className="flex items-center gap-2.5">
          <LogoBadge size={30} />
          <div className="flex-1 min-w-0">
            <div className="flex items-center gap-2">
              <span className="text-sm font-bold text-white">ChainIt</span>
              <span className="ci-pill-off">mini</span>
            </div>
            <p className="ci-muted truncate" style={{ fontSize: 11 }}>
              {snapshot ? (connected ? 'Sibling link live' : 'Scanning for your phone…') : 'Contacting engine…'}
            </p>
          </div>
          <a className="ci-btn-icon" href="/" title="Open the full File Manager">
            <ExternalLink className="w-4 h-4" />
          </a>
        </div>
      </header>

      <main className="p-3 space-y-3 flex-1">
        {/* Status console */}
        <section className="ci-card-pad space-y-2.5">
          <div className="flex items-center justify-between">
            <span className="ci-section-label">Link console</span>
            <span className={`ci-pill-${connected ? 'ok' : 'off'}`}>
              {connected ? 'LIVE' : 'OFFLINE'}
            </span>
          </div>

          <div className="ci-inset px-3 py-2.5 space-y-2">
            <div className="flex items-center gap-2.5">
              {led(siblingOn, 'ok')}
              <Smartphone className="w-4 h-4 text-[var(--ci-text-3)]" />
              <span className="ci-body flex-1 truncate">
                {connected ? snapshot?.sibling.peerDeviceName || 'Phone connected' : 'Sibling Mode'}
              </span>
              <span
                className="ci-chip"
                title={snapshot ? batteryTooltip(snapshot.environment) : 'Phone battery'}
              >
                <BatteryCharging
                  className={`w-3 h-3 ${snapshot ? batteryTint(snapshot.environment) : ''}`}
                />
                {battery}
                {snapshot && batteryKnown(snapshot.environment) && snapshot.environment.phoneCharging
                  ? ` · ${chargeLabel(snapshot.environment)}`
                  : ''}
                {snapshot?.environment.phoneTelemetryLive === false ? ' · stale' : ''}
              </span>
              {locked && <Lock className="w-3.5 h-3.5 text-[var(--ci-amber)]" />}
            </div>

            <div className="flex items-center gap-2.5">
              {led(zMounted, 'warn')}
              <HardDrive className="w-4 h-4 text-[var(--ci-text-3)]" />
              <span className="ci-body flex-1">Z:\ phone drive</span>
              <span className={`ci-pill-${zMounted ? 'ok' : 'off'}`}>{zMounted ? 'MOUNTED' : 'OFF'}</span>
            </div>

            <div className="flex items-center gap-1.5 pt-1">
              {transports.map((t) => (
                <span
                  key={t.id}
                  className="ci-chip"
                  data-on={active === t.id}
                  title={`${t.label}${active === t.id ? ' (active)' : ''}`}
                >
                  {t.icon}
                  {t.label}
                </span>
              ))}
            </div>
          </div>

          {last && (
            <p className="ci-muted" style={{ fontSize: 11 }}>
              Last transfer: {last.label} — {formatBytes(last.bytes)} @{' '}
              {last.effectiveSpeedMBps.toFixed(1)} MB/s over {last.transportUsed}
            </p>
          )}
        </section>

        {/* Quick actions */}
        <section className="ci-card-pad space-y-2">
          <span className="ci-section-label">Quick actions</span>
          <div className="grid grid-cols-2 gap-2">
            <button
              type="button"
              className="ci-btn-ghost justify-start"
              disabled={busy !== null}
              onClick={() =>
                void act('sibling', async () => {
                  const s = await api.updateSibling({ enabled: !siblingOn });
                  return s.sibling.enabled ? 'Sibling Mode enabled' : 'Sibling Mode disabled';
                })
              }
            >
              {busy === 'sibling' ? <RefreshCw className="w-4 h-4 animate-spin" /> : <Smartphone className="w-4 h-4" />}
              <span>{siblingOn ? 'Sibling off' : 'Sibling on'}</span>
            </button>

            <button
              type="button"
              className="ci-btn-ghost justify-start"
              disabled={busy !== null}
              onClick={() =>
                void act('z', async () => {
                  const s = await api.updateSibling({ autoMountZDrive: true, enabled: true });
                  return s.sibling.zDriveMounted ? 'Z:\\ is mounted' : 'Z:\\ mount requested';
                })
              }
            >
              {busy === 'z' ? <RefreshCw className="w-4 h-4 animate-spin" /> : <HardDrive className="w-4 h-4" />}
              <span>Mount Z:\</span>
            </button>

            <button
              type="button"
              className="ci-btn-ghost justify-start"
              disabled={busy !== null}
              onClick={() =>
                void act('icon', async () => {
                  const r = await api.createDesktopShortcut();
                  return r.ok ? 'Desktop icon created' : r.message;
                })
              }
            >
              {busy === 'icon' ? <RefreshCw className="w-4 h-4 animate-spin" /> : <MonitorDown className="w-4 h-4" />}
              <span>Desktop icon</span>
            </button>

            <button
              type="button"
              className="ci-btn-ghost justify-start"
              disabled={busy !== null}
              onClick={() =>
                void act('update', async () => {
                  const r = await api.checkForUpdate();
                  return r.message;
                })
              }
            >
              {busy === 'update' ? <RefreshCw className="w-4 h-4 animate-spin" /> : <RefreshCw className="w-4 h-4" />}
              <span>Update</span>
            </button>
          </div>

          {note && <div className="ci-inset px-3 py-2 ci-body text-[var(--ci-accent-soft)]">{note}</div>}
        </section>
      </main>
    </div>
  );
};
