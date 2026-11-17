/**
 * A faithful model of the Windows RFCOMM dial path, for the browser preview.
 *
 * The preview has no Bluetooth radio, so it cannot connect to a phone. What it *can* do - and
 * what this module exists for - is run the same resolution policy the native engine runs, so
 * the numbers the Link tab shows are results of real dials through that policy rather than
 * constants that happen to look plausible. Inventing `requestsServed: 1284` would be the same
 * mistake as quoting a nominal transfer speed: a figure nobody measured.
 *
 * The policy being modelled is the v1.8.0 one, cheapest-and-most-likely first:
 *   1. an explicit channel in the endpoint text,
 *   2. the channel remembered from an earlier successful dial to that handset,
 *   3. an SDP query for the ChainIt UUID,
 *   4. a bounded blind sweep, every candidate verified by a real request.
 *
 * The property that makes Bluetooth "fast" is assertable here without a radio: the second dial
 * to a handset performs **no** SDP query, because the first one cached the channel.
 */

export type BtDialStrategy = 'explicit' | 'cached' | 'sdp' | 'guess' | 'none';

export interface BtDialResult {
  ok: boolean;
  /** Canonical endpoint text, including the channel when one was resolved. */
  endpoint: string;
  /** Channel the link ended up on (0 when the dial failed). */
  channel: number;
  strategy: BtDialStrategy;
  /** Human-readable cause on failure; empty on success. Never a fake status. */
  error: string;
  /** Measured wall-clock cost of the dial, in milliseconds. */
  durationMs: number;
}

/** What the radio looks like to the model. The preview describes the handset; it never fakes a link. */
export interface BtHandset {
  /** Uppercase colon-separated MAC. */
  address: string;
  /** Whether the ChainIt listener is open at all. */
  listening: boolean;
  /** The RFCOMM channel that listener sits on. */
  channel: number;
  /**
   * Whether that listener published an SDP record. This is the field that reproduces the real
   * world's most common failure: a phone that is listening but invisible to a UUID dial, because
   * it is dozing, was re-paired, or withheld BLUETOOTH_ADVERTISE.
   */
  sdpVisible: boolean;
}

export const BT_PHONE_UUID = 'c4a1b170-0001-4000-8000-00805f9b34fb';
export const BT_PC_UUID = 'c4a1b170-0002-4000-8000-00805f9b34fb';
const MIN_CHANNEL = 1;
const MAX_CHANNEL = 30;
/** Android RFCOMM services sit on low channels almost without exception. */
const SWEEP_LIMIT = 12;

const MAC_RE = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

/** Parses "bt://AA:BB:CC:DD:EE:FF[:channel]", a bare MAC, or a dashed MAC. Null when not Bluetooth. */
export function parseBtEndpoint(raw: string): { address: string; channel: number } | null {
  let text = (raw || '').trim();
  if (text.startsWith('bt://')) text = text.slice(5);
  text = text.replace(/\/+$/g, '');
  if (!text) return null;

  let channel = 0;
  const colonCount = (text.match(/:/g) || []).length;
  if (colonCount === 6) {
    const cut = text.lastIndexOf(':');
    const tail = text.slice(cut + 1);
    if (!/^[0-9]+$/.test(tail)) return null;
    const parsed = Number.parseInt(tail, 10);
    if (!Number.isFinite(parsed) || parsed < MIN_CHANNEL || parsed > MAX_CHANNEL) return null;
    channel = parsed;
    text = text.slice(0, cut);
  } else if (colonCount !== 5 && colonCount !== 0 && !text.includes('-')) {
    return null;
  }

  const normalized = text.replace(/-/g, ':').toUpperCase();
  if (!MAC_RE.test(normalized)) return null;
  return { address: normalized, channel };
}

/** "AA:BB:..." or "bt://aa-bb-..." -> canonical "bt://AA:BB:CC:DD:EE:FF[:channel]". */
export function normalizeBtLink(raw: string): string | null {
  const parsed = parseBtEndpoint(raw);
  if (!parsed) return null;
  return `bt://${parsed.address}${parsed.channel ? `:${parsed.channel}` : ''}`;
}

export class BtLinkModel {
  /** address -> RFCOMM channel that handset actually answered on. */
  private cache = new Map<string, number>();
  private handset: BtHandset | null = null;

  dials = 0;
  successes = 0;
  /** Dials that skipped SDP because the channel was already known. */
  hits = 0;
  /** Dials that had to resolve from scratch. */
  misses = 0;
  sdpQueries = 0;
  /** Links rescued by the verified sweep after SDP found nothing. */
  guessRecovered = 0;
  /** Reused keep-alive channels handed straight back without any dial. */
  pooledReuses = 0;
  requestsServed = 0;
  lastDialError = '';
  lastDialErrorAtMs = 0;
  lastRttMs = 0;
  /** Endpoint the phone reported for itself (address + channel), as the beacon carries it. */
  learnedEndpoint = '';

  /** Describes the handset the preview should behave as though is paired. */
  setHandset(handset: BtHandset | null): void {
    this.handset = handset;
    if (!handset) {
      // No radio, no handset: nothing remembered can be trusted any more.
      this.cache.clear();
      this.learnedEndpoint = '';
    }
  }

  get handsetAddress(): string {
    return this.handset?.address ?? '';
  }

  /** How many handsets have a channel remembered. */
  get cacheEntries(): number {
    return this.cache.size;
  }

  cachedChannel(address: string): number {
    return this.cache.get(address.toUpperCase()) ?? 0;
  }

  /**
   * Reads the phone's self-reported address and channel out of a ping reply or a UDP beacon.
   * This is what lets the PC dial `bt://AA:..:FF:7` directly instead of sweeping every paired
   * device. Returns the endpoint learned, or '' when the payload carried none.
   */
  learnFromPayload(payload: Record<string, unknown> | string): string {
    const obj =
      typeof payload === 'string' ? (safeJsonParse(payload) as Record<string, unknown>) : payload;
    const rawAddress = typeof obj?.btAddress === 'string' ? obj.btAddress : '';
    if (!rawAddress) return '';
    const parsed = parseBtEndpoint(rawAddress);
    if (!parsed) return '';

    const rawChannel = obj?.btRfcommChannel;
    const channel =
      typeof rawChannel === 'number' && rawChannel >= MIN_CHANNEL && rawChannel <= MAX_CHANNEL
        ? rawChannel
        : 0;
    if (channel > 0) this.cache.set(parsed.address, channel);

    const endpoint = `bt://${parsed.address}${channel ? `:${channel}` : ''}`;
    const changed = endpoint !== this.learnedEndpoint;
    this.learnedEndpoint = endpoint;
    return changed ? endpoint : '';
  }

  /** Forgets one handset's channel: its listener moved, so the number is stale. */
  forgetChannel(address: string): void {
    this.cache.delete(address.toUpperCase());
  }

  /** Forgets everything - the radio went away, so no channel is trustworthy. */
  clearCache(): void {
    this.cache.clear();
  }

  /**
   * Runs the resolution policy once and reports what it cost.
   *
   * `budgetMs` bounds the whole resolution, exactly as the native deadline does: the caller's
   * timeout is never multiplied by the number of strategies tried.
   */
  dial(rawEndpoint: string, budgetMs = 4000): BtDialResult {
    const started = Date.now();
    this.dials += 1;
    const parsed = parseBtEndpoint(rawEndpoint);
    if (!parsed) {
      return this.fail(rawEndpoint, 'none', 0, `"${rawEndpoint}" is not a Bluetooth address`, started);
    }

    const handset = this.handset;
    if (!handset) {
      return this.fail(
        parsed.address,
        'none',
        0,
        'this PC has no paired Bluetooth handset to dial',
        started
      );
    }
    if (!handset.listening) {
      return this.fail(
        parsed.address,
        'none',
        0,
        'the phone refused the connection, so its ChainIt listener is not open',
        started
      );
    }
    if (parsed.address !== handset.address) {
      return this.fail(
        parsed.address,
        'none',
        0,
        'that Bluetooth address is not known to this PC (pair it first)',
        started
      );
    }

    // 1/2 - explicit channel, or the one this handset answered on before.
    const remembered = this.cache.get(parsed.address) ?? 0;
    const first = parsed.channel || remembered;
    if (first > 0) {
      if (first === handset.channel) {
        this.cache.set(parsed.address, first);
        if (parsed.channel > 0) this.misses += 1;
        else this.hits += 1;
        return this.succeed(parsed.address, first, parsed.channel > 0 ? 'explicit' : 'cached', started);
      }
      // A number that used to work and now refuses means the listener moved: forget it, so the
      // next dial goes straight to SDP instead of retrying a dead channel first.
      if (remembered === first) this.cache.delete(parsed.address);
    } else {
      this.misses += 1;
    }

    // 3 - authoritative SDP query.
    this.sdpQueries += 1;
    if (handset.sdpVisible) {
      this.cache.set(parsed.address, handset.channel);
      return this.succeed(parsed.address, handset.channel, 'sdp', started);
    }

    // 4 - bounded blind sweep, each candidate verified. A handset that is listening but has
    //    published no record is unreachable any other way, and that is the case that used to
    //    leave the link dead with nothing to show for it.
    const deadline = started + Math.max(500, budgetMs);
    for (let guess = MIN_CHANNEL; guess <= SWEEP_LIMIT; guess += 1) {
      if (guess === first) continue;
      if (Date.now() > deadline) break;
      if (guess === handset.channel) {
        this.cache.set(parsed.address, guess);
        this.guessRecovered += 1;
        return this.succeed(parsed.address, guess, 'guess', started);
      }
    }

    return this.fail(
      parsed.address,
      'guess',
      0,
      'SDP: the phone published no SDP record for ChainIt; verified channel sweep found no ChainIt listener',
      started
    );
  }

  /** Records that a request was served over an already-open channel (no dial). */
  noteReusedRequest(): void {
    this.pooledReuses += 1;
    this.requestsServed += 1;
  }

  /** Records that a request was served after a dial. */
  noteDialedRequest(): void {
    this.requestsServed += 1;
  }

  private succeed(address: string, channel: number, strategy: BtDialStrategy, started: number): BtDialResult {
    this.successes += 1;
    this.lastDialError = '';
    this.lastRttMs = Math.max(0, Date.now() - started);
    return {
      ok: true,
      endpoint: `bt://${address}:${channel}`,
      channel,
      strategy,
      error: '',
      durationMs: this.lastRttMs,
    };
  }

  private fail(
    address: string,
    strategy: BtDialStrategy,
    channel: number,
    error: string,
    started: number
  ): BtDialResult {
    this.lastDialError = error;
    this.lastDialErrorAtMs = Date.now();
    return {
      ok: false,
      endpoint: address.startsWith('bt://') ? address : `bt://${address}`,
      channel,
      strategy,
      error,
      durationMs: Math.max(0, Date.now() - started),
    };
  }
}

function safeJsonParse(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return {};
  }
}

/**
 * The preview's single link model. Named `btLinkModel` rather than `btLink` because
 * `index.ts` already uses `btLink` for the normalised endpoint string in its connect handler.
 */
export const btLinkModel = new BtLinkModel();
