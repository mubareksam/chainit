use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;

/// Supported ChainIt transport channels (Offline-first + Pluggable Online).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TransportKind {
    /// USB Tethering / RNDIS / ADB port-forward (~350-480 MB/s, <1ms, zero battery drain).
    UsbRndis,
    /// Shared local Wi-Fi router discovered via mDNS / UDP beacon (~45-115 MB/s, 2-6ms).
    WifiLan,
    /// Router-free 5 GHz Android LocalOnlyHotspot / Wi-Fi Direct bootstrapped over BLE (~35-85 MB/s).
    LocalHotspot,
    /// Bluetooth Low Energy wakeup/control + RFCOMM fallback stream (~1.8-2.4 MB/s).
    Bluetooth,
    /// Pluggable Online P2P transport (QUIC / WebRTC hole-punching + encrypted relay fallback).
    OnlineP2p,
}

impl TransportKind {
    pub fn is_offline_capable(&self) -> bool {
        !matches!(self, Self::OnlineP2p)
    }

    pub fn display_name(&self) -> &'static str {
        match self {
            Self::UsbRndis => "USB Direct (RNDIS / ADB)",
            Self::WifiLan => "Wi-Fi LAN (5 GHz mDNS)",
            Self::LocalHotspot => "Local Hotspot (BLE Bootstrap)",
            Self::Bluetooth => "Bluetooth (BLE + RFCOMM)",
            Self::OnlineP2p => "Online P2P (QUIC / WebRTC)",
        }
    }
}

/// User-controlled transport selection mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "mode", content = "pinned", rename_all = "snake_case")]
pub enum SelectionMode {
    /// Automatically scores all available links and switches/promotes seamlessly.
    Automatic,
    /// Pins ChainIt to a user-selected transport.
    Manual(TransportKind),
}

/// Real-time link telemetry measured by each transport adapter.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LinkTelemetry {
    pub kind: TransportKind,
    pub available: bool,
    /// True if hardware can bring the link up on demand (e.g., BLE -> 5 GHz Local Hotspot).
    pub can_bootstrap_on_demand: bool,
    pub estimated_mbps_bytes: f64,
    pub rtt_ms: f64,
    pub packet_loss_ratio: f64,
    pub signal_strength_factor: f64, // 0.0 ..= 1.0
    pub power_cost_index: f64,       // 0.0 (charging/USB) ..= 100.0 (metered cellular)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScoredTransport {
    pub kind: TransportKind,
    pub score: f64,
    pub available: bool,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SelectionDecision {
    pub active_transport: Option<TransportKind>,
    pub promote_local_hotspot: bool,
    pub rankings: Vec<ScoredTransport>,
}

/// Pluggable async trait implemented by offline transports (USB, LAN, Hotspot, Bluetooth)
/// and future online transports (WebRTC / QUIC P2P + DERP relay).
pub trait TransportAdapter: Send + Sync {
    fn kind(&self) -> TransportKind;
    fn probe_telemetry(&self) -> LinkTelemetry;
    fn send_frame<'a>(
        &'a self,
        payload: &'a [u8],
    ) -> Pin<Box<dyn Future<Output = Result<(), String>> + Send + 'a>>;
}

/// Evaluates available links and decides which transport to use or promote.
pub struct SmartTransportSelector {
    pub mode: SelectionMode,
    pub current_active: Option<TransportKind>,
    pub hysteresis_bonus: f64,
    pub hotspot_promotion_threshold_bytes: u64,
}

impl Default for SmartTransportSelector {
    fn default() -> Self {
        Self {
            mode: SelectionMode::Automatic,
            current_active: None,
            hysteresis_bonus: 6.0,
            hotspot_promotion_threshold_bytes: 1024 * 1024, // 1 MiB
        }
    }
}

impl SmartTransportSelector {
    pub fn score_link(&self, link: &LinkTelemetry) -> f64 {
        if !link.available {
            return 0.0;
        }
        let bw_score =
            100.0 * ((link.estimated_mbps_bytes + 1.0).log10() / 500.0_f64.log10()).clamp(0.0, 1.0);
        let latency_score = 100.0 * (1.0 - (link.rtt_ms / 150.0)).clamp(0.0, 1.0);
        let reliability_score = 100.0
            * (1.0 - link.packet_loss_ratio.clamp(0.0, 1.0))
            * link.signal_strength_factor.clamp(0.0, 1.0);
        let power_penalty = link.power_cost_index.clamp(0.0, 100.0);

        let mut composite = 0.45 * bw_score
            + 0.25 * latency_score
            + 0.20 * reliability_score
            - 0.10 * power_penalty;

        if self.current_active == Some(link.kind) {
            composite += self.hysteresis_bonus;
        }

        composite.clamp(0.0, 100.0)
    }

    pub fn evaluate(
        &mut self,
        telemetry_map: &HashMap<TransportKind, LinkTelemetry>,
        pending_transfer_bytes: u64,
    ) -> SelectionDecision {
        let mut rankings: Vec<ScoredTransport> = telemetry_map
            .values()
            .map(|t| {
                let score = self.score_link(t);
                let reason = if !t.available && t.can_bootstrap_on_demand {
                    "Standby (can auto-bootstrap over BLE)".to_string()
                } else if !t.available {
                    "Link offline / unreachable".to_string()
                } else {
                    format!(
                        "{:.1} MB/s, {:.1} ms RTT",
                        t.estimated_mbps_bytes, t.rtt_ms
                    )
                };
                ScoredTransport {
                    kind: t.kind,
                    score,
                    available: t.available,
                    reason,
                }
            })
            .collect();

        rankings.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));

        match self.mode {
            SelectionMode::Manual(pinned) => {
                let is_up = telemetry_map
                    .get(&pinned)
                    .map(|t| t.available)
                    .unwrap_or(false);
                let chosen = if is_up { Some(pinned) } else { None };
                self.current_active = chosen;
                SelectionDecision {
                    active_transport: chosen,
                    promote_local_hotspot: false,
                    rankings,
                }
            }
            SelectionMode::Automatic => {
                let best = rankings.iter().find(|r| r.available).map(|r| r.kind);

                // Check if we are currently falling back to Bluetooth with no LAN/USB,
                // and a large file transfer (>= 1 MiB) should auto-promote 5 GHz Local Hotspot via BLE!
                let hotspot_standby = telemetry_map
                    .get(&TransportKind::LocalHotspot)
                    .map(|h| !h.available && h.can_bootstrap_on_demand)
                    .unwrap_or(false);

                let promote_local_hotspot = best == Some(TransportKind::Bluetooth)
                    && hotspot_standby
                    && pending_transfer_bytes >= self.hotspot_promotion_threshold_bytes;

                self.current_active = best;
                SelectionDecision {
                    active_transport: best,
                    promote_local_hotspot,
                    rankings,
                }
            }
        }
    }
}
