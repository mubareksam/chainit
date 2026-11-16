package com.chainit.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import com.chainit.android.R
import com.chainit.android.net.BluetoothRfcommTransport
import com.chainit.android.net.DiscoveredPcInfo
import com.chainit.android.net.LocalLinkEndpoint
import com.chainit.android.net.PhoneHttpAndWebDavServer
import com.chainit.android.net.UdpSiblingDiscovery
import com.chainit.android.ui.MainActivity
import com.chainit.android.vfs.AndroidStorageVfsProvider
import java.io.File

/**
 * Screen-off Sibling daemon: serves the phone VFS/WebDAV endpoint on every local link
 * (USB tethering, Wi-Fi LAN, hotspot, native Bluetooth RFCOMM) and keeps the process alive with a
 * low-importance foreground notification.
 *
 * Robustness rule: nothing here is allowed to take the process down. The foreground notification is
 * raised *first* (so Android can never kill us with ForegroundServiceDidNotStartInTimeException) and
 * every subsystem is started inside its own guard, with its state exposed to the UI.
 */
class SiblingDaemonService : Service() {

    companion object {
        const val CHANNEL_ID = "chainit_sibling_channel"
        const val NOTIFICATION_ID = 4918
        const val VFS_PORT = 49180
        const val DISCOVERY_PORT = 49179

        @Volatile
        var isRunning: Boolean = false
            private set

        @Volatile
        private var activeServer: PhoneHttpAndWebDavServer? = null

        @Volatile
        private var activeDiscovery: UdpSiblingDiscovery? = null

        @Volatile
        private var activeBluetooth: BluetoothRfcommTransport? = null

        @Volatile
        var vfsReady: Boolean = false
            private set

        @Volatile
        var httpReady: Boolean = false
            private set

        @Volatile
        var discoveryReady: Boolean = false
            private set

        @Volatile
        var startupError: String? = null
            private set

        fun getLastVfsSummary(): String =
            activeServer?.lastActivitySummary ?: if (isRunning) "Idle — waiting for Windows request" else "Service stopped"

        fun getEndpoints(context: Context): List<LocalLinkEndpoint> =
            try {
                activeDiscovery?.enumerateEndpoints() ?: UdpSiblingDiscovery(context).enumerateEndpoints()
            } catch (_: Exception) {
                emptyList()
            }

        fun getDiscoveredPc(): DiscoveredPcInfo? = activeDiscovery?.lastDiscoveredPc

        fun isBluetoothListening(): Boolean = activeBluetooth?.isListeningRfcomm == true

        fun getLastBluetoothPeer(): String? = activeBluetooth?.lastBtPeerName

        fun start(ctx: Context) {
            try {
                val intent = Intent(ctx, SiblingDaemonService::class.java)
                if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(intent) else ctx.startService(intent)
            } catch (t: Throwable) {
                startupError = "Service start blocked: ${t.javaClass.simpleName}"
            }
        }
    }

    private var vfs: AndroidStorageVfsProvider? = null
    private var httpServer: PhoneHttpAndWebDavServer? = null
    private var discovery: UdpSiblingDiscovery? = null
    private var btTransport: BluetoothRfcommTransport? = null

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    override fun onCreate() {
        try {
            super.onCreate()
        } catch (_: Throwable) {
        }

        // 1. Raise the foreground notification before doing any work at all.
        var foregroundOk = false
        try {
            ensureChannel()
            val notification = buildForegroundNotification("Starting Sibling link…")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            foregroundOk = true
        } catch (t: Throwable) {
            startupError = "Foreground notification refused: ${t.javaClass.simpleName} ${t.message}"
            // Fall back to a bare notification so the service can still run in the background.
            try {
                ensureChannel()
                startForeground(
                    NOTIFICATION_ID,
                    Notification.Builder(this, CHANNEL_ID)
                        .setContentTitle(getString(R.string.app_name))
                        .setSmallIcon(R.drawable.ic_stat_chainit)
                        .build()
                )
                foregroundOk = true
            } catch (_: Throwable) {
            }
        }

        // 2. Storage provider (never throws: falls back to the app-private dir).
        try {
            val provider = AndroidStorageVfsProvider()
            vfs = provider
            vfsReady = true
            runCatching { File(provider.storageRoot, "ChainIt_Inbox").mkdirs() }
        } catch (t: Throwable) {
            startupError = (startupError ?: "") + " Storage: ${t.javaClass.simpleName};"
            vfsReady = false
        }

        // 3. HTTP + WebDAV endpoint for Z:\ (binds on its own thread).
        val storage = vfs
        if (storage != null) {
            try {
                val server = PhoneHttpAndWebDavServer(applicationContext, storage, VFS_PORT)
                httpServer = server
                server.start()
                activeServer = server
                httpReady = true
            } catch (t: Throwable) {
                startupError = (startupError ?: "") + " VFS server: ${t.javaClass.simpleName};"
                httpReady = false
            }
        }

        // 4. UDP Sibling discovery (LAN / hotspot / USB broadcast).
        try {
            val d = UdpSiblingDiscovery(applicationContext, VFS_PORT, DISCOVERY_PORT)
            discovery = d
            d.start()
            activeDiscovery = d
            discoveryReady = true
        } catch (t: Throwable) {
            startupError = (startupError ?: "") + " Discovery: ${t.javaClass.simpleName};"
            discoveryReady = false
        }

        // 5. Native Bluetooth RFCOMM server (UUID c4a1b170-0001-4000-8000-00805f9b34fb).
        val server = httpServer
        if (server != null) {
            try {
                val bt = BluetoothRfcommTransport(applicationContext, server)
                btTransport = bt
                bt.start()
                activeBluetooth = bt
            } catch (t: Throwable) {
                startupError = (startupError ?: "") + " Bluetooth: ${t.javaClass.simpleName};"
            }
        }

        acquireScreenOffLocks()
        isRunning = true

        if (foregroundOk) {
            updateNotification()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Guarantee we are foregrounded even if onCreate's notification path was delayed.
        try {
            if (!isForeground()) {
                ensureChannel()
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    startForeground(
                        NOTIFICATION_ID,
                        buildForegroundNotification("Sibling link active"),
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                    )
                } else {
                    startForeground(NOTIFICATION_ID, buildForegroundNotification("Sibling link active"))
                }
            }
        } catch (_: Throwable) {
        }
        return START_STICKY
    }

    private fun isForeground(): Boolean {
        return try {
            val am = getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
            for (svc in am.getRunningServices(32)) {
                if (svc.service.className == SiblingDaemonService::class.java.name && svc.foreground) return true
            }
            false
        } catch (_: Throwable) {
            false
        }
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "ChainIt Sibling Link",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Keeps your phone storage mounted on Windows while the screen is off"
            setShowBadge(false)
            enableLights(false)
            enableVibration(false)
        }
        nm.createNotificationChannel(channel)
    }

    private fun updateNotification() {
        try {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.notify(NOTIFICATION_ID, buildForegroundNotification(statusLine()))
        } catch (_: Throwable) {
        }
    }

    private fun statusLine(): String {
        val parts = mutableListOf<String>()
        if (httpReady) parts.add("Z:\\ ready")
        if (isBluetoothListening()) parts.add("Bluetooth live")
        if (getDiscoveredPc() != null) parts.add("PC: " + (getDiscoveredPc()?.pcName ?: ""))
        if (parts.isEmpty()) parts.add("Waiting for Windows")
        return parts.joinToString(" • ")
    }

    private fun acquireScreenOffLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ChainIt::SiblingScreenOffLock").apply {
                setReferenceCounted(false)
                acquire(12L * 60 * 60 * 1000) // hard ceiling: never hold a lock forever
            }
        } catch (_: Throwable) {
        }

        try {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wm.createWifiLock(mode, "ChainIt::SiblingWifiLock").apply {
                setReferenceCounted(false)
                acquire()
            }
            multicastLock = wm.createMulticastLock("ChainIt::SiblingUdpLock").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (_: Throwable) {
        }
    }

    private fun buildForegroundNotification(text: String): Notification {
        val openIntent = Intent(this, MainActivity::class.java)
        val pi = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_stat_chainit)
            .setColor(0xFF10B981.toInt())
            .setContentIntent(pi)
            .setOngoing(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    override fun onDestroy() {
        isRunning = false
        activeServer = null
        activeDiscovery = null
        activeBluetooth = null
        runCatching { btTransport?.stop() }
        runCatching { httpServer?.stop() }
        runCatching { discovery?.stop() }
        try {
            if (wakeLock?.isHeld == true) wakeLock?.release()
            if (wifiLock?.isHeld == true) wifiLock?.release()
            if (multicastLock?.isHeld == true) multicastLock?.release()
        } catch (_: Throwable) {
        }
        try {
            super.onDestroy()
        } catch (_: Throwable) {
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
