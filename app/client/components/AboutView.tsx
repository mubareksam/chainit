import React from 'react';
import {
  Bluetooth,
  Cpu,
  Heart,
  Link2,
  ShieldCheck,
  Smartphone,
  Zap,
} from 'lucide-react';
import { SystemSnapshot } from '../../shared/types';
import { APP_VERSION } from '../App';

interface Props {
  snapshot: SystemSnapshot;
}

/**
 * About / build credits.
 *
 * Kept as a real tab (not a modal) so the attribution is impossible to miss, and
 * so the live link telemetry next to it doubles as proof that the transports
 * named below are the ones actually carrying bytes.
 */
export const AboutView: React.FC<Props> = ({ snapshot }) => {
  const bt = snapshot.bluetooth;
  const sec = snapshot.security;

  const rows: Array<{ label: string; value: string; hint?: string }> = [
    {
      label: 'Windows engine',
      value: `v${APP_VERSION} · native Win32 C++17`,
      hint: 'chainit_win_main.cpp, single portable exe',
    },
    {
      label: 'Android app',
      value: snapshot.sibling.peerAndroidVersion || 'Android 8.0+',
      hint: snapshot.sibling.peerModel || 'com.chainit.android',
    },
    {
      label: 'Active transport',
      value: snapshot.activeTransportId ?? 'none',
      hint: snapshot.sibling.connected ? 'linked' : 'not linked',
    },
    {
      label: 'Bluetooth RFCOMM',
      value: bt?.activeLink ? `live · ${bt.pooledChannels ?? 0}/${bt.maxChannels ?? 3} channels parked` : 'idle',
      hint: bt?.keepAlive
        ? `keep-alive on · ${bt.requestsServed ?? 0} requests served · ${bt.redials ?? 0} dials · ${
            bt.cachedRfcommChannel ? `channel ${bt.cachedRfcommChannel}` : 'SDP each dial'
          }`
        : undefined,
    },
    {
      label: 'Transfer integrity',
      value: sec ? `${sec.integrity}` : 'sha256 per chunk + whole file',
      hint: sec
        ? `${sec.chunksVerified} chunks verified · ${sec.integrityFailures} failures · ${sec.parallelWorkers} × ${Math.round(
            sec.chunkBytes / 1048576
          )} MiB`
        : undefined,
    },
    {
      label: 'Pairing key auth',
      value: sec?.keyAuth ?? 'required',
      hint: sec ? `${sec.lockoutAfter} failures / ${sec.lockoutWindowSec}s → ${sec.lockoutCooldownSec}s lockout` : undefined,
    },
  ];

  return (
    <section className="ci-card overflow-hidden">
      <div className="ci-card-pad space-y-6">
        {/* Attribution */}
        <div className="flex flex-col sm:flex-row sm:items-center gap-4">
          <div className="ci-logo-mark shrink-0">
            <Link2 className="w-6 h-6" />
          </div>
          <div className="min-w-0">
            <h2 className="ci-h1">ChainIt Sibling</h2>
            <p className="ci-body mt-1">
              Android &#8596; Windows file bridge: a real <code className="ci-mono">Z:\</code> phone drive,
              a dual-pane File Manager, and USB / Wi-Fi / hotspot / Bluetooth failover that keeps working
              with the phone screen off.
            </p>
          </div>
        </div>

        <div className="ci-inset rounded-2xl px-5 py-4 flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3">
          <div className="flex items-center gap-3">
            <span className="inline-flex items-center justify-center w-9 h-9 rounded-xl bg-[var(--ci-accent)]/15 text-[var(--ci-accent)]">
              <Heart className="w-5 h-5" />
            </span>
            <div>
              <div className="ci-section-label">Built by</div>
              <div className="text-lg font-bold tracking-tight text-slate-50">Qmer</div>
            </div>
          </div>
          <div className="text-right">
            <div className="ci-section-label">Version</div>
            <div className="ci-mono text-sm text-slate-200">
              {APP_VERSION} <span className="text-[var(--ci-text-4)]">· code 5</span>
            </div>
          </div>
        </div>

        {/* Live telemetry proving the transports work */}
        <div>
          <h3 className="ci-h2 flex items-center gap-2">
            <Cpu className="w-4 h-4 text-[var(--ci-info)]" />
            This link, right now
          </h3>
          <div className="mt-3 grid grid-cols-1 md:grid-cols-2 gap-2">
            {rows.map((r) => (
              <div key={r.label} className="ci-tile">
                <div className="ci-section-label">{r.label}</div>
                <div className="ci-body mt-1 text-slate-100 font-semibold">{r.value}</div>
                {r.hint && <div className="text-[11px] text-[var(--ci-text-4)] mt-0.5 ci-mono">{r.hint}</div>}
              </div>
            ))}
          </div>
        </div>

        {/* What each transport actually does */}
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-3">
          <div className="ci-tile space-y-2">
            <div className="flex items-center gap-2 ci-h2">
              <Bluetooth className="w-4 h-4 text-[var(--ci-info)]" />
              Bluetooth is a full drive, not a file push
            </div>
            <p className="ci-body">
              RFCOMM carries exactly the same HTTP/WebDAV protocol as Wi-Fi, so <code className="ci-mono">Z:\</code>{' '}
              browses, copies, moves, renames and deletes with the radio as the only link. Because dialling
              a radio costs an SDP lookup plus a page, ChainIt keeps{' '}
              <strong className="text-slate-100">three persistent channels open</strong> and reuses them as
              HTTP/1.1 keep-alive sessions — one dial serves a whole Explorer session instead of one request.
              The discovered RFCOMM channel number is cached, so later dials skip SDP entirely.
            </p>
          </div>
          <div className="ci-tile space-y-2">
            <div className="flex items-center gap-2 ci-h2">
              <Smartphone className="w-4 h-4 text-[var(--ci-accent)]" />
              Survives a phone locked all night
            </div>
            <p className="ci-body">
              The Android side runs as a <code className="ci-mono">connectedDevice</code> foreground service —
              the type Android does not time-cap the way it caps <code className="ci-mono">dataSync</code>. A
              Doze heartbeat re-arms the VFS server, the RFCOMM listener and UDP discovery every 9 minutes,
              the partial wake lock renews itself before its 12-hour ceiling, and swiping ChainIt out of
              recents restarts it. Ask once for the battery-optimisation exemption and the link stays up
              indefinitely.
            </p>
          </div>
          <div className="ci-tile space-y-2">
            <div className="flex items-center gap-2 ci-h2">
              <ShieldCheck className="w-4 h-4 text-[var(--ci-accent)]" />
              Neither device can poison the other
            </div>
            <p className="ci-body">
              The Pairing Key is mandatory on every request, five wrong keys in a minute locks the port for
              thirty seconds, and every byte range publishes its SHA-256. Inbound writes are verified and
              deleted on mismatch, downloads are gated chunk-by-chunk and then whole-file before they replace
              anything on disk.
            </p>
          </div>
          <div className="ci-tile space-y-2">
            <div className="flex items-center gap-2 ci-h2">
              <Zap className="w-4 h-4 text-[var(--ci-amber)]" />
              Parallel chunks, invisible to you
            </div>
            <p className="ci-body">
              Downloads split into 2 MiB chunks fetched by four workers at once (one over Bluetooth, where the
              radio serialises a single link anyway), written at exact offsets and renamed atomically. The
              reassembly buffer is hidden and temporary, so you never see a partial file — a transfer is
              either complete and correct, or it does not exist.
            </p>
          </div>
        </div>

        <p className="text-[11px] text-[var(--ci-text-4)] ci-mono">
          Built by Qmer · ChainIt Sibling {APP_VERSION} · Windows engine + Android app + Web File Manager share
          one chain-link identity.
        </p>
      </div>
    </section>
  );
};
