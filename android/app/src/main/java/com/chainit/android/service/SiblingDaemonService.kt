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
import com.chainit.android.net.BluetoothRfcommTransport
import com.chainit.android.net.DiscoveredPcInfo
import com.chainit.android.net.LocalLinkEndpoint
import com.chainit.android.net.PhoneHttpAndWebDavServer
import com.chainit.android.net.UdpSiblingDiscovery
import com.chainit.android.ui.MainActivity
import com.chainit.android.vfs.AndroidStorageVfsProvider
import java.io.File

class SiblingDaemonService : Service() {

    companion object {
        const val CHANNEL_ID = "chainit_sibling_channel"
        const val NOTIFICATION_ID = 4918

        @Volatile
        var isRunning: Boolean = false
            private set

        @Volatile
        private var activeServer: PhoneHttpAndWebDavServer? = null

        @Volatile
        private var activeDiscovery: UdpSiblingDiscovery? = null

        @Volatile
        private var activeBluetooth: BluetoothRfcommTransport? = null

        fun getLastVfsSummary(): String =
            activeServer?.lastActivitySummary ?: "Service stopped"

        fun getEndpoints(context: Context): List<LocalLinkEndpoint> =
            activeDiscovery?.enumerateEndpoints() ?: UdpSiblingDiscovery(context).enumerateEndpoints()

        fun getDiscoveredPc(): DiscoveredPcInfo? =
            activeDiscovery?.lastDiscoveredPc

        fun isBluetoothListening(): Boolean =
            activeBluetooth?.isListeningRfcomm == true

        fun getLastBluetoothPeer(): String? =
            activeBluetooth?.lastBtPeerName
    }

    private lateinit var vfs: AndroidStorageVfsProvider
    private lateinit var httpServer: PhoneHttpAndWebDavServer
    private lateinit var discovery: UdpSiblingDiscovery
    private lateinit var btTransport: BluetoothRfcommTransport

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    override fun onCreate() {
        super.onCreate()
        vfs = AndroidStorageVfsProvider()
        try {
            File(vfs.storageRoot, "ChainIt_Inbox").mkdirs()
        } catch (_: Exception) {
        }

        httpServer = PhoneHttpAndWebDavServer(applicationContext, vfs, 49180)
        discovery = UdpSiblingDiscovery(applicationContext, 49180, 49179)
        btTransport = BluetoothRfcommTransport(applicationContext, httpServer)

        val notification = buildForegroundNotification("Sibling Active • Wi-Fi, Hotspot, USB & Bluetooth Ready")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        acquireScreenOffLocks()
        httpServer.start()
        discovery.start()
        btTransport.start()

        activeServer = httpServer
        activeDiscovery = discovery
        activeBluetooth = btTransport
        isRunning = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        return START_STICKY
    }

    private fun acquireScreenOffLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ChainIt::SiblingScreenOffLock").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (_: Exception) {
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
        } catch (_: Exception) {
        }
    }

    private fun buildForegroundNotification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            "ChainIt Sibling Background Link",
            NotificationManager.IMPORTANCE_LOW
        )
        nm.createNotificationChannel(channel)

        val openIntent = Intent(this, MainActivity::class.java)
        val pi = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("ChainIt Sibling Mode Active")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        isRunning = false
        activeServer = null
        activeDiscovery = null
        activeBluetooth = null
        btTransport.stop()
        httpServer.stop()
        discovery.stop()
        try {
            if (wakeLock?.isHeld == true) wakeLock?.release()
            if (wifiLock?.isHeld == true) wifiLock?.release()
            if (multicastLock?.isHeld == true) multicastLock?.release()
        } catch (_: Exception) {
        }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
