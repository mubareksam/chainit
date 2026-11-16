use serde::{Deserialize, Serialize};
use crate::protocol::DEFAULT_CHUNK_SIZE;
use crate::transport::TransportKind;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TransferDirection {
    PcToPhone,
    PhoneToPc,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChunkTransferSession {
    pub transfer_id: String,
    pub direction: TransferDirection,
    pub source_path: String,
    pub destination_path: String,
    pub total_bytes: u64,
    pub chunk_size_bytes: usize,
    pub completed_chunks: u64,
    pub active_transport: TransportKind,
    pub transport_switches: Vec<TransportKind>,
}

impl ChunkTransferSession {
    pub fn new(
        transfer_id: String,
        direction: TransferDirection,
        source_path: String,
        destination_path: String,
        total_bytes: u64,
        initial_transport: TransportKind,
    ) -> Self {
        Self {
            transfer_id,
            direction,
            source_path,
            destination_path,
            total_bytes,
            chunk_size_bytes: DEFAULT_CHUNK_SIZE,
            completed_chunks: 0,
            active_transport: initial_transport,
            transport_switches: vec![initial_transport],
        }
    }

    pub fn total_chunks(&self) -> u64 {
        if self.total_bytes == 0 {
            return 1;
        }
        self.total_bytes.div_ceil(self.chunk_size_bytes as u64)
    }

    pub fn next_byte_offset(&self) -> u64 {
        (self.completed_chunks * self.chunk_size_bytes as u64).min(self.total_bytes)
    }

    pub fn progress_ratio(&self) -> f64 {
        if self.total_bytes == 0 {
            return 1.0;
        }
        (self.next_byte_offset() as f64 / self.total_bytes as f64).clamp(0.0, 1.0)
    }

    pub fn verify_and_advance_chunk(&mut self, payload: &[u8], expected_blake3_hex: &str) -> bool {
        let digest = blake3::hash(payload);
        if digest.to_hex().as_str() == expected_blake3_hex {
            self.completed_chunks += 1;
            true
        } else {
            false
        }
    }

    /// Seamlessly switches the transport pipe at a chunk boundary without losing progress.
    pub fn migrate_transport(&mut self, next_transport: TransportKind) -> u64 {
        if self.active_transport != next_transport {
            self.active_transport = next_transport;
            self.transport_switches.push(next_transport);
        }
        self.completed_chunks
    }
}
