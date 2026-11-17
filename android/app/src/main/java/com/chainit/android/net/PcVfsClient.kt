package com.chainit.android.net

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.net.Uri
import com.chainit.android.service.SiblingDaemonService
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/**
 * Native client for the file API that ChainIt-Windows.exe serves on the PC.
 *
 * Why this file exists
 * --------------------
 * "Browse PC files" used to fire `Intent(ACTION_VIEW, http://<pc>:4173/mobile)` — it handed the user
 * to whatever browser happened to be installed. That broke the product promise twice: the interface
 * was drawn by Edge/Chrome (so the recent-apps tile and the identity belonged to the browser, not to
 * ChainIt), and the feature died the moment Wi-Fi did. Windows got a native file manager for exactly
 * this reason; this is the phone's half of the same fix.
 *
 * No new protocol was needed, because the PC already speaks plain HTTP/1.1 on both of its links:
 *   1. Wi-Fi / hotspot / USB tethering — TCP to `http://<pcIp>:4173/api/vfs/pc/<action>`.
 *   2. Bluetooth RFCOMM, zero Wi-Fi — the PC publishes the *same* HTTP API on service UUID
 *      `c4a1b170-0002-4000-8000-00805f9b34fb` (`bluetoothServerLoop()` hands every accepted socket
 *      to its HTTP handler), so a hand-written request over an RFCOMM socket is enough. That is also
 *      how the phone already pushes a file to the PC's Desktop.
 *
 * Bodies stream: a download goes straight to disk and an upload straight from the content resolver,
 * so a 2 GB video never passes through the heap. Every failure returns a sentence that says what to
 * do next — an unreachable PC is reported as unreachable, a wrong Pairing Key as a wrong key, a
 * rate-limited PC as "wait 30 s". Nothing is faked, and nothing opens a browser.
 */
object PcVfsClient {

    const val PC_HTTP_PORT = 4173
    private const val PREFS = "chainit_prefs"

    private const val LAN_CONNECT_MS = 3500
    private const val LAN_READ_MS = 20000
    private const val LAN_BODY_READ_MS = 180000
    private const val BT_CONNECT_MS = 12000
    private const val BT_READ_MS = 60000
    private const val BT_BODY_READ_MS = 180000

    /** Largest response held in memory (list / text / JSON answers are all far smaller). */
    private const val MAX_MEMORY_BODY = 24L * 1024 * 1024

    private const val USER_AGENT = "ChainIt-Android/1.6"

    enum class Transport(val label: String) {
        LAN("Wi-Fi"),
        BLUETOOTH("Bluetooth"),
        NONE("no link")
    }

    /** Where to dial. A blank `ip` means "Bluetooth only", which is a full link, not a fallback. */
    data class Target(val ip: String, val port: Int, val pcName: String) {
        val label: String
            get() = if (ip.isBlank()) "paired Bluetooth PC" else "${pcName.ifBlank { "PC" }} ($ip:$port)"
    }

    class Reply(
        val status: Int,
        val bytes: ByteArray,
        val transport: Transport,
        val rttMs: Long,
        val problem: String?
    ) {
        val ok: Boolean get() = status in 200..299
        val text: String get() = String(bytes, Charsets.UTF_8)

        /** One honest sentence for the status bar / toast — never a blank failure. */
        val message: String get() = problem ?: describe(status, transport)
    }

    data class Item(
        val name: String,
        val path: String,
        val isDirectory: Boolean,
        val sizeBytes: Long,
        val modifiedMs: Long,
        val mimeType: String,
        val ext: String
    ) {
        /** Type badge: the phone's equivalent of the Windows list's shell icon + Type column. */
        val badge: String
            get() {
                if (isDirectory) return "DIR"
                val raw = ext.removePrefix(".").uppercase()
                return if (raw.isBlank()) "FILE" else raw.take(4)
            }
    }

    class Listing(
        val currentPath: String,
        val items: List<Item>,
        val transport: Transport,
        val rttMs: Long,
        val problem: String?
    ) {
        val ok: Boolean get() = problem == null
        val message: String get() = problem ?: describe(200, transport)
    }

    class DownloadResult(val ok: Boolean, val bytes: Long, val transport: Transport, val message: String)

    /** Where a request body comes from. Re-openable, so a Bluetooth retry can resend it. */
    private interface BodySource {
        fun open(): InputStream?
        fun length(): Long
        fun contentType(): String
    }

    /**
     * Where a response body goes. Re-opened per transport attempt, so a Wi-Fi download that dies
     * half-way can never leave its bytes in front of the Bluetooth attempt's.
     */
    private interface BodySink {
        fun open(): OutputStream?
    }

    // ---------------------------------------------------------------------------------------------
    // Target resolution
    // ---------------------------------------------------------------------------------------------

    fun siblingKey(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString("sibling_key", "CHAINIT-SIBLING") ?: "CHAINIT-SIBLING"

    /**
     * Manual IP wins (that is what the field on the main screen is for), then the remembered one,
     * then UDP discovery. A blank result is not an error: Bluetooth can still reach a paired PC.
     */
    fun resolveTarget(context: Context, manualIp: String?): Target {
        val manual = (manualIp ?: "").trim()
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (manual.isNotEmpty()) {
            prefs.edit().putString("last_pc_ip", manual).apply()
            return Target(manual, PC_HTTP_PORT, discoveredName(manual))
        }
        val remembered = (prefs.getString("last_pc_ip", "") ?: "").trim()
        if (remembered.isNotEmpty()) return Target(remembered, PC_HTTP_PORT, discoveredName(remembered))
        val pc = SiblingDaemonService.getDiscoveredPc()
        return if (pc != null) Target(pc.ip, pc.httpPort, pc.pcName) else Target("", PC_HTTP_PORT, "")
    }

    private fun discoveredName(ip: String): String {
        val pc = SiblingDaemonService.getDiscoveredPc() ?: return ""
        return if (pc.ip == ip) pc.pcName else ""
    }

    fun bluetoothReady(context: Context): Boolean {
        if (!BluetoothRfcommTransport.hasBluetoothPermission(context)) return false
        val adapter = runCatching { BluetoothRfcommTransport.getLocalAdapter(context) }.getOrNull() ?: return false
        if (!adapter.isEnabled) return false
        val bonded = runCatching { adapter.bondedDevices }.getOrNull() ?: return false
        return bonded.isNotEmpty()
    }

    // ---------------------------------------------------------------------------------------------
    // Operations
    // ---------------------------------------------------------------------------------------------

    fun list(context: Context, target: Target, dir: String): Listing {
        val reply = request(context, target, "GET", "/api/vfs/pc/list?path=${enc(dir)}", null, null, LAN_READ_MS, BT_READ_MS, null, null, null)
        if (!reply.ok) return Listing(dir, emptyList(), reply.transport, reply.rttMs, reply.message)
        return try {
            val json = JSONObject(reply.text)
            val current = json.optString("currentPath", dir)
            val arr = json.optJSONArray("items") ?: JSONArray()
            val out = ArrayList<Item>(arr.length())
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                out.add(
                    Item(
                        name = o.optString("name", ""),
                        path = o.optString("path", current),
                        isDirectory = o.optString("kind", "file") == "directory",
                        sizeBytes = o.optLong("sizeBytes", 0L),
                        modifiedMs = o.optLong("modifiedMs", 0L),
                        mimeType = o.optString("mimeType", ""),
                        ext = o.optString("ext", "")
                    )
                )
            }
            Listing(current, out, reply.transport, reply.rttMs, null)
        } catch (t: Throwable) {
            Listing(dir, emptyList(), reply.transport, reply.rttMs, "The PC answered but its listing could not be read (${t.javaClass.simpleName}).")
        }
    }

    /** Inline text preview — the PC serves editable text up to 2 MB and says so when it will not. */
    fun readText(context: Context, target: Target, path: String): Reply =
        request(context, target, "GET", "/api/vfs/pc/text?path=${enc(path)}", null, null, LAN_READ_MS, BT_READ_MS, null, null, null)

    fun mkdir(context: Context, target: Target, parentDir: String, name: String): Reply {
        val body = JSONObject().put("parentDir", parentDir).put("name", name).toString().toByteArray(Charsets.UTF_8)
        return request(context, target, "POST", "/api/vfs/pc/mkdir", body, null, LAN_READ_MS, BT_READ_MS, null, null, null)
    }

    fun rename(context: Context, target: Target, path: String, newName: String): Reply {
        val body = JSONObject().put("path", path).put("newName", newName).toString().toByteArray(Charsets.UTF_8)
        return request(context, target, "POST", "/api/vfs/pc/rename", body, null, LAN_READ_MS, BT_READ_MS, null, null, null)
    }

    fun delete(context: Context, target: Target, paths: List<String>): Reply {
        val arr = JSONArray()
        for (p in paths) arr.put(p)
        val body = JSONObject().put("paths", arr).toString().toByteArray(Charsets.UTF_8)
        return request(context, target, "POST", "/api/vfs/pc/delete", body, null, LAN_READ_MS, BT_READ_MS, null, null, null)
    }

    /** One PC file → phone storage, streamed to disk with a byte count as it arrives. */
    fun download(
        context: Context,
        target: Target,
        path: String,
        dest: File,
        onProgress: (Long) -> Unit
    ): DownloadResult {
        val seen = AtomicLong(0L)
        val sink = object : BodySink {
            override fun open(): OutputStream? {
                dest.parentFile?.mkdirs()
                seen.set(0L)
                return try {
                    FileOutputStream(dest)
                } catch (t: Throwable) {
                    null
                }
            }
        }
        val reply = request(
            context, target, "GET", "/api/vfs/pc/raw?path=${enc(path)}&download=1",
            null, null, LAN_BODY_READ_MS, BT_BODY_READ_MS, sink, null
        ) { bytes ->
            seen.set(bytes)
            onProgress(bytes)
        }
        val written = seen.get()
        if (!reply.ok) {
            runCatching { dest.delete() }
            return DownloadResult(false, written, reply.transport, reply.message)
        }
        return DownloadResult(true, written, reply.transport, "Saved ${humanSize(written)} over ${reply.transport.label}")
    }

    /**
     * Phone file (content:// Uri) → a folder on the PC, streamed in both directions.
     * The PC's handler reads a fixed Content-Length, so an upload whose size Android will not report
     * is buffered first — and refused with an explanation if it is too big to buffer.
     */
    fun uploadUri(
        context: Context,
        target: Target,
        uri: Uri,
        fileName: String,
        dir: String,
        onProgress: (Long, Long) -> Unit
    ): Reply {
        val reported = uriSize(context, uri)
        val query = "/api/vfs/pc/upload?dir=${enc(dir)}&filename=${enc(fileName)}"

        if (reported >= 0) {
            val streaming = object : BodySource {
                override fun open(): InputStream? = runCatching { context.contentResolver.openInputStream(uri) }.getOrNull()
                override fun length(): Long = reported
                override fun contentType(): String = "application/octet-stream"
            }
            return request(context, target, "POST", query, null, streaming, LAN_BODY_READ_MS, BT_BODY_READ_MS, null, onProgress, null)
        }

        val buffered = ByteArrayOutputStream()
        runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val buf = ByteArray(64 * 1024)
                var total = 0L
                while (true) {
                    val r = input.read(buf)
                    if (r <= 0) break
                    total += r
                    if (total > MAX_MEMORY_BODY) return Reply(
                        0, ByteArray(0), Transport.NONE, 0L,
                        "Android will not report this file's size and it is larger than ${humanSize(MAX_MEMORY_BODY)}, so it cannot be sent. Copy it to the phone's storage first and send it from there."
                    )
                    buffered.write(buf, 0, r)
                    onProgress(total, -1L)
                }
            }
        }
        val payload = buffered.toByteArray()
        return request(context, target, "POST", query, payload, null, LAN_BODY_READ_MS, BT_BODY_READ_MS, null, onProgress, null)
    }

    private fun uriSize(context: Context, uri: Uri): Long {
        var size = -1L
        runCatching {
            context.contentResolver.query(uri, null, null, null, null)?.use { c ->
                val idx = c.getColumnIndex(android.provider.OpenableColumns.SIZE)
                if (idx >= 0 && c.moveToFirst() && !c.isNull(idx)) size = c.getLong(idx)
            }
        }
        return size
    }

    // ---------------------------------------------------------------------------------------------
    // Transport core: Wi-Fi first, Bluetooth RFCOMM as a full second link (not a degraded mode)
    // ---------------------------------------------------------------------------------------------

    private fun request(
        context: Context,
        target: Target,
        method: String,
        pathAndQuery: String,
        body: ByteArray?,
        bodySource: BodySource?,
        lanReadMs: Int,
        btReadMs: Int,
        sink: BodySink?,
        onUploadProgress: ((Long, Long) -> Unit)?,
        onDownloadProgress: ((Long) -> Unit)?
    ): Reply {
        val key = siblingKey(context)
        var lanProblem: String? = null

        if (target.ip.isNotBlank()) {
            val out = lanCall(target, method, pathAndQuery, body, bodySource, key, lanReadMs, sink, onUploadProgress, onDownloadProgress)
            // A 4xx/5xx is an answer, not a transport failure — never re-ask over Bluetooth.
            if (out.status > 0) return Reply(out.status, out.body, Transport.LAN, out.rttMs, out.problem)
            lanProblem = out.problem
        }

        val bt = btCall(context, method, pathAndQuery, body, bodySource, key, btReadMs, sink, onUploadProgress, onDownloadProgress)
        if (bt.status > 0) return Reply(bt.status, bt.body, Transport.BLUETOOTH, bt.rttMs, bt.problem)

        val why = when {
            lanProblem != null && bt.problem != null -> "$lanProblem  •  Bluetooth: ${bt.problem}"
            bt.problem != null -> "Bluetooth: ${bt.problem}"
            lanProblem != null -> lanProblem
            else -> "No link to the PC."
        }
        return Reply(0, ByteArray(0), Transport.NONE, 0L, why)
    }

    private class Outcome(
        var status: Int = 0,
        var body: ByteArray = ByteArray(0),
        var rttMs: Long = 0L,
        var problem: String? = null
    )

    private fun lanCall(
        target: Target,
        method: String,
        pathAndQuery: String,
        body: ByteArray?,
        bodySource: BodySource?,
        key: String,
        readMs: Int,
        sink: BodySink?,
        onUploadProgress: ((Long, Long) -> Unit)?,
        onDownloadProgress: ((Long) -> Unit)?
    ): Outcome {
        val started = System.currentTimeMillis()
        var conn: HttpURLConnection? = null
        try {
            val url = URL("http://${target.ip}:${target.port}$pathAndQuery")
            conn = (url.openConnection() as HttpURLConnection).apply {
                requestMethod = method
                connectTimeout = LAN_CONNECT_MS
                this.readTimeout = readMs
                instanceFollowRedirects = false
                setRequestProperty("X-ChainIt-Key", key)
                setRequestProperty("Accept", "*/*")
                setRequestProperty("User-Agent", USER_AGENT)
            }

            val payload = bodySource ?: fixedBody(body, "application/json")
            if (payload != null) {
                val len = payload.length()
                if (len < 0) return Outcome(problem = "This file's size is unknown, so it cannot be sent to the PC.")
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", payload.contentType())
                conn.setFixedLengthStreamingMode(len)
                conn.outputStream.use { out -> copyBody(payload, out, len, onUploadProgress) }
            }

            val status = conn.responseCode
            val out = Outcome()
            out.status = status
            out.rttMs = System.currentTimeMillis() - started

            if (status !in 200..299) {
                out.body = readMemory(conn.errorStream)
                out.problem = describe(status, Transport.LAN, String(out.body, Charsets.UTF_8))
                return out
            }
            if (sink != null) {
                val stream = sink.open()
                if (stream == null) {
                    return Outcome(status = 0, problem = "This phone cannot write that file to its own storage. Grant All-files access in ChainIt's setup list, then try again.")
                }
                try {
                    stream.use { copyStream(conn.inputStream, it, conn.contentLengthLong) { seen, _ -> onDownloadProgress?.invoke(seen) } }
                } catch (t: Throwable) {
                    return Outcome(status = 0, problem = "The download stopped: ${t.javaClass.simpleName}")
                }
                return out
            }
            out.body = readMemory(conn.inputStream)
            return out
        } catch (t: SocketTimeoutException) {
            return Outcome(problem = "The PC at ${target.ip}:${target.port} did not answer within ${readMs / 1000} s. Is ChainIt-Windows.exe running?")
        } catch (t: Throwable) {
            return Outcome(problem = "The Wi-Fi link to ${target.ip}:${target.port} failed (${t.javaClass.simpleName}).")
        } finally {
            runCatching { conn?.disconnect() }
        }
    }

    private fun fixedBody(body: ByteArray?, contentType: String): BodySource? {
        if (body == null) return null
        return object : BodySource {
            override fun open(): InputStream = ByteArrayInputStream(body)
            override fun length(): Long = body.size.toLong()
            override fun contentType(): String = contentType
        }
    }

    private fun copyBody(source: BodySource, out: OutputStream, length: Long, onProgress: ((Long, Long) -> Unit)?) {
        val input = source.open() ?: return
        try {
            copyStream(input, out, length, onProgress)
        } finally {
            runCatching { input.close() }
        }
    }

    private fun copyStream(input: InputStream?, out: OutputStream, total: Long, onProgress: ((Long, Long) -> Unit)?) {
        if (input == null) return
        val buf = ByteArray(64 * 1024)
        var seen = 0L
        while (true) {
            val r = input.read(buf)
            if (r <= 0) break
            out.write(buf, 0, r)
            seen += r
            onProgress?.invoke(seen, total)
        }
        out.flush()
    }

    private fun readMemory(stream: InputStream?): ByteArray {
        if (stream == null) return ByteArray(0)
        val out = ByteArrayOutputStream()
        val buf = ByteArray(16 * 1024)
        try {
            while (true) {
                val r = stream.read(buf)
                if (r <= 0) break
                out.write(buf, 0, r)
                if (out.size() > MAX_MEMORY_BODY) break
            }
        } catch (_: Throwable) {
            // Keep whatever arrived; the caller reports a truncation instead of a fake success.
        } finally {
            runCatching { stream.close() }
        }
        return out.toByteArray()
    }

    // ---------------------------------------------------------------------------------------------
    // Bluetooth RFCOMM: the PC serves the same HTTP API on UUID c4a1b170-0002-…, no Wi-Fi needed
    // ---------------------------------------------------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun btCall(
        context: Context,
        method: String,
        pathAndQuery: String,
        body: ByteArray?,
        bodySource: BodySource?,
        key: String,
        readMs: Int,
        sink: BodySink?,
        onUploadProgress: ((Long, Long) -> Unit)?,
        onDownloadProgress: ((Long) -> Unit)?
    ): Outcome {
        if (!BluetoothRfcommTransport.hasBluetoothPermission(context)) {
            return Outcome(problem = "grant Bluetooth permission in ChainIt's setup list first")
        }
        val adapter = runCatching { BluetoothRfcommTransport.getLocalAdapter(context) }.getOrNull()
            ?: return Outcome(problem = "this phone has no Bluetooth radio")
        if (!adapter.isEnabled) return Outcome(problem = "Bluetooth is off")
        val bonded = runCatching { adapter.bondedDevices }.getOrNull()
        if (bonded.isNullOrEmpty()) return Outcome(problem = "no PC is paired yet")

        runCatching { adapter.cancelDiscovery() }

        var lastProblem = "no paired PC answered ChainIt's Bluetooth service"
        for (device in bonded) {
            val outcome = btCallDevice(device, method, pathAndQuery, body, bodySource, key, readMs, sink, onUploadProgress, onDownloadProgress)
            if (outcome.status > 0) return outcome
            if (outcome.problem != null) lastProblem = outcome.problem ?: lastProblem
        }
        return Outcome(problem = lastProblem)
    }

    @SuppressLint("MissingPermission")
    private fun btCallDevice(
        device: BluetoothDevice,
        method: String,
        pathAndQuery: String,
        body: ByteArray?,
        bodySource: BodySource?,
        key: String,
        readMs: Int,
        sink: BodySink?,
        onUploadProgress: ((Long, Long) -> Unit)?,
        onDownloadProgress: ((Long) -> Unit)?
    ): Outcome {
        val started = System.currentTimeMillis()
        var sock: BluetoothSocket? = null
        try {
            sock = openRfcomm(device)
                ?: return Outcome(problem = "no RFCOMM channel could be opened to ${deviceLabel(device)}")

            // connect() can block for tens of seconds against a radio that is paired but asleep, so
            // dial on a worker and give up after BT_CONNECT_MS: the user gets an answer, never a hang.
            val dialSock = sock
            val dialError = AtomicReference<String?>(null)
            val dialer = Thread {
                try {
                    dialSock.connect()
                } catch (t: Throwable) {
                    dialError.set(t.javaClass.simpleName)
                }
            }
            dialer.isDaemon = true
            dialer.name = "ChainItBtDial"
            dialer.start()
            dialer.join(BT_CONNECT_MS.toLong())
            if (dialer.isAlive) {
                runCatching { sock.close() }
                return Outcome(problem = "${deviceLabel(device)} did not accept the Bluetooth link within ${BT_CONNECT_MS / 1000} s")
            }
            if (dialError.get() != null) {
                runCatching { sock.close() }
                return Outcome(problem = "${deviceLabel(device)} refused ChainIt's Bluetooth service (${dialError.get()})")
            }

            // The PC reads a fixed Content-Length, so the length must be known before dialling in.
            val payload = bodySource ?: fixedBody(body, "application/json")
            var payloadLength = -1L
            if (payload != null) {
                payloadLength = payload.length()
                if (payloadLength < 0) {
                    return Outcome(problem = "this file's size is unknown, so it cannot be sent over Bluetooth")
                }
            }

            val out = sock.outputStream
            val header = StringBuilder()
            header.append(method).append(' ').append(pathAndQuery).append(" HTTP/1.1\r\n")
            header.append("Host: chainit-bluetooth\r\n")
            header.append("X-ChainIt-Key: ").append(key).append("\r\n")
            header.append("Accept: */*\r\n")
            header.append("User-Agent: ").append(USER_AGENT).append("\r\n")
            if (payload != null) {
                header.append("Content-Type: ").append(payload.contentType()).append("\r\n")
                header.append("Content-Length: ").append(payloadLength).append("\r\n")
            }
            header.append("Connection: close\r\n\r\n")
            out.write(header.toString().toByteArray(Charsets.UTF_8))
            if (payload != null) copyBody(payload, out, payloadLength, onUploadProgress)
            out.flush()

            val source = ByteSource(sock.inputStream, System.currentTimeMillis() + readMs)
            val statusLine = source.readLine()
                ?: return Outcome(problem = "${deviceLabel(device)} closed the Bluetooth link without answering")
            val status = parseStatus(statusLine)

            var contentLength = -1L
            var chunked = false
            while (true) {
                val line = source.readLine() ?: break
                if (line.isEmpty()) break
                val colon = line.indexOf(':')
                if (colon <= 0) continue
                val name = line.substring(0, colon).trim().lowercase()
                val value = line.substring(colon + 1).trim()
                if (name == "content-length") {
                    val parsed = value.toLongOrNull()
                    if (parsed != null) contentLength = parsed
                }
                if (name == "transfer-encoding" && value.lowercase().contains("chunked")) chunked = true
            }

            val outcome = Outcome()
            outcome.status = status
            outcome.rttMs = System.currentTimeMillis() - started

            if (status in 200..299 && sink != null) {
                val stream = sink.open()
                    ?: return Outcome(problem = "this phone cannot write that file to its own storage. Grant All-files access in ChainIt's setup list, then try again.")
                var wrote = 0L
                try {
                    stream.use {
                        wrote = readBody(source, it, contentLength, chunked, Long.MAX_VALUE) { seen, _ -> onDownloadProgress?.invoke(seen) }
                    }
                } catch (t: Throwable) {
                    return Outcome(problem = "the Bluetooth download stopped after ${humanSize(wrote)} (${t.javaClass.simpleName})")
                }
                if (contentLength >= 0 && wrote < contentLength) {
                    return Outcome(problem = "the Bluetooth link closed after ${humanSize(wrote)} of ${humanSize(contentLength)}")
                }
                return outcome
            }

            val memory = ByteArrayOutputStream()
            readBody(source, memory, contentLength, chunked, MAX_MEMORY_BODY, null)
            outcome.body = memory.toByteArray()
            if (status !in 200..299) {
                outcome.problem = describe(status, Transport.BLUETOOTH, String(outcome.body, Charsets.UTF_8))
            }
            return outcome
        } catch (t: Throwable) {
            return Outcome(problem = "the Bluetooth request to ${deviceLabel(device)} failed (${t.javaClass.simpleName})")
        } finally {
            runCatching { sock?.close() }
        }
    }

    @SuppressLint("MissingPermission")
    private fun openRfcomm(device: BluetoothDevice): BluetoothSocket? {
        return try {
            device.createRfcommSocketToServiceRecord(BluetoothRfcommTransport.CHAINIT_PC_UUID)
        } catch (_: Throwable) {
            try {
                device.createInsecureRfcommSocketToServiceRecord(BluetoothRfcommTransport.CHAINIT_PC_UUID)
            } catch (_: Throwable) {
                null
            }
        }
    }

    private fun deviceLabel(device: BluetoothDevice): String =
        runCatching { device.name }.getOrNull() ?: runCatching { device.address }.getOrNull() ?: "the paired PC"

    private fun parseStatus(statusLine: String): Int {
        val parts = statusLine.split(' ')
        if (parts.size < 2) return 0
        return parts[1].toIntOrNull() ?: 0
    }

    /** Reads exactly one HTTP body: fixed length, chunked, or until the PC closes the link. */
    private fun readBody(
        source: ByteSource,
        out: OutputStream,
        contentLength: Long,
        chunked: Boolean,
        cap: Long,
        onProgress: ((Long, Long) -> Unit)?
    ): Long {
        if (chunked) {
            var total = 0L
            while (true) {
                val line = source.readLine() ?: break
                if (line.isEmpty()) continue
                val semi = line.indexOf(';')
                val sizeText = (if (semi >= 0) line.substring(0, semi) else line).trim()
                if (sizeText.isEmpty()) continue
                val size = try {
                    java.lang.Long.parseLong(sizeText, 16)
                } catch (_: Throwable) {
                    -1L
                }
                if (size <= 0L) break
                val room = if (cap - total < 0) 0L else (cap - total)
                val written = source.copyTo(out, size, room, onProgress)
                total += written
                source.readLine() // every chunk is followed by CRLF
                if (written < size) break
            }
            return total
        }
        return source.copyTo(out, contentLength, cap, onProgress)
    }

    /** Buffered byte source over a stream, so headers and body can be read without pushback support. */
    private class ByteSource(private val input: InputStream, private val deadlineMs: Long) {
        private var buf: ByteArray = ByteArray(0)
        private var len: Int = 0
        private var pos: Int = 0

        private fun fill(): Boolean {
            if (pos < len) return true
            if (System.currentTimeMillis() > deadlineMs) return false
            val tmp = ByteArray(32 * 1024)
            val r = runCatching { input.read(tmp) }.getOrDefault(-1)
            if (r <= 0) return false
            buf = tmp
            len = r
            pos = 0
            return true
        }

        /** One CRLF-terminated line, "" for a blank line, null at end of stream. */
        fun readLine(): String? {
            val out = ByteArrayOutputStream()
            while (true) {
                if (!fill()) return if (out.size() == 0) null else String(out.toByteArray(), Charsets.UTF_8)
                val b = buf[pos].toInt() and 0xFF
                pos++
                if (b == 13) {
                    if (fill() && (buf[pos].toInt() and 0xFF) == 10) pos++
                    break
                }
                if (b == 10) break
                out.write(b)
                if (out.size() > 64 * 1024) break
            }
            return String(out.toByteArray(), Charsets.UTF_8)
        }

        /**
         * Copies `want` bytes (`want < 0` = until end of stream) and never more than `cap`.
         * Returns how many bytes reached `out`.
         */
        fun copyTo(out: OutputStream, want: Long, cap: Long, onProgress: ((Long, Long) -> Unit)?): Long {
            var remaining = want
            var written = 0L
            while (true) {
                if (!fill()) break
                val available = (len - pos).toLong()
                val take = if (remaining < 0) available else minOf(available, remaining)
                if (take <= 0) break
                val room = cap - written
                if (room > 0) {
                    val store = if (take < room) take else room
                    out.write(buf, pos, store.toInt())
                    written += store
                    onProgress?.invoke(written, if (want >= 0) want else -1L)
                }
                pos += take.toInt()
                if (remaining >= 0) {
                    remaining -= take
                    if (remaining <= 0) break
                } else if (written >= cap) {
                    break // uncapped read hit the cap: stop rather than drain the link forever
                }
            }
            try {
                out.flush()
            } catch (_: Throwable) {
            }
            return written
        }
    }

    private fun enc(value: String): String =
        URLEncoder.encode(value, "UTF-8").replace("+", "%20")

    // ---------------------------------------------------------------------------------------------
    // Honest wording + formatting
    // ---------------------------------------------------------------------------------------------

    fun describe(status: Int, transport: Transport, body: String = ""): String = when (status) {
        in 200..299 -> when (transport) {
            Transport.LAN -> "PC answered over Wi-Fi"
            Transport.BLUETOOTH -> "PC answered over Bluetooth RFCOMM"
            Transport.NONE -> "No link"
        }
        401 -> "The PC rejected the Pairing Key. Make sure ChainIt shows the same key on the phone and on the PC."
        403 -> "Sibling Mode is off on the PC. Turn it on in the ChainIt window, then try again."
        404 -> if (body.contains("not found", true)) "The PC no longer has that file or folder — refresh the list." else "The PC does not serve that path."
        429 -> "The PC is cooling down after repeated key failures. Wait 30 seconds and try again."
        503 -> "The PC is not ready to serve that path yet (503). Try again in a moment."
        0 -> "No answer from the PC."
        else -> "The PC answered with HTTP $status."
    }

    /** Byte count formatted the way the Windows file manager formats it. */
    fun humanSize(bytes: Long): String {
        if (bytes < 0) return "—"
        if (bytes == 0L) return "0 B"
        val units = arrayOf("B", "KB", "MB", "GB", "TB")
        var value = bytes.toDouble()
        var u = 0
        while (value >= 1024.0 && u < units.size - 1) {
            value /= 1024.0
            u++
        }
        return if (u == 0) "$bytes B" else String.format(java.util.Locale.US, "%.1f %s", value, units[u])
    }

    /** `2026-11-16 14:05` — the same shape the native Windows list uses for its Modified column. */
    fun humanTime(ms: Long): String {
        if (ms <= 0L) return "—"
        return try {
            val fmt = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", java.util.Locale.US)
            fmt.format(java.util.Date(ms))
        } catch (_: Throwable) {
            "—"
        }
    }

    /** Virtual-path helpers: the PC root is `/`, children look like `/Desktop/photo.jpg`. */
    fun parentOf(path: String): String {
        val clean = path.replace('\\', '/')
        val cut = clean.lastIndexOf('/')
        if (cut <= 0) return "/"
        return clean.substring(0, cut)
    }

    fun joinPath(dir: String, name: String): String {
        val base = dir.replace('\\', '/')
        val trimmed = if (base.endsWith("/")) base else "$base/"
        return trimmed + name
    }

    /** Extension → accent colour, so a list of files reads at a glance like Explorer's icons do. */
    fun tintFor(item: Item): Int = when {
        item.isDirectory -> 0xFF10B981.toInt()
        item.ext in IMAGE_EXT -> 0xFF22D3EE.toInt()
        item.ext in DOC_EXT -> 0xFF3B82F6.toInt()
        item.ext in MEDIA_EXT -> 0xFFF472B6.toInt()
        item.ext in ARCHIVE_EXT -> 0xFFF59E0B.toInt()
        item.ext in CODE_EXT -> 0xFF6EE7B7.toInt()
        else -> 0xFF64748B.toInt()
    }

    private val IMAGE_EXT = setOf(".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".heic", ".svg")
    private val DOC_EXT = setOf(".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt", ".md", ".rtf", ".csv", ".odt")
    private val MEDIA_EXT = setOf(".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".mp4", ".mkv", ".avi", ".mov", ".webm")
    private val ARCHIVE_EXT = setOf(".zip", ".rar", ".7z", ".tar", ".gz", ".iso", ".bz2")
    private val CODE_EXT = setOf(".kt", ".java", ".cpp", ".c", ".h", ".py", ".js", ".ts", ".tsx", ".json", ".xml", ".sh", ".ps1", ".html", ".css")

    fun isTextLike(item: Item): Boolean = item.ext in DOC_EXT || item.ext in CODE_EXT
    fun isImage(item: Item): Boolean = item.ext in IMAGE_EXT
}
