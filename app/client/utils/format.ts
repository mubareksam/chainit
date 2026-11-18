import { DriveTarget, EnvironmentSimulation, PcVolume } from '../../shared/types';

export function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const idx = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const val = bytes / Math.pow(1024, idx);
  return `${val < 10 && idx > 0 ? val.toFixed(1) : Math.round(val)} ${units[idx]}`;
}

export function toWindowsDisplayPath(target: DriveTarget, virtualPath: string, pcRootLetter = 'C'): string {
  if (target === 'phone') {
    if (!virtualPath || virtualPath === '/') return 'Z:\\';
    return `Z:${virtualPath.replace(/\//g, '\\')}`;
  }
  // A PC path may name its own volume ("/D:/Games"), which must not be rewritten to C: - that would
  // show the user a folder that does not exist on the drive they are looking at.
  const named = /^\/([A-Za-z]):(\/.*)?$/.exec(virtualPath || '');
  if (named) {
    const letter = named[1].toUpperCase();
    const rest = (named[2] || '').replace(/\//g, '\\');
    return `${letter}:\\${rest.replace(/^\\/, '')}`;
  }
  const drive = `${pcRootLetter.toUpperCase()}:`;
  if (!virtualPath || virtualPath === '/') return `${drive}\\`;
  return `${drive}${virtualPath.replace(/\//g, '\\')}`;
}

/** "AC" / "USB" / "wireless" - what the phone is being powered by, as reported by BatteryManager. */
export function chargeLabel(env: EnvironmentSimulation): string {
  if (!env.phoneCharging) return 'on battery';
  switch (env.phoneChargeSource) {
    case 'ac':
      return 'AC';
    case 'usb':
      return 'USB';
    case 'wireless':
      return 'wireless';
    default:
      return 'charging';
  }
}

/** Emerald when healthy or on charge, amber below 40%, red below 15% - the same rule the engine uses. */
export function batteryTint(env: EnvironmentSimulation): string {
  // An unmeasured battery gets no colour: a tint is a claim about the charge.
  if (!batteryKnown(env)) return 'opacity-60';
  const pct = env.phoneBatteryPct ?? 0;
  if (env.phoneCharging || pct >= 40) return 'text-emerald-400';
  if (pct >= 15) return 'text-amber-400';
  return 'text-rose-400';
}

/**
 * Whether a percentage has actually been reported (v1.9.0).
 *
 * The engine used to default the phone's battery to a literal 88, so a handset that answered without
 * the field was drawn as a healthy 88 % gauge. `phoneBatteryPct === 0` is a real reading — a dead
 * battery — and must not be confused with "nobody measured one", which is what this distinguishes.
 */
export function batteryKnown(env: EnvironmentSimulation): boolean {
  // An engine that predates the flag is treated as reporting one: it only ever set a percentage when
  // a phone answered, and refusing to draw it would hide a real reading.
  return env.phoneBatteryKnown !== false && (env.phoneSeenMs ?? 0) > 0;
}

/**
 * Which link carried the last reading, as a phrase.
 *
 * Over Bluetooth this is the point of the whole card: it is the difference between "the offline link
 * carries real telemetry" and "this is a Wi-Fi report from four minutes ago".
 */
export function telemetryCarrierLabel(env: EnvironmentSimulation): string {
  switch (env.phoneTelemetryTransport) {
    case 'bluetooth':
      return 'over Bluetooth RFCOMM';
    case 'wifi_lan':
      return 'over Wi-Fi / LAN';
    case 'local_hotspot':
      return 'over the hotspot';
    case 'usb_rndis':
      return 'over USB tethering';
    case 'online_p2p':
      return 'over the online relay';
    default:
      return '';
  }
}

/** The percentage, or an honest placeholder when no phone has reported one. */
export function batteryLabel(env: EnvironmentSimulation): string {
  return batteryKnown(env) ? `${env.phoneBatteryPct ?? 0}%` : '—';
}

/**
 * The full sentence behind the percentage.
 * Nothing is invented: when no phone has reported (phoneSeenMs is 0 or missing) the tooltip says so,
 * when a phone answered but named no battery it says that instead, and when the report is older than
 * three minutes it says how old rather than implying "now".
 */
export function batteryTooltip(env: EnvironmentSimulation): string {
  const seen = env.phoneSeenMs ?? 0;
  if (!seen) return 'No phone has reported its battery yet — open ChainIt on the phone.';
  if (!batteryKnown(env)) {
    return 'The phone answered, but its ChainIt build reports no battery detail — update the APK.';
  }
  const ageMin = Math.max(0, Math.round((Date.now() - seen) / 60000));
  const age = ageMin < 1 ? 'just now' : ageMin < 60 ? `${ageMin} min ago` : `${Math.round(ageMin / 60)} h ago`;
  const temp = typeof env.phoneTemperatureC === 'number' && env.phoneTemperatureC > 0
    ? ` · ${env.phoneTemperatureC.toFixed(1)}°C`
    : '';
  const carrier = telemetryCarrierLabel(env);
  return `Phone battery ${env.phoneBatteryPct}% · ${chargeLabel(env)}${temp} · reported ${age}${
    env.phoneTelemetryLive === false ? ' (stale)' : ''
  }${carrier ? ` · ${carrier}` : ''}`;
}

/**
 * Who moved the bytes, in two words - the chip label and the sentence behind it.
 * An entry is only actionable if the reader knows whether they sent it, the PC's File Manager sent it,
 * or the phone sent it by itself (which is how a screen-off share shows up later).
 */
export function sourceLabel(source: string): string {
  switch (source) {
    case 'ui':
      return 'File Manager';
    case 'api':
      return 'client call';
    case 'phone-upload':
      return 'phone pushed';
    case 'phone-download':
      return 'phone pulled';
    case 'webdav':
      return 'Z: drive';
    case 'preview':
      return 'preview';
    default:
      return source || 'recorded';
  }
}

export function sourceExplain(source: string): string {
  switch (source) {
    case 'ui':
      return 'Started in the Windows File Manager (the native ChainIt window).';
    case 'api':
      return 'Started by a client call to the PC\u2019s file API.';
    case 'phone-upload':
      return 'The phone pushed this file to the PC - possibly with its screen off.';
    case 'phone-download':
      return 'The phone saved this file from the PC.';
    case 'webdav':
      return 'Written through the mounted Z: drive (WebDAV).';
    case 'preview':
      return 'Recorded by the node preview runtime.';
    default:
      return 'Provenance not recorded for this entry.';
  }
}

/** Caption for a volume chip: "D: Data · 412 GB free" with an honest note when it is not readable. */
export function volumeCaption(vol: PcVolume): string {
  const head = vol.label ? `${vol.letter}: ${vol.label}` : `${vol.letter}:`;
  if (vol.isPcRoot) return `${head} (profile folder)`;
  if (!vol.ready) return vol.kind === 'network' ? `${head} (remote)` : `${head} (no media)`;
  if (vol.totalBytes <= 0) return head;
  return `${head} · ${formatBytes(vol.freeBytes)} free`;
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
