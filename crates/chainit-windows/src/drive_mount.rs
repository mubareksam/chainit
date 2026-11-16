use serde::{Deserialize, Serialize};
use std::process::Command;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WindowsMountBackend {
    /// Tier 1: High-speed user-mode filesystem volume via WinFsp kernel driver.
    WinFspKernelVolume,
    /// Tier 2: Zero-driver built-in Windows Explorer WebDAV loopback mount (`net use Z:`).
    WebDavLoopback,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SiblingDriveMountConfig {
    pub drive_letter: char,
    pub volume_label: String,
    pub webdav_loopback_url: String,
    pub preferred_backend: WindowsMountBackend,
}

impl Default for SiblingDriveMountConfig {
    fn default() -> Self {
        Self {
            drive_letter: 'Z',
            volume_label: "ChainIt Sibling Phone".to_string(),
            webdav_loopback_url: "http://127.0.0.1:4173/dav".to_string(),
            preferred_backend: WindowsMountBackend::WebDavLoopback,
        }
    }
}

/// Manages mounting and unmounting the paired Android Sibling storage as a real Windows drive (Z:\)
/// so File Explorer supports native Ctrl+C, Ctrl+V, Cut/Move, and Drag-and-Drop without waking the phone.
pub struct WindowsDriveMounter {
    pub config: SiblingDriveMountConfig,
    pub is_mounted: bool,
}

impl WindowsDriveMounter {
    pub fn new(config: SiblingDriveMountConfig) -> Self {
        Self {
            config,
            is_mounted: false,
        }
    }

    pub fn winfsp_installed() -> bool {
        #[cfg(target_os = "windows")]
        {
            std::path::Path::new(r"C:\Program Files (x86)\WinFsp\bin\winfsp-x64.dll").exists()
                || std::path::Path::new(r"C:\Program Files\WinFsp\bin\winfsp-x64.dll").exists()
        }
        #[cfg(not(target_os = "windows"))]
        {
            false
        }
    }

    /// Automatically mounts `Z:\` when the Sibling phone enters range and connects.
    pub fn mount_sibling_drive(&mut self) -> Result<WindowsMountBackend, String> {
        let drive_target = format!("{}:", self.config.drive_letter.to_ascii_uppercase());

        #[cfg(target_os = "windows")]
        {
            // Clean up stale drive mapping if disconnected abruptly earlier
            let _ = Command::new("net")
                .args(["use", &drive_target, "/delete", "/y"])
                .output();

            // Map local ChainIt WebDAV VFS server directly to Z:\ in Windows Explorer
            let status = Command::new("net")
                .args([
                    "use",
                    &drive_target,
                    &self.config.webdav_loopback_url,
                    "/persistent:no",
                ])
                .output()
                .map_err(|e| format!("Failed to invoke Windows net.exe: {e}"))?;

            if !status.status.success() {
                let stderr = String::from_utf8_lossy(&status.stderr);
                return Err(format!(
                    "Failed to map {} to {}: {}",
                    drive_target, self.config.webdav_loopback_url, stderr
                ));
            }

            self.is_mounted = true;
            return Ok(WindowsMountBackend::WebDavLoopback);
        }

        #[cfg(not(target_os = "windows"))]
        {
            let _ = drive_target;
            self.is_mounted = true;
            Ok(WindowsMountBackend::WebDavLoopback)
        }
    }

    pub fn unmount_sibling_drive(&mut self) -> Result<(), String> {
        let drive_target = format!("{}:", self.config.drive_letter.to_ascii_uppercase());

        #[cfg(target_os = "windows")]
        {
            let _ = Command::new("net")
                .args(["use", &drive_target, "/delete", "/y"])
                .output();
        }

        #[cfg(not(target_os = "windows"))]
        {
            let _ = drive_target;
        }

        self.is_mounted = false;
        Ok(())
    }
}
