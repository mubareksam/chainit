package com.chainit.android.net

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
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
 *
 * v1.8.0 - "Bluetooth must work smoothly and fast". Four things used to stop it working, and
 * each is fixed here rather than papered over:
 *
 *  1. The listener *publishes an SDP record*, which on Android 12+ needs BLUETOOTH_ADVERTISE
 *     as well as BLUETOOTH_CONNECT. Only CONNECT was ever checked, so a phone that granted
 *     one and denied the other threw a SecurityException from listen*, published nothing, and
 *     the PC's SDP query could never resolve it. The failure is now named exactly.
 *  2. The phone -> PC push had a single way to connect (secure RFCOMM by service record). When
 *     that fails - the classic "read failed, socket might closed or timeout, read ret: -1" that
 *     a desktop-published record produces on many handsets - there was nothing else to try. It
 *     now walks secure -> insecure -> explicit channel, and only accepts an HTTP 200.
 *  3. Nothing told the PC which RFCOMM channel this phone is listening on, so the PC had to
 *     re-run an SDP query (or guess) on every dial. The resolved channel is now exposed and
 *     published in the UDP beacon and the ping reply, so the PC can dial it directly.
 *  4. The device that answered last is remembered and tried first, which is the difference
 *     between a push that takes one connect and one that walks every paired device.
 */
class BluetoothRfcommTransport(
    private val context: Context,
    private val httpServer: PhoneHttpAndWebDavServer
) {
    companion object {
        val CHAINIT_PHONE_UUID: UUID = UUID.fromString("c4a1b170-0001-4000-8000-00805f9b34fb")
        val CHAINIT_PC_UUID: UUID = UUID.fromString("c4a1b170-0002-4000-8000-00805f9b34fb")

        /** Highest RFCOMM channel the connect fallbacks will try. Android services sit low. */
        private const val MAX_FALLBACK_CHANNEL = 12

        private const val PREFS = "chainit_bluetooth"
        private const val KEY_LAST_PC = "lastPcAddress"

        /** True when the phone may *dial* another device (BLUETOOTH_CONNECT). */
        fun hasBluetoothPermission(context: Context): Boolean {
            return if (Build.VERSION.SDK_INT >= 31) {
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
            } else {
                // Below Android 12 the install-time BLUETOOTH/BLUETOOTH_ADMIN pair covers it.
                context.checkSelfPermission(Manifest.permission.BLUETOOTH) == PackageManager.PERMISSION_GRANTED ||
                    Build.VERSION.SDK_INT < 23
            }
        }

        /**
         * True when the phone may *publish* its RFCOMM service (BLUETOOTH_ADVERTISE on 12+).
         * `listenUsingRfcommWithServiceRecord` registers an SDP record, so without this the
         * listener cannot be found by the PC even though it is running.
         */
        fun hasBluetoothAdvertisePermission(context: Context): Boolean {
            return if (Build.VERSION.SDK_INT >= 31) {
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE) == PackageManager.PERMISSION_GRANTED
            } else {
                hasBluetoothPermission(context)
            }
        }

        /**
         * Names the missing permission instead of reporting a generic failure, because
         * "Bluetooth did not connect" is not something a user can act on. Null when all is well.
         */
        fun bluetoothPermissionProblem(context: Context): String? {
            if (Build.VERSION.SDK_INT < 31) return null
            val missing = ArrayList<String>()
            if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                missing.add("Nearby devices (BLUETOOTH_CONNECT)")
            }
            if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE) != PackageManager.PERMISSION_GRANTED) {
                missing.add("advertise to nearby devices (BLUETOOTH_ADVERTISE)")
            }
            if (missing.isEmpty()) return null
            val joined = StringBuilder()
            for (i in missing.indices) {
                if (i > 0) joined.append(" and ")
                joined.append(missing[i])
            }
            return "Android withheld the Bluetooth permission to $joined. Grant it in Settings > Apps > ChainIt > Permissions, then reopen ChainIt."
        }

        @SuppressLint("MissingPermission")
        fun getLocalAdapter(context: Context): BluetoothAdapter? {
            val mgr = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
            return mgr?.adapter
        }

        /** This phone's own Bluetooth address, so the PC never has to guess which paired device it is. */
        @SuppressLint("MissingPermission")
        fun localAddress(context: Context): String? {
            if (!hasBluetoothPermission(context)) return null
            val adapter = getLocalAdapter(context) ?: return null
            return runCatching { adapter.address }.getOrNull()
        }

        /**
         * Connects to `device`'s ChainIt PC service, trying every way Android offers.
         *
         * The order matters: the secure service-record dial is what normally works, the insecure
         * variant covers desktop stacks that do not negotiate authentication, and the explicit
         * channel dial is the recovery for records that resolve to a channel the secure API then
         * refuses. Returns a *connected* socket or null.
         */
        /**
         * Every way Android offers to reach a device's ChainIt PC service, best first.
         *
         * Shared by the file push and by [PcVfsClient]'s native PC browser, because both used to
         * fail the same way: one socket-creation call, and when it produced nothing the request was
         * reported as "no RFCOMM channel could be opened" with nothing else tried. Each entry is a
         * label plus a factory, so a caller can decide whether to connect inline or on its own
         * deadline-bounded worker and still report *which* strategy failed.
         */
        @SuppressLint("MissingPermission")
        fun rfcommStrategies(device: BluetoothDevice): List<Pair<String, () -> BluetoothSocket?>> {
            val strategies = ArrayList<Pair<String, () -> BluetoothSocket?>>()
            strategies.add(Pair("secure service record") {
                runCatching { device.createRfcommSocketToServiceRecord(CHAINIT_PC_UUID) }.getOrNull()
            })
            strategies.add(Pair("insecure service record") {
                runCatching { device.createInsecureRfcommSocketToServiceRecord(CHAINIT_PC_UUID) }.getOrNull()
            })
            // Reflection path: createRfcommSocket(int channel). Hidden but stable across releases,
            // and the only way to reach a desktop listener whose SDP record the secure API rejects.
            for (channel in 1..MAX_FALLBACK_CHANNEL) {
                strategies.add(Pair("channel $channel") {
                    runCatching {
                        val m = device.javaClass.getMethod("createRfcommSocket", Int::class.javaPrimitiveType)
                        m.invoke(device, channel) as? BluetoothSocket
                    }.getOrNull()
                })
            }
            return strategies
        }

        @SuppressLint("MissingPermission")
        private fun connectToPc(device: BluetoothDevice, attempts: MutableList<String>): BluetoothSocket? {
            for (entry in rfcommStrategies(device)) {
                val sock = entry.second()
                if (sock == null) {
                    attempts.add("${entry.first}: socket could not be created")
                    continue
                }
                try {
                    sock.connect()
                    return sock
                } catch (e: Exception) {
                    attempts.add("${entry.first}: ${e.message ?: e.javaClass.simpleName}")
                    runCatching { sock.close() }
                }
            }
            return null
        }

        /**
         * Remembers which PC actually answered, so the next Bluetooth request dials it first.
         *
         * Shared by the file push and by [PcVfsClient]: both used to walk the whole paired-device
         * list every time, paying one failed connect per wrong device before reaching the PC. The
         * address is learned either from a successful Bluetooth reply or from the X-ChainIt-Pc-Bt
         * header the PC puts on every response, so a single Wi-Fi request is enough to teach it.
         */
        fun rememberPcAddress(context: Context, address: String) {
            val normalized = address.trim().uppercase()
            if (normalized.isEmpty()) return
            runCatching {
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                    .edit().putString(KEY_LAST_PC, normalized).apply()
            }
        }

        fun rememberedPcAddress(context: Context): String? = runCatching {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_LAST_PC, null)
        }.getOrNull()

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
            bluetoothPermissionProblem(context)?.let { return Pair(false, it) }
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

            // Try the device that answered last before walking the whole paired list.
            val last = rememberedPcAddress(context)
            val ordered = ArrayList<BluetoothDevice>()
            for (device in bonded) {
                if (last != null && runCatching { device.address }.getOrNull()?.uppercase() == last) ordered.add(0, device)
                else ordered.add(device)
            }

            val attempts = ArrayList<String>()
            for (device in ordered) {
                val devLabel = try { device.name ?: device.address } catch (_: Exception) { "Windows PC" }
                val sock = connectToPc(device, attempts)
                if (sock == null) continue
                try {
                    val encodedName = URLEncoder.encode(fileName, "UTF-8").replace("+", "%20")
                    val hdr = buildString {
                        append("POST /api/vfs/pc/upload?dir=%2FDesktop&filename=$encodedName HTTP/1.1\r\n")
                        append("Host: bluetooth-pc\r\n")
                        append("X-ChainIt-Transport: bluetooth-rfcomm\r\n")
                        append("Content-Type: application/octet-stream\r\n")
                        append("Content-Length: $byteLength\r\n")
                        append("Connection: close\r\n")
                        append("\r\n")
                    }
                    val out = sock.outputStream
                    out.write(hdr.toByteArray(Charsets.UTF_8))
                    if (bufferedBytes != null) {
                        out.write(bufferedBytes)
                    } else {
                        context.contentResolver.openInputStream(uri)?.use { input ->
                            val buf = ByteArray(64 * 1024)
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

                    // Wait for the PC's HTTP status line. Reading once and searching for the
                    // substring "200" is not a check: a body containing "200" anywhere, or a
                    // status line split across two reads, both produced a wrong verdict.
                    val respBuf = ByteArray(1024)
                    var got = 0
                    val deadline = System.currentTimeMillis() + 20_000L
                    while (got < respBuf.size && System.currentTimeMillis() < deadline) {
                        val n = sock.inputStream.read(respBuf, got, respBuf.size - got)
                        if (n <= 0) break
                        got += n
                        val head = String(respBuf, 0, got, Charsets.UTF_8)
                        if (head.contains("\r\n\r\n") || head.contains("\n\n")) break
                    }
                    val respText = String(respBuf, 0, got, Charsets.UTF_8)
                    val statusLine = respText.substringBefore("\r\n").substringBefore("\n")
                    if (statusLine.contains(" 200 ") || statusLine.contains(" 201 ")) {
                        rememberPcAddress(context, device.address)
                        return Pair(true, "Sent $fileName over Bluetooth to $devLabel (C:\\Desktop)")
                    }
                    attempts.add("$devLabel answered \"$statusLine\"")
                    if (statusLine.contains(" 401 ") || statusLine.contains(" 403 ")) {
                        return Pair(
                            false,
                            "$devLabel rejected the push ($statusLine). The sibling key on the PC does not match the phone - open the Link tab on both and check it."
                        )
                    }
                } catch (e: Exception) {
                    attempts.add("$devLabel: ${e.message ?: e.javaClass.simpleName}")
                } finally {
                    try {
                        sock.close()
                    } catch (_: Exception) {
                    }
                }
            }

            val why = if (attempts.isEmpty()) "no paired device was reachable" else attempts.last()
            return Pair(
                false,
                "No paired Windows PC running ChainIt-Windows.exe answered over Bluetooth. Last attempt - $why. " +
                    "On the PC, check the Link tab says the Bluetooth receiver is published."
            )
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
     * The RFCOMM channel this phone's listener actually sits on, or 0 when the platform will
     * not say. Published to the PC so it can dial the channel directly instead of re-running an
     * SDP query on every request - the single biggest latency in the Bluetooth path.
     */
    @Volatile
    var rfcommChannel: Int = 0
        private set

    /** Reads the channel out of a BluetoothServerSocket. Best effort: 0 when hidden away. */
    private fun resolveServerChannel(ss: BluetoothServerSocket): Int {
        runCatching {
            val m = ss.javaClass.getDeclaredMethod("getChannel")
            m.isAccessible = true
            val v = m.invoke(ss)
            if (v is Int && v > 0) return v
        }
        runCatching {
            val f = ss.javaClass.getDeclaredField("mChannel")
            f.isAccessible = true
            val v = f.get(ss)
            if (v is Int && v > 0) return v
        }
        return 0
    }

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
     *
     * Both listen variants register an SDP record, so both need BLUETOOTH_ADVERTISE on Android
     * 12+. When it is missing the reason is reported verbatim rather than as "could not open an
     * RFCOMM channel", because that message tells a user nothing they can fix.
     */
    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        running = true
        Thread {
            while (running) {
                try {
                    val permissionProblem = bluetoothPermissionProblem(context)
                    if (permissionProblem != null) {
                        isListeningRfcomm = false
                        listenMode = "blocked: permission"
                        lastError = permissionProblem
                        Thread.sleep(2500L)
                        continue
                    }
                    val adapter = getLocalAdapter(context)
                    if (adapter == null) {
                        isListeningRfcomm = false
                        listenMode = "blocked: no adapter"
                        lastError = "This device has no Bluetooth adapter."
                        Thread.sleep(2500L)
                        continue
                    }
                    if (!adapter.isEnabled) {
                        isListeningRfcomm = false
                        listenMode = "blocked: radio off"
                        lastError = "Bluetooth radio is off. Turn it on and ChainIt re-publishes itself automatically."
                        Thread.sleep(2500L)
                        continue
                    }

                    // Secure (authenticated + encrypted) first; fall back to the insecure channel,
                    // which is what a lot of desktop stacks actually negotiate against.
                    var ss: BluetoothServerSocket? = null
                    var mode = "secure"
                    var secureError: String? = null
                    try {
                        ss = adapter.listenUsingRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)
                    } catch (e: Exception) {
                        secureError = e.message ?: e.javaClass.simpleName
                        try {
                            ss = adapter.listenUsingInsecureRfcommWithServiceRecord("ChainItSibling", CHAINIT_PHONE_UUID)
                            mode = "insecure"
                        } catch (e2: Exception) {
                            val second = e2.message ?: e2.javaClass.simpleName
                            val hint = if (second.contains("Permission", ignoreCase = true) ||
                                secureError?.contains("Permission", ignoreCase = true) == true
                            ) {
                                " Android withheld the advertise permission, so no SDP record could be published."
                            } else if (second.contains("in use", ignoreCase = true) ||
                                second.contains("Address already", ignoreCase = true)
                            ) {
                                " Another ChainIt listener already holds the channel; the older instance is still running."
                            } else {
                                ""
                            }
                            lastError = "Could not open an RFCOMM channel: secure=$secureError, insecure=$second.$hint"
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
                    rfcommChannel = resolveServerChannel(ss)

                    while (running) {
                        val client = ss.accept()
                        acceptedSessions += 1
                        // Discovery competes with an RFCOMM link for the radio; stopping it for the
                        // duration of a session is what keeps a transfer from stalling.
                        runCatching { adapter.cancelDiscovery() }
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
                    rfcommChannel = 0
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
        rfcommChannel = 0
        try {
            serverSocket?.close()
        } catch (_: Exception) {
        }
        serverSocket = null
    }
}
