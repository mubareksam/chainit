package com.chainit.android.net

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import java.io.ByteArrayOutputStream
import java.net.URLEncoder
import java.util.UUID
import java.util.concurrent.Executors

/**
 * Pure Native Bluetooth RFCOMM Transport for ChainIt Sibling Mode.
 * Works 100% offline even when Wi-Fi, Hotspot, Mobile Data, and USB are ALL turned OFF!
 * - Server UUID (Phone): c4a1b170-0001-4000-8000-00805f9b34fb (serves Z:\ & File Manager to Windows PC)
 * - Server UUID (PC)   : c4a1b170-0002-4000-8000-00805f9b34fb (receives instant files pushed from Phone -> PC)
 */
class BluetoothRfcommTransport(
    private val context: Context,
    private val httpServer: PhoneHttpAndWebDavServer
) {
    companion object {
        val CHAINIT_PHONE_UUID: UUID = UUID.fromString("c4a1b170-0001-4000-8000-00805f9b34fb")
        val CHAINIT_PC_UUID: UUID = UUID.fromString("c4a1b170-0002-4000-8000-00805f9b34fb")

        fun hasBluetoothPermission(context: Context): Boolean {
            return if (Build.VERSION.SDK_INT >= 31) {
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
            } else {
                true
            }
        }

        @SuppressLint("MissingPermission")
        fun getLocalAdapter(context: Context): BluetoothAdapter? {
            val mgr = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
            return mgr?.adapter
        }

        /**
         * Sends a file from Android Phone -> Paired Windows PC purely over Bluetooth RFCOMM
         * (zero Wi-Fi, zero Hotspot, zero USB needed!).
         */
        @SuppressLint("MissingPermission")
        fun sendUriOverBluetoothToPairedPc(
            context: Context,
            uri: Uri,
            fileName: String
        ): Pair<Boolean, String> {
            if (!hasBluetoothPermission(context)) {
                return Pair(false, "Grant Bluetooth permission in ChainIt setup first.")
            }
            val adapter = getLocalAdapter(context)
                ?: return Pair(false, "Bluetooth hardware not available on this device.")
            if (!adapter.isEnabled) {
                return Pair(false, "Turn on Bluetooth first.")
            }

            val bonded = adapter.bondedDevices ?: emptySet()
            if (bonded.isEmpty()) {
                return Pair(false, "Pair your Android phone and Windows laptop once in Bluetooth Settings.")
            }

            // Determine exact byte length of the selected URI
            var byteLength = -1L
            context.contentResolver.query(uri, null, null, null, null)?.use { c ->
                val sizeIdx = c.getColumnIndex(OpenableColumns.SIZE)
                if (sizeIdx >= 0 && c.moveToFirst() && !c.isNull(sizeIdx)) {
                    byteLength = c.getLong(sizeIdx)
                }
            }
            val bufferedBytes: ByteArray? = if (byteLength < 0) {
                val baos = ByteArrayOutputStream()
                context.contentResolver.openInputStream(uri)?.use { it.copyTo(baos) }
                val arr = baos.toByteArray()
                byteLength = arr.size.toLong()
                arr
            } else {
                null
            }

            try {
                adapter.cancelDiscovery()
            } catch (_: Exception) {
            }

            var lastErr = "No paired Windows PC running ChainIt-Windows.exe responded over Bluetooth."
            for (device in bonded) {
                var sock: BluetoothSocket? = null
                try {
                    sock = device.createRfcommSocketToServiceRecord(CHAINIT_PC_UUID)
                    sock.connect()
                    val encodedName = URLEncoder.encode(fileName, "UTF-8").replace("+", "%20")
                    val hdr = buildString {
                        append("POST /api/vfs/pc/upload?dir=%2FDesktop&filename=$encodedName HTTP/1.1\r\n")
                        append("Host: bluetooth-pc\r\n")
                        append("Content-Type: application/octet-stream\r\n")
                        append("Content-Length: $byteLength\r\n")
                        append("Connection: close\r\n\r\n")
                    }
                    val out = sock.outputStream
                    out.write(hdr.toByteArray(Charsets.UTF_8))
                    if (bufferedBytes != null) {
                        out.write(bufferedBytes)
                    } else {
                        context.contentResolver.openInputStream(uri)?.use { input ->
                            val buf = ByteArray(16 * 1024)
                            var rem = byteLength
                            while (rem > 0) {
                                val r = input.read(buf, 0, minOf(buf.size.toLong(), rem).toInt())
                                if (r <= 0) break
                                out.write(buf, 0, r)
                                rem -= r
                            }
                        }
                    }
                    out.flush()

                    // Wait for HTTP 200 response from PC before closing RFCOMM channel
                    val respBuf = ByteArray(512)
                    val n = sock.inputStream.read(respBuf)
                    val respText = if (n > 0) String(respBuf, 0, n, Charsets.UTF_8) else ""
                    val devLabel = try { device.name ?: device.address } catch (_: Exception) { "Windows PC" }
                    if (respText.contains("200")) {
                        return Pair(true, "✓ Sent $fileName over Bluetooth to $devLabel (C:\\Desktop)!")
                    }
                } catch (e: Exception) {
                    lastErr = e.message ?: lastErr
                } finally {
                    try {
                        sock?.close()
                    } catch (_: Exception) {
                    }
                }
            }
            return Pair(false, lastErr)
        }
    }

    private val pool = Executors.newCachedThreadPool()

    @Volatile
    private var running = false

    @Volatile
    var isListeningRfcomm: Boolean = false
        private set

    @Volatile
    var lastBtPeerName: String? = null
        private set

    private var serverSocket: BluetoothServerSocket? = null

    /**
     * How the listen socket was obtained, so the Windows side and the UI can explain a failure
     * instead of showing a link that is "connected" but serves nothing.
     */
    @Volatile
    var listenMode: String = "idle"
        private set

    @Volatile
    var lastError: String? = null
        private set

    @Volatile
    var acceptedSessions: Long = 0L
        private set

    /**
     * Publishes the ChainIt VFS over RFCOMM and keeps it published.
     *
     * Two things make Bluetooth behave like a LAN link rather than a one-shot file push:
     *  1. the accepted socket is handed to [PhoneHttpAndWebDavServer.handleStreamSession] as a
     *     *keep-alive* session, so Windows dials the radio once and then serves a whole Explorer
     *     session (PROPFIND, GET ranges, PUT, MKCOL, MOVE, DELETE) over that single link;
     *  2. the listener is re-armed forever - secure channel first, then the insecure channel that
     *     many stacks require - and a watchdog restarts it if `accept()` ever dies, if Bluetooth is
     *     toggled off/on, or if the phone has been locked for hours.
     */
    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        running = true
        Thread {
            while (running) {
                try {
                    if (!hasBluetoothPermission(context)) {
                        isListeningRfcomm = false
                        listenMode = "blocked: permission"
                        lastError = "Bluetooth permission not granted"
                        Thread.sleep(2500L)
                        continue
                    }
                    val adapter = getLocalAdapter(context)
                    if (adapter == null || !adapter.isEnabled) {
                        isListeningRfcomm = false
                        listenMode = "blocked: radio off"
                        lastError = "Bluetooth radio is off"
                        Thread.sleep(2500L)
                        continue
                    }

                    // Secure (authenticated + encrypted) first; fall back to the insecure channel,
                    // which is what a lot of desktop stacks actually negotiate against.
                    var ss: BluetoothServerSocket? = null
                    var mode = "secure"
                    try {
                        ss = adapter.listenUsingRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)
                    } catch (e: Exception) {
                        try {
                            ss = adapter.listenUsingInsecureRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)
                            mode = "insecure"
                        } catch (e2: Exception) {
                            lastError = "Could not open an RFCOMM channel: " + (e2.message ?: e.message ?: "unknown")
                        }
                    }
                    if (ss == null) {
                        isListeningRfcomm = false
                        listenMode = "failed"
                        Thread.sleep(2500L)
                        continue
                    }

                    serverSocket = ss
                    isListeningRfcomm = true
                    listenMode = mode
                    lastError = null

                    while (running) {
                        val client = ss.accept()
                        acceptedSessions += 1
                        try {
                            lastBtPeerName = client.remoteDevice?.name ?: client.remoteDevice?.address
                        } catch (_: Exception) {
                        }
                        pool.execute {
                            client.use { btSock ->
                                try {
                                    httpServer.handleStreamSession(
                                        btSock.inputStream,
                                        btSock.outputStream,
                                        viaBluetooth = true,
                                        closeable = btSock
                                    )
                                } catch (_: Exception) {
                                }
                            }
                        }
                    }
                } catch (_: InterruptedException) {
                    break
                } catch (e: Exception) {
                    // accept() throws when the radio is toggled, when a peer drops mid-dial, or when
                    // Doze finally suspends us. Never give up: re-arm and keep listening.
                    isListeningRfcomm = false
                    listenMode = "re-arming"
                    lastError = e.message ?: e.javaClass.simpleName
                    try {
                        serverSocket?.close()
                    } catch (_: Exception) {
                    }
                    serverSocket = null
                    try {
                        Thread.sleep(2500L)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }
        }.apply { isDaemon = true; name = "ChainItRfcommListener"; start() }

        // Watchdog: if the listener is supposed to be up but is not, force a re-arm.
        Thread {
            while (running) {
                try {
                    Thread.sleep(20_000L)
                    if (running && !isListeningRfcomm) {
                        try {
                            serverSocket?.close()
                        } catch (_: Exception) {
                        }
                        serverSocket = null
                    }
                } catch (_: InterruptedException) {
                    return@Thread
                } catch (_: Throwable) {
                }
            }
        }.apply { isDaemon = true; name = "ChainItRfcommWatchdog"; start() }
    }

    fun stop() {
        running = false
        isListeningRfcomm = false
        listenMode = "stopped"
        try {
            serverSocket?.close()
        } catch (_: Exception) {
        }
        serverSocket = null
    }
}
