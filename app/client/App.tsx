import React, { useCallback, useEffect, useState } from 'react';
import {
  Activity,
  ArrowRightLeft,
  BatteryCharging,
  Check,
  Columns2,
  Download,
  FolderTree,
  HardDrive,
  Info,
  Link2,
  Lock,
  MonitorDown,
  PanelLeft,
  Radio,
  RefreshCw,
  ShieldCheck,
  Smartphone,
  Sparkles,
  Unlock,
  X,
  Zap,
} from 'lucide-react';
import { DriveTarget, SystemSnapshot, VfsItem } from '../shared/types';
import { api } from './api/client';
import { LogoBadge } from './components/Brand';
import { SortState, loadSortPref, saveSortPref } from './utils/sort';
import { AboutView } from './components/AboutView';
import { ConnectivityView } from './components/ConnectivityView';
import { DownloadsModal } from './components/DownloadsModal';
import { ClipboardState, DrivePane } from './components/DrivePane';
import { FilePreviewModal } from './components/FilePreviewModal';
import { MobileCompanionPage } from './components/MobileCompanionPage';
import { MiniPanel } from './components/MiniPanel';
import { SiblingAndMountView } from './components/SiblingAndMountView';
import {
  batteryKnown,
  batteryLabel,
  batteryTint,
  batteryTooltip,
  chargeLabel,
  formatBytes,
  formatShortTime,
  sourceExplain,
  sourceLabel,
  toWindowsDisplayPath,
} from './utils/format';

/** Kept in sync with android/app/src/main/AndroidManifest.xml and the Windows exe. */
export const APP_VERSION = '1.9.0';

type MainTab = 'explorer' | 'connectivity' | 'sibling' | 'about';
type ExplorerLayout = 'dual' | 'single_z';

interface ToastItem {
  id: number;
  message: string;
  kind: 'success' | 'error' | 'info';
}

export const App: React.FC = () => {
  const [snapshot, setSnapshot] = useState<SystemSnapshot | null>(null);
  const [activeTab, setActiveTab] = useState<MainTab>('explorer');
  const [explorerLayout, setExplorerLayout] = useState<ExplorerLayout>('dual');
  const [singleActiveTarget, setSingleActiveTarget] = useState<DriveTarget>('phone');

  const [isMobileRoute, setIsMobileRoute] = useState(
    () => typeof window !== 'undefined' && window.location.pathname.startsWith('/mobile')
  );
  const [isMiniRoute] = useState(
    () => typeof window !== 'undefined' && window.location.pathname.startsWith('/mini')
  );

  const [pcDir, setPcDir] = useState('/Desktop');
  const [phoneDir, setPhoneDir] = useState('/DCIM/Camera');
  const [focusedPane, setFocusedPane] = useState<DriveTarget>('phone');
  const [refreshToken, setRefreshToken] = useState(0);

  const [clipboard, setClipboard] = useState<ClipboardState | null>(null);
  const [previewFile, setPreviewFile] = useState<{ target: DriveTarget; item: VfsItem } | null>(
    null
  );
  const [toasts, setToasts] = useState<ToastItem[]>([]);
  const [showTelemetry, setShowTelemetry] = useState(true);
  const [showDownloads, setShowDownloads] = useState(false);
  const [sort, setSort] = useState<SortState>(() => loadSortPref());
  const handleSortChange = useCallback((next: SortState) => {
    setSort(next);
    saveSortPref(next);
  }, []);
  const [engineVersion, setEngineVersion] = useState<string>(APP_VERSION);
  const [engineRuntime, setEngineRuntime] = useState<string>('');
  const [updating, setUpdating] = useState(false);

  useEffect(() => {
    api.appVersion()
      .then((v) => {
        if (v && v.version) setEngineVersion(v.version);
        if (v && v.runtime) setEngineRuntime(v.runtime);
      })
      .catch(() => undefined);
  }, []);

  const notify = useCallback(
    (message: string, kind: 'success' | 'error' | 'info' = 'info') => {
      const id = Date.now() + Math.random();
      setToasts((prev) => [...prev.slice(-3), { id, message, kind }]);
      setTimeout(() => {
        setToasts((prev) => prev.filter((t) => t.id !== id));
      }, 3600);
    },
    []
  );

  /**
   * True in-place update: the running engine replaces its own binary and restarts,
   * so the old build is gone instead of sitting next to the new one.
   */
  const runSelfUpdate = useCallback(async () => {
    setUpdating(true);
    notify('Checking for a newer ChainIt build…', 'info');
    try {
      const res = await api.checkForUpdate();
      notify(res.message || `Update status: ${res.status}`, res.status === 'error' ? 'error' : 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Update check failed', 'error');
    } finally {
      setUpdating(false);
    }
  }, [notify]);

  const [shortcutBusy, setShortcutBusy] = useState(false);
  /** Asks ChainIt-Windows.exe to drop a chain-link .lnk on the user's Desktop. */
  const runDesktopShortcut = useCallback(async () => {
    setShortcutBusy(true);
    try {
      const res = await api.createDesktopShortcut();
      notify(res.message, res.ok ? 'success' : 'info');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Could not create the desktop icon', 'error');
    } finally {
      setShortcutBusy(false);
    }
  }, [notify]);

  useEffect(() => {
    api
      .getState()
      .then(setSnapshot)
      .catch((err) => notify(err.message, 'error'));
  }, [notify]);

  useEffect(() => {
    let ws: WebSocket | null = null;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;
    let unmounted = false;

    const connectWs = () => {
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(`${proto}//${window.location.host}/ws`);
      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.snapshot) {
            setSnapshot(msg.snapshot);
          }
          if (msg.type === 'vfs_changed') {
            setRefreshToken((n) => n + 1);
          }
        } catch {
          // ignore malformed frame
        }
      };
      ws.onclose = () => {
        if (!unmounted) {
          retryTimer = setTimeout(connectWs, 2000);
        }
      };
    };

    connectWs();
    return () => {
      unmounted = true;
      if (retryTimer) clearTimeout(retryTimer);
      ws?.close();
    };
  }, []);

  const executeTransfer = useCallback(
    async (params: {
      op: 'copy' | 'move';
      fromTarget: DriveTarget;
      fromPaths: string[];
      toTarget: DriveTarget;
      toDir: string;
    }) => {
      try {
        const res = await api.transfer(params);
        setSnapshot(res.snapshot);
        setRefreshToken((n) => n + 1);
        if (res.status === 'awaiting_approval') {
          notify(
            'Paused for confirmation because Sibling Mode is OFF. Approve in banner or re-enable Sibling Mode!',
            'info'
          );
        } else {
          notify(
            `${params.op === 'move' ? 'Moved' : 'Copied'} ${params.fromPaths.length} item(s) → ${toWindowsDisplayPath(
              params.toTarget,
              params.toDir
            )}`,
            'success'
          );
        }
      } catch (err) {
        notify(err instanceof Error ? err.message : 'Transfer failed', 'error');
      }
    },
    [notify]
  );

  const handlePasteInto = useCallback(
    async (toTarget: DriveTarget, toDir: string) => {
      if (!clipboard) return;
      await executeTransfer({
        op: clipboard.op,
        fromTarget: clipboard.fromTarget,
        fromPaths: clipboard.paths,
        toTarget,
        toDir,
      });
      if (clipboard.op === 'move') {
        setClipboard(null);
      }
    },
    [clipboard, executeTransfer]
  );

  const handleQuickEnableSibling = async () => {
    try {
      const s1 = await api.updateSibling({
        enabled: true,
        autoConnectOnRange: true,
        allowScreenOffVfs: true,
        zeroPromptTransfers: true,
        autoMountZDrive: true,
      });
      if (!s1.activeTransportId) {
        const s2 = await api.updateEnvironment({ sharedWifiAvailable: true, bluetoothEnabled: true });
        setSnapshot(s2);
      } else {
        setSnapshot(s1);
      }
      setRefreshToken((n) => n + 1);
      notify('Sibling Mode enabled — Z:\\ auto-mounted & zero-touch access restored!', 'success');
    } catch (err) {
      notify(err instanceof Error ? err.message : 'Failed to enable Sibling Mode', 'error');
    }
  };

  if (!snapshot) {
    return (
      <div className="min-h-screen bg-slate-950 flex items-center justify-center text-slate-300 text-sm gap-2.5">
        <Zap className="w-5 h-5 text-emerald-400 animate-pulse" />
        <span>Starting ChainIt Sibling Daemon & Z:\ VFS Engine...</span>
      </div>
    );
  }

  if (isMiniRoute) {
    return <MiniPanel />;
  }

  if (isMobileRoute) {
    return (
      <>
        <MobileCompanionPage
          snapshot={snapshot}
          onSnapshot={setSnapshot}
          refreshToken={refreshToken}
          onBackToDesktop={() => {
            window.history.pushState({}, '', '/');
            setIsMobileRoute(false);
          }}
          notify={notify}
        />
        <ToastStack toasts={toasts} />
      </>
    );
  }

  const activeTransport = snapshot.transports.find((t) => t.isActive) || null;

  return (
    <div className="ci-shell">
      {/* Application bar */}
      <header className="ci-header px-4 lg:px-6 py-3">
        <div className="ci-container flex flex-wrap items-center justify-between gap-3">
          {/* Brand + Peer Pill */}
          <div className="flex items-center gap-3">
            <div className="flex items-center gap-3">
              <LogoBadge size={38} />
              <div>
                <div className="flex items-center gap-2">
                  <span className="text-[15px] font-bold tracking-tight text-white">ChainIt</span>
                  <span
                    className="ci-pill-info"
                    style={{ letterSpacing: '0.06em' }}
                    title={engineRuntime === 'windows-native' ? 'ChainIt-Windows.exe native engine' : 'ChainIt preview engine'}
                  >
                    v{engineVersion}
                  </span>
                </div>
                <p className="ci-muted hidden sm:block mt-0.5">
                  Windows ⇄ Android Sibling link · USB · Wi-Fi · Hotspot · native Bluetooth
                </p>
              </div>
            </div>

            <div className="hidden md:flex items-center gap-2 pl-3 ml-1 border-l border-slate-800 text-xs">
              <span
                className={`w-2 h-2 rounded-full ${
                  snapshot.sibling.connected ? 'bg-emerald-400 animate-pulse' : 'bg-amber-400'
                }`}
              />
              <span className="font-semibold text-slate-200">{snapshot.sibling.peerDeviceName}</span>
              <span
                className="text-slate-500 inline-flex items-center gap-1"
                title={batteryTooltip(snapshot.environment)}
              >
                <BatteryCharging
                  className={`w-3.5 h-3.5 ${batteryTint(snapshot.environment)}`}
                />
                {batteryLabel(snapshot.environment)}
                {batteryKnown(snapshot.environment) && snapshot.environment.phoneCharging ? (
                  <span className="text-emerald-400/90">· {chargeLabel(snapshot.environment)}</span>
                ) : null}
                {snapshot.environment.phoneTelemetryLive === false ? (
                  <span className="text-amber-400/90">· stale</span>
                ) : null}
              </span>
            </div>
          </div>

          {/* Live Status Pills & Quick Toggles */}
          <div className="flex flex-wrap items-center gap-2">
            {/* Phone Screen Lock Simulator Pill */}
            <button
              onClick={async () => {
                const next = await api.updateEnvironment({
                  phoneScreenLocked: !snapshot.environment.phoneScreenLocked,
                });
                setSnapshot(next);
                setRefreshToken((n) => n + 1);
                notify(
                  next.environment.phoneScreenLocked
                    ? 'Phone screen locked (in pocket) — Z:\\ stays mounted via Sibling VFS!'
                    : 'Phone screen unlocked',
                  'info'
                );
              }}
              className={`inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-xl border text-xs font-medium transition ${
                snapshot.environment.phoneScreenLocked
                  ? 'border-slate-700 bg-slate-900 text-slate-200 hover:border-slate-600'
                  : 'border-cyan-500/30 bg-cyan-950/30 text-cyan-200'
              }`}
              title="Click to simulate locking/unlocking your phone screen in your pocket"
            >
              {snapshot.environment.phoneScreenLocked ? (
                <>
                  <Lock className="w-3.5 h-3.5 text-amber-400" />
                  <span>Phone: Screen OFF</span>
                </>
              ) : (
                <>
                  <Unlock className="w-3.5 h-3.5 text-cyan-400" />
                  <span>Phone: Awake</span>
                </>
              )}
            </button>

            {/* Active Transport Pill */}
            <button
              onClick={() => setActiveTab('connectivity')}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-xl border border-slate-800 bg-slate-900 hover:border-cyan-500/40 text-xs transition"
            >
              <Radio
                className={`w-3.5 h-3.5 ${
                  activeTransport ? 'text-cyan-400' : 'text-rose-400'
                }`}
              />
              <span className="font-semibold text-slate-200">
                {snapshot.selectionMode === 'auto' ? 'Auto' : 'Manual'}:{' '}
                {activeTransport ? activeTransport.shortName : 'Offline Standby'}
              </span>
              {activeTransport && (
                <span className="font-mono text-[11px] text-emerald-400 hidden sm:inline">
                  {activeTransport.speedMBps} MB/s
                </span>
              )}
            </button>

            {/* Sibling Mode Master Switch */}
            <button
              onClick={async () => {
                const next = await api.updateSibling({ enabled: !snapshot.sibling.enabled });
                setSnapshot(next);
                setRefreshToken((n) => n + 1);
                notify(
                  next.sibling.enabled
                    ? 'Sibling Mode ON: Auto-connect, zero prompts & screen-off Z:\\ active'
                    : 'Sibling Mode OFF: Standard manual approval mode',
                  next.sibling.enabled ? 'success' : 'info'
                );
              }}
              className={`inline-flex items-center gap-1.5 px-3 py-1.5 rounded-xl text-xs font-bold transition ${
                snapshot.sibling.enabled
                  ? 'bg-emerald-500/20 border border-emerald-500/50 text-emerald-300 hover:bg-emerald-500/30'
                  : 'bg-slate-800 border border-slate-700 text-slate-400 hover:text-slate-200'
              }`}
            >
              <ShieldCheck className="w-4 h-4" />
              <span>Sibling: {snapshot.sibling.enabled ? 'ON' : 'OFF'}</span>
            </button>

            {/* Switch to Mobile Companion View */}
            <button
              onClick={() => {
                window.history.pushState({}, '', '/mobile');
                setIsMobileRoute(true);
              }}
              className="ci-btn-ghost"
              title="Preview the Android phone side"
            >
              <Smartphone className="w-3.5 h-3.5 text-emerald-400" />
              <span className="hidden sm:inline">Phone View</span>
            </button>

            {/* Create a desktop icon on this PC */}
            <button
              type="button"
              onClick={() => void runDesktopShortcut()}
              disabled={shortcutBusy}
              className="ci-btn-ghost"
              title="Create a ChainIt desktop icon on this PC"
            >
              <MonitorDown className={`w-3.5 h-3.5 text-[var(--ci-accent)] ${shortcutBusy ? 'animate-pulse' : ''}`} />
              <span className="hidden lg:inline">{shortcutBusy ? 'Working…' : 'Desktop icon'}</span>
            </button>

            {/* Replace the installed build with the newest one */}
            <button
              type="button"
              onClick={() => void runSelfUpdate()}
              disabled={updating}
              className="ci-btn-ghost"
              title="Download the newest release and replace the installed ChainIt (no second copy)"
            >
              <RefreshCw className={`w-3.5 h-3.5 text-[var(--ci-info)] ${updating ? 'animate-spin' : ''}`} />
              <span className="hidden sm:inline">{updating ? 'Updating…' : 'Update'}</span>
            </button>

            {/* Download Final .exe & .apk */}
            <button
              onClick={() => setShowDownloads(true)}
              className="ci-btn-primary"
            >
              <Download className="w-3.5 h-3.5" />
              <span>Download .exe & .apk</span>
            </button>
          </div>
        </div>

        {/* Navigation Bar */}
        <div className="ci-container mt-3 pt-3 border-t border-[var(--ci-border-soft)] flex flex-wrap items-center justify-between gap-2">
          <nav className="ci-nav">
            <NavTabButton
              active={activeTab === 'explorer'}
              onClick={() => setActiveTab('explorer')}
              icon={<FolderTree className="w-4 h-4" />}
              label="File Manager"
            />
            <NavTabButton
              active={activeTab === 'connectivity'}
              onClick={() => setActiveTab('connectivity')}
              icon={<Sparkles className="w-4 h-4" />}
              label="Connectivity"
            />
            <NavTabButton
              active={activeTab === 'sibling'}
              onClick={() => setActiveTab('sibling')}
              icon={<HardDrive className="w-4 h-4" />}
              label="Sibling & Z:\ Mount"
            />
            <NavTabButton
              active={activeTab === 'about'}
              onClick={() => setActiveTab('about')}
              icon={<Info className="w-4 h-4" />}
              label="About"
            />
          </nav>

          {activeTab === 'explorer' && (
            <div className="flex items-center gap-2">
              {clipboard && (
                <div className="hidden lg:flex items-center gap-2 px-2.5 py-1 rounded-lg bg-emerald-500/10 border border-emerald-500/30 text-[11px] text-emerald-300">
                  <span>
                    Clipboard ({clipboard.op === 'move' ? 'Cut' : 'Copy'}):{' '}
                    <strong>{clipboard.names[0]}</strong>
                    {clipboard.names.length > 1 ? ` +${clipboard.names.length - 1}` : ''}
                  </span>
                  <button
                    onClick={() => setClipboard(null)}
                    className="text-slate-400 hover:text-white"
                  >
                    <X className="w-3 h-3" />
                  </button>
                </div>
              )}

              <div className="inline-flex rounded-xl border border-slate-800 bg-slate-900 p-0.5 text-xs">
                <button
                  onClick={() => setExplorerLayout('dual')}
                  className={`inline-flex items-center gap-1.5 px-2.5 py-1 rounded-lg font-medium transition ${
                    explorerLayout === 'dual'
                      ? 'bg-slate-800 text-white'
                      : 'text-slate-400 hover:text-slate-200'
                  }`}
                >
                  <Columns2 className="w-3.5 h-3.5 text-cyan-400" />
                  Dual-Pane (C:\ ⇄ Z:\)
                </button>
                <button
                  onClick={() => setExplorerLayout('single_z')}
                  className={`inline-flex items-center gap-1.5 px-2.5 py-1 rounded-lg font-medium transition ${
                    explorerLayout === 'single_z'
                      ? 'bg-slate-800 text-white'
                      : 'text-slate-400 hover:text-slate-200'
                  }`}
                >
                  <PanelLeft className="w-3.5 h-3.5 text-emerald-400" />
                  This PC Explorer (Z:\ Drive)
                </button>
              </div>
            </div>
          )}
        </div>
      </header>

      {/* Pending Approval Banner (Only appears if user turns Sibling Mode OFF!) */}
      {snapshot.pendingApprovals.length > 0 && (
        <div className="bg-amber-500/15 border-b border-amber-500/30 px-4 lg:px-6 py-2.5">
          <div className="max-w-[1600px] mx-auto flex flex-wrap items-center justify-between gap-2 text-xs">
            <span className="text-amber-200">
              <strong>Receiver Confirmation Waiting:</strong> Sibling Mode is currently OFF, so{' '}
              <strong>{snapshot.pendingApprovals[0].itemNames.join(', ')}</strong> requires phone approval.
            </span>
            <div className="flex items-center gap-2">
              <button
                onClick={async () => {
                  const res = await api.resolveApproval(snapshot.pendingApprovals[0].id, true);
                  setSnapshot(res.snapshot);
                  setRefreshToken((n) => n + 1);
                  notify('Approved transfer!', 'success');
                }}
                className="inline-flex items-center gap-1 px-3 py-1 rounded-lg bg-emerald-500 text-slate-950 font-semibold"
              >
                <Check className="w-3.5 h-3.5" /> Simulate Accept on Phone
              </button>
              <button
                onClick={() => void handleQuickEnableSibling()}
                className="px-3 py-1 rounded-lg bg-slate-800 hover:bg-slate-700 text-slate-200 font-medium"
              >
                Re-enable Sibling Mode (Never Ask Again)
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Main Content Workspace */}
      {/*
        The link can be "connected" and still serve nothing (radio napped, Sibling Mode off on the
        phone, Pairing Key mismatch). When that happens the engine reports exactly why, and we say so
        loudly here instead of letting the user stare at an empty Z:\ drive.
      */}
      {snapshot.linkProblem && (
        <div className="max-w-[1600px] w-full mx-auto px-4 lg:px-6 pt-4">
          <div className="ci-inset rounded-xl border border-[var(--ci-amber)]/40 bg-[var(--ci-amber)]/10 px-4 py-3 flex flex-wrap items-center gap-3">
            <span className="ci-dot ci-dot-warn shrink-0" />
            <div className="min-w-0 flex-1">
              <div className="text-xs font-bold text-amber-200">Sibling link is up but not serving files</div>
              <div className="text-[11px] text-amber-100/80 mt-0.5">{snapshot.linkProblem}</div>
            </div>
            <button
              type="button"
              className="ci-btn-ghost shrink-0"
              onClick={() => setRefreshToken((n) => n + 1)}
              title="Re-scan every transport now"
            >
              <RefreshCw className="w-3.5 h-3.5" />
              <span>Re-check</span>
            </button>
          </div>
        </div>
      )}

      <main className="flex-1 max-w-[1600px] w-full mx-auto px-4 lg:px-6 py-5">
        {activeTab === 'explorer' &&
          (explorerLayout === 'dual' ? (
            <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-stretch">
              <DrivePane
                target="pc"
                otherTarget="phone"
                otherCurrentDir={phoneDir}
                currentDir={pcDir}
                onNavigate={setPcDir}
                stats={snapshot.drives.pc}
                snapshot={snapshot}
                refreshToken={refreshToken}
                isFocused={focusedPane === 'pc'}
                onFocusPane={() => setFocusedPane('pc')}
                clipboard={clipboard}
                onSetClipboard={setClipboard}
                onPasteInto={handlePasteInto}
                onDirectTransfer={executeTransfer}
                onOpenFile={(target, item) => setPreviewFile({ target, item })}
                onQuickEnableSibling={() => void handleQuickEnableSibling()}
                notify={notify}
                sort={sort}
                onSortChange={handleSortChange}
              />

              <DrivePane
                target="phone"
                otherTarget="pc"
                otherCurrentDir={pcDir}
                currentDir={phoneDir}
                onNavigate={setPhoneDir}
                stats={snapshot.drives.phone}
                snapshot={snapshot}
                refreshToken={refreshToken}
                isFocused={focusedPane === 'phone'}
                onFocusPane={() => setFocusedPane('phone')}
                clipboard={clipboard}
                onSetClipboard={setClipboard}
                onPasteInto={handlePasteInto}
                onDirectTransfer={executeTransfer}
                onOpenFile={(target, item) => setPreviewFile({ target, item })}
                onQuickEnableSibling={() => void handleQuickEnableSibling()}
                notify={notify}
                sort={sort}
                onSortChange={handleSortChange}
              />
            </div>
          ) : (
            /* Windows 11 File Explorer "This PC" Single-Pane View with Drive Tree Sidebar */
            <div className="grid grid-cols-1 lg:grid-cols-12 gap-4">
              <aside className="lg:col-span-3 rounded-2xl border border-slate-800 bg-slate-900/90 p-3.5 space-y-4">
                <div>
                  <div className="text-[11px] font-semibold uppercase tracking-wider text-slate-500 px-2 mb-2">
                    This PC — Local & Sibling Drives
                  </div>
                  <div className="space-y-1.5">
                    <button
                      onClick={() => setSingleActiveTarget('phone')}
                      className={`w-full flex items-center justify-between p-2.5 rounded-xl border text-left transition ${
                        singleActiveTarget === 'phone'
                          ? 'bg-emerald-500/15 border-emerald-500/40 text-emerald-100'
                          : 'bg-slate-950/60 border-slate-800 text-slate-300 hover:border-slate-700'
                      }`}
                    >
                      <div className="flex items-center gap-2.5 min-w-0">
                        <Smartphone className="w-4 h-4 text-emerald-400 shrink-0" />
                        <div className="min-w-0">
                          <div className="text-xs font-semibold truncate">
                            Z:\ {snapshot.sibling.peerDeviceName}
                          </div>
                          <div className="text-[10px] text-emerald-400">
                            Sibling Auto-Drive • Screen-Off Ready
                          </div>
                        </div>
                      </div>
                    </button>

                    <button
                      onClick={() => setSingleActiveTarget('pc')}
                      className={`w-full flex items-center justify-between p-2.5 rounded-xl border text-left transition ${
                        singleActiveTarget === 'pc'
                          ? 'bg-cyan-500/15 border-cyan-500/40 text-cyan-100'
                          : 'bg-slate-950/60 border-slate-800 text-slate-300 hover:border-slate-700'
                      }`}
                    >
                      <div className="flex items-center gap-2.5 min-w-0">
                        <HardDrive className="w-4 h-4 text-cyan-400 shrink-0" />
                        <div className="min-w-0">
                          <div className="text-xs font-semibold truncate">C:\ Windows 11 SSD</div>
                          <div className="text-[10px] text-slate-400">Local Hard Disk</div>
                        </div>
                      </div>
                    </button>
                  </div>
                </div>

                <div>
                  <div className="text-[11px] font-semibold uppercase tracking-wider text-slate-500 px-2 mb-2">
                    Z:\ Phone Quick Access
                  </div>
                  <div className="space-y-1 text-xs">
                    {[
                      { name: 'Camera Roll (DCIM)', path: '/DCIM/Camera' },
                      { name: 'Screenshots', path: '/Pictures/Screenshots' },
                      { name: 'Downloads', path: '/Download' },
                      { name: 'Documents & Notes', path: '/Documents' },
                      { name: 'Music Library', path: '/Music' },
                      { name: 'ChainIt Inbox', path: '/ChainIt_Inbox' },
                    ].map((f) => (
                      <button
                        key={f.path}
                        onClick={() => {
                          setSingleActiveTarget('phone');
                          setPhoneDir(f.path);
                        }}
                        className={`w-full text-left px-3 py-1.5 rounded-lg transition ${
                          singleActiveTarget === 'phone' && phoneDir === f.path
                            ? 'bg-slate-800 text-emerald-300 font-medium'
                            : 'text-slate-400 hover:bg-slate-800/60 hover:text-slate-200'
                        }`}
                      >
                        Z:{f.path.replace(/\//g, '\\')} — {f.name}
                      </button>
                    ))}
                  </div>
                </div>
              </aside>

              <div className="lg:col-span-9">
                <DrivePane
                  target={singleActiveTarget}
                  otherTarget={singleActiveTarget === 'phone' ? 'pc' : 'phone'}
                  otherCurrentDir={singleActiveTarget === 'phone' ? pcDir : phoneDir}
                  currentDir={singleActiveTarget === 'phone' ? phoneDir : pcDir}
                  onNavigate={singleActiveTarget === 'phone' ? setPhoneDir : setPcDir}
                  stats={
                    singleActiveTarget === 'phone' ? snapshot.drives.phone : snapshot.drives.pc
                  }
                  snapshot={snapshot}
                  refreshToken={refreshToken}
                  isFocused={true}
                  onFocusPane={() => {}}
                  clipboard={clipboard}
                  onSetClipboard={setClipboard}
                  onPasteInto={handlePasteInto}
                  onDirectTransfer={executeTransfer}
                  onOpenFile={(target, item) => setPreviewFile({ target, item })}
                  onQuickEnableSibling={() => void handleQuickEnableSibling()}
                  notify={notify}
                  sort={sort}
                  onSortChange={handleSortChange}
                />
              </div>
            </div>
          ))}

        {activeTab === 'connectivity' && (
          <ConnectivityView snapshot={snapshot} onSnapshot={setSnapshot} notify={notify} />
        )}

        {activeTab === 'sibling' && (
          <SiblingAndMountView snapshot={snapshot} onSnapshot={setSnapshot} notify={notify} />
        )}

        {activeTab === 'about' && <AboutView snapshot={snapshot} />}
      </main>

      {/* Bottom Live Transfer & Protocol Telemetry Strip */}
      <footer className="border-t border-slate-800/90 bg-slate-950/95 px-4 lg:px-6 py-2.5">
        <div className="max-w-[1600px] mx-auto">
          <div className="flex items-center justify-between gap-2">
            <button
              onClick={() => setShowTelemetry((v) => !v)}
              className="inline-flex items-center gap-2 text-xs font-semibold text-slate-300 hover:text-white"
            >
              <Activity className="w-3.5 h-3.5 text-emerald-400" />
              <span>
                Live Transfer & Sibling Protocol Stream ({snapshot.transfers.length} transfers •{' '}
                {snapshot.logs.length} events)
              </span>
              <span className="text-[11px] text-slate-500">
                {showTelemetry ? '[Hide]' : '[Expand]'}
              </span>
            </button>

            <span className="text-[11px] font-mono text-slate-500 hidden sm:inline">
              WebDAV Z:\ Endpoint: /dav/ • Zero-Prompt Sibling:{' '}
              {snapshot.sibling.enabled ? 'ACTIVE' : 'PAUSED'}
            </span>
          </div>

          {showTelemetry && (
            <div className="mt-2.5 grid grid-cols-1 lg:grid-cols-2 gap-3 max-h-36 overflow-y-auto pr-1">
              {/* Recent shared files */}
              <div className="space-y-1">
                <div className="flex items-center justify-between gap-2 px-1">
                  <span className="text-[10px] uppercase tracking-wide text-slate-500">
                    Recent shared files · {snapshot.transfers.length} kept
                  </span>
                  {snapshot.transfers.length > 0 && (
                    <button
                      onClick={async () => {
                        try {
                          await api.clearHistory();
                          const next = await api.getState();
                          setSnapshot(next);
                          notify('Shared-file history cleared', 'info');
                        } catch (err) {
                          notify(err instanceof Error ? err.message : 'Could not clear history', 'error');
                        }
                      }}
                      className="text-[10px] text-slate-500 hover:text-rose-300 transition"
                      title="Removes every recorded share from the PC's history file"
                    >
                      Clear history
                    </button>
                  )}
                </div>
                {snapshot.transfers.length === 0 ? (
                  <div className="text-[11px] text-slate-500 py-2">
                    Drag & drop, Copy/Paste (Ctrl+C / Ctrl+V), or click "Copy to Z:\" above to see live
                    multi-transport transfer telemetry. The Windows engine keeps this list in
                    %APPDATA%\ChainIt, so it survives a restart.
                  </div>
                ) : (
                  snapshot.transfers.slice(0, 4).map((tr) => (
                    <div
                      key={tr.id}
                      className="flex items-center justify-between gap-2 px-3 py-1.5 rounded-lg bg-slate-900/90 border border-slate-800/80 text-[11px]"
                    >
                      <div className="flex items-center gap-2 min-w-0">
                        <ArrowRightLeft className="w-3.5 h-3.5 text-emerald-400 shrink-0" />
                        <span className="font-medium text-slate-200 truncate">{tr.label}</span>
                        {tr.source && (
                          <span
                            className="px-1.5 py-0.5 rounded bg-slate-800 text-slate-400 text-[10px] shrink-0"
                            title={sourceExplain(tr.source)}
                          >
                            {sourceLabel(tr.source)}
                          </span>
                        )}
                        {tr.virtualPath && (
                          <span className="font-mono text-[10px] text-slate-500 truncate" title={tr.virtualPath}>
                            {tr.virtualPath}
                          </span>
                        )}
                        {tr.promotedHotspot && (
                          <span className="px-1.5 py-0.5 rounded bg-amber-500/20 text-amber-300 text-[10px] font-semibold shrink-0">
                            BLE → 5GHz Boost
                          </span>
                        )}
                      </div>
                      <div className="font-mono text-slate-400 shrink-0">
                        {formatBytes(tr.bytes)} • {tr.transportUsed} ({tr.effectiveSpeedMBps} MB/s) •{' '}
                        {formatShortTime(tr.timestampMs)}
                      </div>
                    </div>
                  ))
                )}
              </div>

              {/* Protocol Logs */}
              <div className="space-y-1">
                {snapshot.logs.slice(0, 4).map((lg) => (
                  <div
                    key={lg.id}
                    className="flex items-center justify-between gap-2 px-3 py-1.5 rounded-lg bg-slate-900/60 border border-slate-800/60 text-[11px]"
                  >
                    <div className="flex items-center gap-2 min-w-0">
                      <span className="font-mono text-[10px] px-1.5 py-0.5 rounded bg-slate-800 text-cyan-300 shrink-0">
                        {lg.layer}
                      </span>
                      <span className="text-slate-300 truncate" title={lg.detail}>
                        {lg.summary}
                      </span>
                    </div>
                    <span className="font-mono text-[10px] text-slate-500 shrink-0">
                      {formatShortTime(lg.timestampMs)}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          )}
        </div>
      </footer>

      {/* Inline File Preview / Direct Z:\ Editor Modal */}
      {previewFile && (
        <FilePreviewModal
          target={previewFile.target}
          item={previewFile.item}
          phoneScreenLocked={snapshot.environment.phoneScreenLocked}
          onClose={() => setPreviewFile(null)}
          onSaved={() => setRefreshToken((n) => n + 1)}
          onCopyToOtherDrive={(fromTarget, item) => {
            const toTarget: DriveTarget = fromTarget === 'phone' ? 'pc' : 'phone';
            const toDir = toTarget === 'phone' ? phoneDir : pcDir;
            void executeTransfer({
              op: 'copy',
              fromTarget,
              fromPaths: [item.path],
              toTarget,
              toDir,
            });
          }}
          notify={notify}
        />
      )}

      {showDownloads && <DownloadsModal onClose={() => setShowDownloads(false)} />}

      <ToastStack toasts={toasts} />
    </div>
  );
};

const NavTabButton: React.FC<{
  active: boolean;
  onClick: () => void;
  icon: React.ReactNode;
  label: string;
}> = ({ active, onClick, icon, label }) => (
  <button onClick={onClick} className="ci-nav-btn" data-active={active} type="button">
    <span className="ci-nav-icon">{icon}</span>
    <span>{label}</span>
  </button>
);

const ToastStack: React.FC<{ toasts: ToastItem[] }> = ({ toasts }) => (
  <div className="fixed bottom-4 right-4 z-50 flex flex-col gap-2 pointer-events-none">
    {toasts.map((t) => (
      <div
        key={t.id}
        className={`ci-toast ${
          t.kind === 'success'
            ? 'bg-[rgba(6,46,34,0.94)] border-[rgba(16,185,129,0.45)] text-emerald-100'
            : t.kind === 'error'
              ? 'bg-[rgba(58,15,26,0.94)] border-[rgba(244,63,94,0.45)] text-rose-100'
              : 'bg-[rgba(14,20,32,0.96)] border-[var(--ci-border-strong)] text-slate-100'
        }`}
      >
        {t.message}
      </div>
    ))}
  </div>
);
