mod drive_mount;

use chainit_core::sibling::{DeviceRole, SiblingBond};
use chainit_core::transport::{LinkTelemetry, SelectionMode, SmartTransportSelector, TransportKind};
use drive_mount::{SiblingDriveMountConfig, WindowsDriveMounter};
use std::collections::HashMap;

#[tokio::main]
async fn main() {
    println!("=== ChainIt Windows Sibling Daemon (CHNP/1.0) ===");

    let bond = SiblingBond::new(
        "android-s24-sibling".to_string(),
        "Galaxy S24 Ultra".to_string(),
        "9f82c1d4b8e704a1e3302f9d118a4c5b22e0f41998a173c46b5508e29d01a377".to_string(),
        [0x42; 32],
    );

    let now_secs = 1_794_830_000_u64;
    let beacon = bond.compute_ble_beacon_tag(now_secs, DeviceRole::AndroidPhone);
    println!(
        "[Sibling] Listening for rotating BLE wakeup tag {:02x?} (screen-off VFS: {})",
        beacon,
        bond.authorizes_unattended_vfs()
    );

    let mut selector = SmartTransportSelector {
        mode: SelectionMode::Automatic,
        ..Default::default()
    };

    let mut telemetry = HashMap::new();
    telemetry.insert(
        TransportKind::WifiLan,
        LinkTelemetry {
            kind: TransportKind::WifiLan,
            available: true,
            can_bootstrap_on_demand: false,
            estimated_mbps_bytes: 94.5,
            rtt_ms: 2.6,
            packet_loss_ratio: 0.001,
            signal_strength_factor: 0.96,
            power_cost_index: 15.0,
        },
    );
    telemetry.insert(
        TransportKind::LocalHotspot,
        LinkTelemetry {
            kind: TransportKind::LocalHotspot,
            available: false,
            can_bootstrap_on_demand: true,
            estimated_mbps_bytes: 72.0,
            rtt_ms: 3.8,
            packet_loss_ratio: 0.001,
            signal_strength_factor: 0.98,
            power_cost_index: 28.0,
        },
    );
    telemetry.insert(
        TransportKind::Bluetooth,
        LinkTelemetry {
            kind: TransportKind::Bluetooth,
            available: true,
            can_bootstrap_on_demand: false,
            estimated_mbps_bytes: 2.1,
            rtt_ms: 18.0,
            packet_loss_ratio: 0.005,
            signal_strength_factor: 0.92,
            power_cost_index: 8.0,
        },
    );

    let decision = selector.evaluate(&telemetry, 0);
    println!(
        "[Transport] Automatic mode selected {:?} (rankings: {:?})",
        decision.active_transport,
        decision
            .rankings
            .iter()
            .map(|r| (r.kind.display_name(), format!("{:.1}", r.score)))
            .collect::<Vec<_>>()
    );

    let mut mounter = WindowsDriveMounter::new(SiblingDriveMountConfig::default());
    if bond.authorizes_unattended_vfs() {
        match mounter.mount_sibling_drive() {
            Ok(backend) => println!(
                "[VFS] Auto-mounted Sibling phone storage at {}: via {:?}",
                mounter.config.drive_letter, backend
            ),
            Err(err) => eprintln!("[VFS] Mount warning: {err}"),
        }
    }
}
