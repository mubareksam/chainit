import React from 'react';
import { CheckCircle2, Download, Laptop, ShieldCheck, Smartphone, X } from 'lucide-react';

interface Props {
  onClose: () => void;
}

export const DownloadsModal: React.FC<Props> = ({ onClose }) => {
  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/80 backdrop-blur-sm p-4"
      onClick={onClose}
    >
      <div
        className="w-full max-w-2xl rounded-2xl border border-slate-800 bg-slate-900 shadow-2xl overflow-hidden"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-5 py-4 border-b border-slate-800 bg-slate-950/70">
          <div className="flex items-center gap-2.5">
            <ShieldCheck className="w-5 h-5 text-emerald-400" />
            <div>
              <h2 className="text-base font-bold text-white">
                Download ChainIt Final Installers (.exe & .apk)
              </h2>
              <p className="text-xs text-slate-400">
                Standalone zero-dependency binaries for Windows 10/11 & Android 8–15
              </p>
            </div>
          </div>
          <button
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-white hover:bg-slate-800"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="p-5 space-y-5 max-h-[82vh] overflow-y-auto">
          {/* Two Download Cards */}
          <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
            <div className="rounded-2xl border border-cyan-500/30 bg-cyan-950/15 p-4 flex flex-col justify-between space-y-4">
              <div className="space-y-2">
                <div className="w-10 h-10 rounded-xl bg-cyan-500/20 text-cyan-400 flex items-center justify-center">
                  <Laptop className="w-5 h-5" />
                </div>
                <h3 className="text-sm font-bold text-white">ChainIt for Windows (64-bit)</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Single portable <code className="text-cyan-300">ChainIt-Windows.exe</code>. Includes
                  System Tray daemon, Smart Link auto-discovery, automatic <code className="text-cyan-300">Z:\</code>{' '}
                  Explorer drive mounter, and the dedicated ChainIt File Manager.
                </p>
              </div>
              <a
                href="/downloads/ChainIt-Windows.exe"
                download="ChainIt-Windows.exe"
                className="w-full py-2.5 px-4 rounded-xl bg-cyan-500 hover:bg-cyan-400 text-slate-950 font-bold text-xs flex items-center justify-center gap-2 transition shadow"
              >
                <Download className="w-4 h-4" />
                Download ChainIt-Windows.exe
              </a>
            </div>

            <div className="rounded-2xl border border-emerald-500/30 bg-emerald-950/15 p-4 flex flex-col justify-between space-y-4">
              <div className="space-y-2">
                <div className="w-10 h-10 rounded-xl bg-emerald-500/20 text-emerald-400 flex items-center justify-center">
                  <Smartphone className="w-5 h-5" />
                </div>
                <h3 className="text-sm font-bold text-white">ChainIt Sibling for Android</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Signed <code className="text-emerald-300">ChainIt-Android.apk</code> (Android 8–15).
                  Runs screen-off background Sibling VFS over USB Tethering, Wi-Fi LAN, or Local Hotspot
                  with zero prompts.
                </p>
              </div>
              <a
                href="/downloads/ChainIt-Android.apk"
                download="ChainIt-Android.apk"
                className="w-full py-2.5 px-4 rounded-xl bg-emerald-500 hover:bg-emerald-400 text-slate-950 font-bold text-xs flex items-center justify-center gap-2 transition shadow"
              >
                <Download className="w-4 h-4" />
                Download ChainIt-Android.apk
              </a>
            </div>
          </div>

          {/* 3-Minute Setup Checklist */}
          <div className="rounded-xl border border-slate-800 bg-slate-950/80 p-4 space-y-3 text-xs">
            <h4 className="font-semibold text-slate-100 flex items-center gap-2">
              <CheckCircle2 className="w-4 h-4 text-emerald-400" />
              One-Time Setup Steps (Never Needed Again Once Configured)
            </h4>
            <ol className="space-y-2 text-slate-300 list-decimal list-inside leading-relaxed">
              <li>
                <strong className="text-white">On Android:</strong> Install{' '}
                <code className="text-emerald-300">ChainIt-Android.apk</code>, open it once, and tap the two
                green setup buttons: <em>Grant All-Files Access</em> (required for <code className="text-emerald-300">Z:\</code>)
                and <em>Allow Screen-Off Standby</em>. Leave <strong>Sibling Mode ON</strong>.
              </li>
              <li>
                <strong className="text-white">On Windows 10/11:</strong> Run{' '}
                <code className="text-cyan-300">ChainIt-Windows.exe</code> (if Windows SmartScreen asks, click{' '}
                <em>More info → Run anyway</em>, and click <em>Allow access</em> on Windows Firewall so UDP
                auto-discovery works on Wi-Fi & Hotspot).
              </li>
              <li>
                <strong className="text-white">Connect Offline or LAN:</strong> Put both devices on the same
                Wi-Fi, OR turn on <strong>Phone Hotspot / Windows Mobile Hotspot</strong> (100% offline, zero
                internet needed), OR plug in a <strong>USB cable + USB Tethering</strong> (420 MB/s). ChainIt
                auto-discovers your phone, mounts <code className="text-emerald-300">Z:\</code> in File Explorer,
                and lets you browse/copy/paste even with your phone screen locked!
              </li>
            </ol>
          </div>
        </div>
      </div>
    </div>
  );
};
