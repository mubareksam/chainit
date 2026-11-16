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

    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        running = true
        Thread {
            while (running) {
                try {
                    if (!hasBluetoothPermission(context)) {
                        isListeningRfcomm = false
                        Thread.sleep(2500L)
                        continue
                    }
                    val adapter = getLocalAdapter(context)
                    if (adapter == null || !adapter.isEnabled) {
                        isListeningRfcomm = false
                        Thread.sleep(2500L)
                        continue
                    }

                    val ss = adapter.listenUsingRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)
                    serverSocket = ss
                    isListeningRfcomm = true

                    while (running) {
                        val client = ss.accept()
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
                                        viaBluetooth = true
                                    )
                                } catch (_: Exception) {
                                }
                            }
                        }
                    }
                } catch (_: Exception) {
                    isListeningRfcomm = false
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
        }.apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        isListeningRfcomm = false
        try {
            serverSocket?.close()
        } catch (_: Exception) {
        }
        serverSocket = null
    }
}
