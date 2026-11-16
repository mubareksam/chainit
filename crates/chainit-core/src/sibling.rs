use hmac::{Hmac, Mac};
use serde::{Deserialize, Serialize};
use sha2::Sha256;

type HmacSha256 = Hmac<Sha256>;

/// 15-minute beacon rotation window so nearby strangers cannot fingerprint a static BLE MAC/tag.
pub const BEACON_EPOCH_SECONDS: u64 = 900;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DeviceRole {
    WindowsLaptop = 1,
    AndroidPhone = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SiblingState {
    Unpaired,
    PairedStandard,
    SiblingStandbyBle,
    SiblingConnectedMounted,
}

/// Persistent mutual trust bond enabling zero-touch auto-connect, zero-prompt transfers,
/// and unattended Windows Z:\ Virtual File System mounting even when the phone screen is off.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SiblingBond {
    pub peer_device_id: String,
    pub peer_device_name: String,
    pub peer_ed25519_pubkey_hex: String,
    pub sibling_mode_enabled: bool,
    pub auto_mount_drive_letter: char,
    pub allow_screen_off_vfs: bool,
    pub zero_prompt_transfers: bool,
    /// Shared 32-byte secret derived via X25519 + HKDF during initial pairing.
    beacon_secret: [u8; 32],
}

impl SiblingBond {
    pub fn new(
        peer_device_id: String,
        peer_device_name: String,
        peer_ed25519_pubkey_hex: String,
        beacon_secret: [u8; 32],
    ) -> Self {
        Self {
            peer_device_id,
            peer_device_name,
            peer_ed25519_pubkey_hex,
            sibling_mode_enabled: true,
            auto_mount_drive_letter: 'Z',
            allow_screen_off_vfs: true,
            zero_prompt_transfers: true,
            beacon_secret,
        }
    }

    /// Computes an 8-byte privacy-preserving BLE wakeup tag for the given 15-minute epoch.
    pub fn compute_ble_beacon_tag(&self, unix_seconds: u64, sender_role: DeviceRole) -> [u8; 8] {
        let epoch = unix_seconds / BEACON_EPOCH_SECONDS;
        self.compute_tag_for_epoch(epoch, sender_role)
    }

    fn compute_tag_for_epoch(&self, epoch: u64, sender_role: DeviceRole) -> [u8; 8] {
        let mut mac =
            HmacSha256::new_from_slice(&self.beacon_secret).expect("32-byte HMAC key is valid");
        mac.update(&epoch.to_be_bytes());
        mac.update(&[sender_role as u8]);
        let digest = mac.finalize().into_bytes();
        let mut out = [0u8; 8];
        out.copy_from_slice(&digest[..8]);
        out
    }

    /// Verifies an incoming BLE wakeup advertisement against `[epoch - 1, epoch, epoch + 1]`
    /// to tolerate up to +-15 minutes of device clock drift while offline.
    pub fn verify_ble_beacon_tag(
        &self,
        observed_tag: &[u8; 8],
        now_unix_seconds: u64,
        expected_sender: DeviceRole,
    ) -> bool {
        if !self.sibling_mode_enabled {
            return false;
        }
        let current_epoch = now_unix_seconds / BEACON_EPOCH_SECONDS;
        for candidate in [
            current_epoch.saturating_sub(1),
            current_epoch,
            current_epoch + 1,
        ] {
            if &self.compute_tag_for_epoch(candidate, expected_sender) == observed_tag {
                return true;
            }
        }
        false
    }

    /// Returns whether incoming transfers or VFS requests bypass interactive user prompts.
    pub fn authorizes_unattended_vfs(&self) -> bool {
        self.sibling_mode_enabled && self.allow_screen_off_vfs
    }
}
