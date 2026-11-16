use serde::{Deserialize, Serialize};
use crate::transport::TransportKind;

pub const PROTOCOL_VERSION: &str = "CHNP/1.0";
pub const DEFAULT_CHUNK_SIZE: usize = 1024 * 1024; // 1 MiB resumable chunks

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum VfsEntryKind {
    File,
    Directory,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VfsEntry {
    pub name: String,
    pub path: String,
    pub kind: VfsEntryKind,
    pub size_bytes: u64,
    pub modified_unix_ms: u64,
    pub mime_type: Option<String>,
}

/// Wire protocol frames exchanged over any active ChainIt transport
/// (USB RNDIS, Wi-Fi LAN, Local Hotspot, Bluetooth RFCOMM, or Pluggable Online P2P).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "frame", rename_all = "snake_case")]
pub enum ChnpFrame {
    /// Initial capability and identity exchange.
    Hello {
        protocol_version: String,
        device_id: String,
        device_name: String,
        ed25519_pubkey_hex: String,
        challenge_nonce_hex: String,
    },
    /// Mutual Sibling authentication proving possession of paired Ed25519 key.
    SiblingAuth {
        device_id: String,
        signature_hex: String,
        sibling_mode_enabled: bool,
        auto_mount_drive: bool,
    },
    /// Sent over BLE/RFCOMM to instruct Android to spin up a 5 GHz LocalOnlyHotspot
    /// when no shared Wi-Fi router is available and high-speed VFS/transfer is needed.
    PromoteLocalHotspotRequest {
        reason: String,
        expected_bytes: u64,
    },
    /// Ephemeral 5 GHz SoftAP credentials returned over encrypted BLE GATT.
    PromoteLocalHotspotReady {
        ssid: String,
        passphrase: String,
        host_ip: String,
        port: u16,
    },
    /// Seamless mid-transfer transport migration announcement.
    SwitchTransport {
        transfer_id: Option<String>,
        target_transport: TransportKind,
        resume_chunk_index: u64,
    },
    /// Virtual File System (Z:\ Drive & ChainIt File Manager) RPCs.
    VfsReadDirRequest {
        req_id: u64,
        path: String,
    },
    VfsReadDirResponse {
        req_id: u64,
        entries: Vec<VfsEntry>,
    },
    VfsStatRequest {
        req_id: u64,
        path: String,
    },
    VfsStatResponse {
        req_id: u64,
        entry: Option<VfsEntry>,
    },
    VfsReadChunkRequest {
        req_id: u64,
        path: String,
        offset: u64,
        length: u32,
    },
    VfsReadChunkResponse {
        req_id: u64,
        offset: u64,
        blake3_hex: String,
        payload_b64: String,
        eof: bool,
    },
    VfsWriteChunkRequest {
        req_id: u64,
        path: String,
        offset: u64,
        truncate_first: bool,
        blake3_hex: String,
        payload_b64: String,
    },
    VfsMoveRequest {
        req_id: u64,
        from_path: String,
        to_path: String,
        overwrite: bool,
    },
    VfsMkdirRequest {
        req_id: u64,
        path: String,
    },
    VfsDeleteRequest {
        req_id: u64,
        path: String,
        recursive: bool,
    },
    VfsAck {
        req_id: u64,
        ok: bool,
        error: Option<String>,
    },
}
