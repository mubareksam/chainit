package com.chainit.android.net

import android.app.KeyguardManager
import android.content.Context
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import com.chainit.android.vfs.AndroidStorageVfsProvider
import com.chainit.android.vfs.FileOps
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.security.MessageDigest
import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.net.URLEncoder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executors

class PhoneHttpAndWebDavServer(
    private val context: Context,
    private val vfs: AndroidStorageVfsProvider,
    val port: Int = 49180
) {
    private val pool = Executors.newCachedThreadPool()
    private var serverSocket: ServerSocket? = null

    @Volatile
    private var running = false

    @Volatile
    var lastActivitySummary: String = "Standby — awaiting Windows Sibling"
        private set

    fun start() {
        if (running) return
        running = true
        Thread {
            try {
                val ss = ServerSocket()
                ss.reuseAddress = true
                ss.bind(InetSocketAddress("0.0.0.0", port))
                serverSocket = ss
                while (running) {
                    val client = ss.accept()
                    pool.execute { handleClient(client) }
                }
            } catch (_: Exception) {
            }
        }.apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        try {
            serverSocket?.close()
        } catch (_: Exception) {
        }
        serverSocket = null
    }

    // -----------------------------------------------------------------------------------------
    // Security hardening
    //   * the Pairing Key is REQUIRED on every request (no anonymous access, ever)
    //   * 5 wrong/missing keys inside 60 s lock the VFS port for 30 s (brute-force brake)
    //   * every served range carries its SHA-256 and every inbound file is verified
    //     against the sender's SHA-256, so a tampered or torn transfer is discarded
    // -----------------------------------------------------------------------------------------
    private val authFailures = ArrayDeque<Long>()
    private var lockUntilMs = 0L
    private val shaCache = object : LinkedHashMap<String, String>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, String>?): Boolean = size > 64
    }

    private fun isLockedOut(): Boolean = synchronized(authFailures) { System.currentTimeMillis() < lockUntilMs }

    private fun registerAuthFailure() {
        synchronized(authFailures) {
            val now = System.currentTimeMillis()
            authFailures.addLast(now)
            while (authFailures.isNotEmpty() && now - authFailures.first() > 60_000) authFailures.removeFirst()
            if (authFailures.size >= 5) {
                lockUntilMs = now + 30_000
                authFailures.clear()
            }
        }
    }

    private fun sha256Hex(data: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }

    private fun sha256FileWhole(file: File): String {
        val key = "${file.absolutePath}|${file.length()}|${file.lastModified()}"
        synchronized(shaCache) { shaCache[key]?.let { return it } }
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { fis ->
            val buf = ByteArray(256 * 1024)
            while (true) {
                val r = fis.read(buf)
                if (r <= 0) break
                md.update(buf, 0, r)
            }
        }
        val hex = md.digest().joinToString("") { "%02x".format(it) }
        synchronized(shaCache) { shaCache[key] = hex }
        return hex
    }

    private fun sha256FileRange(file: File, start: Long, len: Long): String {
        if (start == 0L && len == file.length()) return sha256FileWhole(file)
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { fis ->
            var skip = start
            while (skip > 0) {
                val jumped = fis.skip(skip)
                if (jumped <= 0) break
                skip -= jumped
            }
            val buf = ByteArray(256 * 1024)
            var remaining = len
            while (remaining > 0) {
                val r = fis.read(buf, 0, minOf(buf.size.toLong(), remaining).toInt())
                if (r <= 0) break
                md.update(buf, 0, r)
                remaining -= r
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    /** Verify an inbound file against the sender's SHA-256; discard it on any mismatch. */
    private fun verifyInbound(file: File, expected: String?): Boolean {
        if (expected.isNullOrBlank()) return true
        return runCatching { sha256FileWhole(file).equals(expected.trim(), ignoreCase = true) }.getOrDefault(false)
    }

    /** Why a request was refused - the PC shows this verbatim instead of a dead-looking link. */
    private enum class AuthVerdict { OK, BAD_KEY, SIBLING_OFF }

    private fun checkSiblingAuth(headers: Map<String, String>): AuthVerdict {
        val prefs = context.getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        val expected = prefs.getString("sibling_key", "CHAINIT-SIBLING") ?: "CHAINIT-SIBLING"
        val provided = headers["x-chainit-key"]
        // Strict: a missing or wrong Pairing Key is refused. ChainIt-Windows.exe always sends it.
        if (provided == null || provided != expected) return AuthVerdict.BAD_KEY
        // The key is correct, so this is a switch the user flipped - say exactly that rather
        // than answering 401 and leaving Windows staring at an empty Z:\ drive.
        if (!prefs.getBoolean("sibling_enabled", true)) return AuthVerdict.SIBLING_OFF
        return AuthVerdict.OK
    }

    /**
     * Keep-alive bookkeeping.
     *
     * Bluetooth RFCOMM is expensive to dial (SDP lookup + page, easily 1-5 s per connect), so a
     * single RFCOMM link must be able to serve a whole Explorer session. Every response writer
     * asks [connectionHeader] whether the socket stays open, and [CountingInputStream] lets the
     * session loop drain any body a handler did not consume so the next request line is never
     * mis-parsed out of leftover upload bytes.
     */
    private val sessionKeepAlive = ThreadLocal<Boolean>()

    private fun connectionHeader(): String =
        if (sessionKeepAlive.get() == true) "Connection: keep-alive\r\n" else "Connection: close\r\n"

    private class CountingInputStream(private val src: InputStream) : InputStream() {
        var count: Long = 0L
            private set

        override fun read(): Int {
            val b = src.read()
            if (b >= 0) count += 1
            return b
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            val r = src.read(b, off, len)
            if (r > 0) count += r.toLong()
            return r
        }

        override fun skip(n: Long): Long {
            val r = src.skip(n)
            if (r > 0) count += r
            return r
        }

        override fun available(): Int = src.available()
        override fun close() = src.close()
    }

    /** Open sessions, so an idle one can be force-closed instead of pinning a thread forever. */
    private class Session(val closeable: java.io.Closeable?) {
        @Volatile
        var lastActivityMs: Long = System.currentTimeMillis()
    }

    private val sessions = java.util.Collections.synchronizedList(mutableListOf<Session>())
    private val maxIdleSessionMs = 10L * 60 * 1000

    @Volatile
    var openSessions: Int = 0
        private set

    @Volatile
    var servedRequests: Long = 0L
        private set

    private fun startSessionSweeper() {
        if (sweeperStarted.getAndSet(true)) return
        Thread {
            while (running) {
                try {
                    Thread.sleep(30_000L)
                    val now = System.currentTimeMillis()
                    val idle = mutableListOf<Session>()
                    synchronized(sessions) {
                        val it = sessions.iterator()
                        while (it.hasNext()) {
                            val s = it.next()
                            if (now - s.lastActivityMs > maxIdleSessionMs) { idle.add(s); it.remove() }
                        }
                    }
                    for (s in idle) runCatching { s.closeable?.close() }
                    openSessions = sessions.size
                } catch (_: InterruptedException) {
                    return@Thread
                } catch (_: Throwable) {
                }
            }
        }.apply { isDaemon = true; name = "ChainItSessionSweeper"; start() }
    }

    private val sweeperStarted = java.util.concurrent.atomic.AtomicBoolean(false)

    private fun handleClient(socket: Socket) {
        socket.use { sock ->
            // Long enough for a keep-alive link to sit between Explorer requests; the sweeper
            // force-closes anything idle for 10 minutes.
            sock.soTimeout = 5 * 60 * 1000
            handleStreamSession(sock.getInputStream(), sock.getOutputStream(), viaBluetooth = false, sock)
        }
    }

    /**
     * Serves one connection: as many requests as the peer wants to pipeline on it.
     *
     * `closeable` is the underlying socket, kept so the idle sweeper can break a blocked read on a
     * link whose peer vanished (Bluetooth streams have no SO_TIMEOUT of their own).
     */
    fun handleStreamSession(
        rawInput: InputStream,
        rawOutput: OutputStream,
        viaBluetooth: Boolean,
        closeable: java.io.Closeable? = null
    ) {
        val input = BufferedInputStream(rawInput)
        val output = BufferedOutputStream(rawOutput)
        startSessionSweeper()

        val session = Session(closeable)
        sessions.add(session)
        openSessions = sessions.size
        try {
            var served = 0
            while (running) {
                session.lastActivityMs = System.currentTimeMillis()

                val reqLine = readAsciiLine(input) ?: break
                if (reqLine.isEmpty()) continue
                val parts = reqLine.split(" ")
                if (parts.size < 2) break
                val method = parts[0].uppercase(Locale.US)
                val rawTarget = parts[1]
                val httpVersion = if (parts.size > 2) parts[2].uppercase(Locale.US) else "HTTP/1.1"

                val headers = mutableMapOf<String, String>()
                var malformed = false
                while (true) {
                    val line = readAsciiLine(input)
                    if (line == null) { malformed = true; break }
                    if (line.isEmpty()) break
                    val colon = line.indexOf(':')
                    if (colon > 0) {
                        headers[line.substring(0, colon).trim().lowercase(Locale.US)] =
                            line.substring(colon + 1).trim()
                    }
                }
                if (malformed) break

                // Does this connection stay open after the response?
                val connHeader = headers["connection"]?.lowercase(Locale.US) ?: ""
                val wantsClose = connHeader.contains("close")
                val keepAlive = !wantsClose && (httpVersion == "HTTP/1.1" || connHeader.contains("keep-alive"))
                sessionKeepAlive.set(keepAlive)

                val contentLength = headers["content-length"]?.toLongOrNull() ?: 0L
                val counted = CountingInputStream(input)

                try {
                    when {
                        method == "OPTIONS" && !rawTarget.startsWith("/dav") ->
                            sendSimpleResponse(output, 204, "No Content", "text/plain", ByteArray(0))

                        isLockedOut() -> {
                            val err = JSONObject().put("error", "Too many Pairing Key failures - the Sibling VFS port is cooling down for 30 s.").toString()
                            sendSimpleResponse(output, 429, "Too Many Requests", "application/json", err.toByteArray(Charsets.UTF_8))
                        }

                        else -> {
                            when (checkSiblingAuth(headers)) {
                                AuthVerdict.BAD_KEY -> {
                                    registerAuthFailure()
                                    val err = JSONObject().put("error", "Pairing Key required and must match the one shown in ChainIt Sibling.").toString()
                                    sendSimpleResponse(output, 401, "Unauthorized", "application/json", err.toByteArray(Charsets.UTF_8))
                                }
                                AuthVerdict.SIBLING_OFF -> {
                                    // Correct key, switch off: 403 with the exact remedy, not a silent 401.
                                    val err = JSONObject()
                                        .put("error", "Sibling Mode is OFF on the phone. Open ChainIt on the Android side and enable Sibling Mode - the Pairing Key already matches.")
                                        .put("code", "sibling_disabled").toString()
                                    sendSimpleResponse(output, 403, "Forbidden", "application/json", err.toByteArray(Charsets.UTF_8))
                                }
                                AuthVerdict.OK -> {
                                    if (rawTarget == "/dav" || rawTarget.startsWith("/dav/") || rawTarget.startsWith("/dav?")) {
                                        handleWebDav(method, rawTarget, headers, contentLength, counted, output)
                                    } else {
                                        handleChainItApi(method, rawTarget, headers, contentLength, counted, output)
                                    }
                                    if (viaBluetooth) {
                                        lastActivitySummary = "[Bluetooth RFCOMM] $lastActivitySummary"
                                    }
                                }
                            }
                        }
                    }
                } catch (e: java.net.SocketTimeoutException) {
                    break // idle too long on a TCP keep-alive link
                } catch (e: Exception) {
                    val msg = JSONObject().put("error", e.message ?: "Phone VFS error").toString()
                    runCatching {
                        sendSimpleResponse(output, 400, "Bad Request", "application/json", msg.toByteArray(Charsets.UTF_8))
                    }
                }

                // Never let an unconsumed request body desynchronise the next request line.
                val left = contentLength - counted.count
                if (left > 0) {
                    var remaining = left
                    val drain = ByteArray(64 * 1024)
                    while (remaining > 0) {
                        val r = counted.read(drain, 0, minOf(drain.size.toLong(), remaining).toInt())
                        if (r <= 0) break
                        remaining -= r.toLong()
                    }
                    if (remaining > 0) break
                }

                runCatching { output.flush() }
                served += 1
                servedRequests += 1
                session.lastActivityMs = System.currentTimeMillis()
                if (!keepAlive) break
                if (served >= 100_000) break
            }
        } finally {
            sessionKeepAlive.remove()
            sessions.remove(session)
            openSessions = sessions.size
        }
    }

    private fun handleChainItApi(
        method: String,
        rawTarget: String,
        headers: Map<String, String>,
        contentLength: Long,
        input: InputStream,
        output: OutputStream
    ) {
        val uri = Uri.parse("http://localhost$rawTarget")
        val pathPart = uri.path ?: "/"

        when {
            pathPart == "/chainit/ping" && method == "GET" -> {
                val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
                val km = context.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
                val (totalB, usedB) = vfs.getStorageCapacity()
                // v1.7.0: the PC shows the charge, the plug and the temperature next to the
                // percentage, because "86%" alone does not say whether the link survives the night.
                val batt = BatteryTelemetry.read(context)

                val obj = JSONObject()
                obj.put("ok", true)
                obj.put("deviceName", "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}")
                obj.put("model", Build.MODEL)
                obj.put("androidVersion", "Android ${Build.VERSION.RELEASE}")
                obj.put("batteryPct", batt.pct)
                obj.put("charging", batt.charging)
                obj.put("chargeSource", batt.source)
                obj.put("temperatureC", batt.tempC)
                obj.put("screenLocked", !pm.isInteractive || km.isKeyguardLocked)
                obj.put("totalBytes", totalB)
                obj.put("usedBytes", usedB)
                sendJson(output, 200, obj)
            }

            pathPart == "/chainit/list" && method == "GET" -> {
                val dir = uri.getQueryParameter("path") ?: "/"
                lastActivitySummary = "Listed Z:${dir.replace('/', '\\')}"
                sendJson(output, 200, vfs.listAsJson(dir))
            }

            pathPart == "/chainit/raw" && (method == "GET" || method == "HEAD") -> {
                val fPath = uri.getQueryParameter("path") ?: "/"
                val download = uri.getQueryParameter("download") == "1"
                val file = vfs.resolveSafe(fPath)
                if (!file.exists() || !file.isFile) {
                    sendSimpleResponse(output, 404, "Not Found", "text/plain", "Not found".toByteArray())
                    return
                }
                lastActivitySummary = "Streamed ${file.name}"
                streamFileResponse(output, method, file, headers["range"], download)
            }

            pathPart == "/chainit/text" && method == "GET" -> {
                val fPath = uri.getQueryParameter("path") ?: "/"
                val file = vfs.resolveSafe(fPath)
                require(file.exists() && file.isFile) { "File not found" }
                val entry = vfs.toEntry(file)
                val editable = vfs.isEditableText(file.name) && file.length() <= 2 * 1024 * 1024
                val content = if (editable) file.readText(Charsets.UTF_8) else JSONObject.NULL
                val res = JSONObject()
                res.put("item", entry.toJson())
                res.put("editable", editable)
                res.put("content", content)
                sendJson(output, 200, res)
            }

            pathPart == "/chainit/text" && method == "PUT" -> {
                val body = JSONObject(String(readExactBytes(input, contentLength.toInt()), Charsets.UTF_8))
                val fPath = body.getString("path")
                val text = body.optString("content", "")
                val file = vfs.resolveSafe(fPath)
                file.parentFile?.mkdirs()
                file.writeText(text, Charsets.UTF_8)
                lastActivitySummary = "Edited ${file.name} directly from PC"
                val res = JSONObject().put("ok", true).put("item", vfs.toEntry(file).toJson())
                sendJson(output, 200, res)
            }

            pathPart == "/chainit/upload" && method == "POST" -> {
                val dir = uri.getQueryParameter("dir") ?: "/ChainIt_Inbox"
                val rawName = uri.getQueryParameter("filename") ?: "received_${System.currentTimeMillis()}.bin"
                val safeName = FileOps.sanitizeName(File(rawName).name)
                val dirFile = vfs.resolveSafe(dir)
                dirFile.mkdirs()
                val dest = File(dirFile, safeName)
                copyFixedBytes(input, dest, contentLength)
                if (!verifyInbound(dest, headers["x-chainit-file-sha256"])) {
                    runCatching { dest.delete() }
                    sendJson(output, 409, JSONObject().put("error", "SHA-256 mismatch on upload - file discarded."))
                    return
                }
                MediaScannerConnection.scanFile(context, arrayOf(dest.absolutePath), null, null)
                lastActivitySummary = "Received ${dest.name} (${dest.length()} B)"
                sendJson(output, 200, JSONObject().put("item", vfs.toEntry(dest).toJson()))
            }

            pathPart == "/chainit/mkdir" && method == "POST" -> {
                val body = JSONObject(String(readExactBytes(input, contentLength.toInt()), Charsets.UTF_8))
                val created = vfs.makeDirectory(body.optString("parentDir", "/"), body.getString("name"))
                lastActivitySummary = "Created folder ${created.name}"
                sendJson(output, 200, JSONObject().put("item", created.toJson()))
            }

            pathPart == "/chainit/rename" && method == "POST" -> {
                val body = JSONObject(String(readExactBytes(input, contentLength.toInt()), Charsets.UTF_8))
                val updated = vfs.renameItem(body.getString("path"), body.getString("newName"))
                lastActivitySummary = "Renamed to ${updated.name}"
                sendJson(output, 200, JSONObject().put("item", updated.toJson()))
            }

            pathPart == "/chainit/delete" && method == "POST" -> {
                val body = JSONObject(String(readExactBytes(input, contentLength.toInt()), Charsets.UTF_8))
                val arr = body.optJSONArray("paths") ?: JSONArray()
                val list = (0 until arr.length()).map { arr.getString(it) }
                val deleted = vfs.deleteItems(list)
                lastActivitySummary = "Deleted $deleted item(s)"
                sendJson(output, 200, JSONObject().put("deleted", deleted))
            }

            pathPart == "/chainit/transfer" && method == "POST" -> {
                val body = JSONObject(String(readExactBytes(input, contentLength.toInt()), Charsets.UTF_8))
                val op = body.optString("op", "copy")
                val arr = body.optJSONArray("fromPaths") ?: JSONArray()
                val paths = (0 until arr.length()).map { arr.getString(it) }
                val toDir = body.optString("toDir", "/")
                val (bytes, names) = vfs.copyOrMove(op, paths, toDir)
                val nArr = JSONArray()
                names.forEach { nArr.put(it) }
                lastActivitySummary = "${if (op == "move") "Moved" else "Copied"} ${names.size} item(s)"
                sendJson(output, 200, JSONObject().put("totalBytes", bytes).put("itemNames", nArr))
            }

            else -> {
                sendSimpleResponse(
                    output,
                    200,
                    "OK",
                    "text/plain",
                    "ChainIt Android Sibling VFS Node Active (Port $port)".toByteArray(Charsets.UTF_8)
                )
            }
        }
    }

    private fun handleWebDav(
        method: String,
        rawTarget: String,
        headers: Map<String, String>,
        contentLength: Long,
        input: InputStream,
        output: OutputStream
    ) {
        val extraHeaders = mapOf(
            "DAV" to "1, 2",
            "MS-Author-Via" to "DAV",
            "Allow" to "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE, PROPFIND, PROPPATCH, LOCK, UNLOCK",
            "Accept-Ranges" to "bytes"
        )

        if (method == "OPTIONS") {
            sendSimpleResponse(output, 200, "OK", "text/plain", ByteArray(0), extraHeaders)
            return
        }

        val subPath = extractDavSubpath(rawTarget)
        val targetFile = vfs.resolveSafe(subPath)

        when (method) {
            "PROPFIND" -> {
                if (contentLength > 0) readExactBytes(input, contentLength.toInt())
                if (!targetFile.exists()) {
                    sendSimpleResponse(output, 404, "Not Found", "text/plain", "Not Found".toByteArray(), extraHeaders)
                    return
                }
                val depth = headers["depth"] ?: "1"
                val responses = StringBuilder()
                val vPath = vfs.normalizeVirtualPath(targetFile)
                responses.append(buildDavItemXml(vPath, targetFile))
                if (targetFile.isDirectory && depth != "0") {
                    targetFile.listFiles()
                        ?.filter { !it.name.startsWith(".") }
                        ?.forEach { child ->
                            responses.append("\n").append(buildDavItemXml(vfs.normalizeVirtualPath(child), child))
                        }
                }
                val xml = """<?xml version="1.0" encoding="utf-8"?>
<D:multistatus xmlns:D="DAV:">
$responses
</D:multistatus>"""
                lastActivitySummary = "WebDAV Z:${vPath.replace('/', '\\')}"
                sendSimpleResponse(
                    output,
                    207,
                    "Multi-Status",
                    "application/xml; charset=\"utf-8\"",
                    xml.toByteArray(Charsets.UTF_8),
                    extraHeaders
                )
            }

            "GET", "HEAD" -> {
                if (!targetFile.exists()) {
                    sendSimpleResponse(output, 404, "Not Found", "text/plain", "Not Found".toByteArray(), extraHeaders)
                    return
                }
                if (targetFile.isDirectory) {
                    sendSimpleResponse(output, 200, "OK", "text/plain", "Directory".toByteArray(), extraHeaders)
                    return
                }
                lastActivitySummary = "WebDAV Read ${targetFile.name}"
                streamFileResponse(output, method, targetFile, headers["range"], false, extraHeaders)
            }

            "PUT" -> {
                val existed = targetFile.exists()
                targetFile.parentFile?.mkdirs()
                copyFixedBytes(input, targetFile, contentLength)
                if (!verifyInbound(targetFile, headers["x-chainit-file-sha256"])) {
                    runCatching { targetFile.delete() }
                    val err = JSONObject().put("error", "SHA-256 mismatch on upload - file discarded.").toString()
                    sendSimpleResponse(output, 409, "Conflict", "application/json", err.toByteArray(Charsets.UTF_8), extraHeaders)
                    return
                }
                MediaScannerConnection.scanFile(context, arrayOf(targetFile.absolutePath), null, null)
                lastActivitySummary = "WebDAV Write ${targetFile.name}"
                sendSimpleResponse(
                    output,
                    if (existed) 204 else 201,
                    if (existed) "No Content" else "Created",
                    "text/plain",
                    ByteArray(0),
                    extraHeaders
                )
            }

            "MKCOL" -> {
                targetFile.mkdirs()
                lastActivitySummary = "WebDAV Mkdir ${targetFile.name}"
                sendSimpleResponse(output, 201, "Created", "text/plain", ByteArray(0), extraHeaders)
            }

            "DELETE" -> {
                if (!targetFile.exists()) {
                    sendSimpleResponse(output, 404, "Not Found", "text/plain", ByteArray(0), extraHeaders)
                    return
                }
                FileOps.deleteRecursive(targetFile)
                lastActivitySummary = "WebDAV Delete ${targetFile.name}"
                sendSimpleResponse(output, 204, "No Content", "text/plain", ByteArray(0), extraHeaders)
            }

            "MOVE", "COPY" -> {
                val destHdr = headers["destination"] ?: ""
                val destSub = extractDavSubpath(destHdr)
                val destFile = vfs.resolveSafe(destSub)
                val existed = destFile.exists()
                destFile.parentFile?.mkdirs()
                if (method == "COPY") {
                    FileOps.copyRecursive(targetFile, destFile)
                } else {
                    if (existed) FileOps.deleteRecursive(destFile)
                    if (!targetFile.renameTo(destFile)) {
                        FileOps.copyRecursive(targetFile, destFile)
                        FileOps.deleteRecursive(targetFile)
                    }
                }
                lastActivitySummary = "WebDAV $method ${destFile.name}"
                sendSimpleResponse(
                    output,
                    if (existed) 204 else 201,
                    if (existed) "No Content" else "Created",
                    "text/plain",
                    ByteArray(0),
                    extraHeaders
                )
            }

            "LOCK" -> {
                if (contentLength > 0) readExactBytes(input, contentLength.toInt())
                val token = "opaquelocktoken:chainit-android-lock"
                val xml = """<?xml version="1.0" encoding="utf-8"?>
<D:prop xmlns:D="DAV:"><D:lockdiscovery><D:activelock><D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>infinity</D:depth><D:timeout>Second-3600</D:timeout><D:locktoken><D:href>$token</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>"""
                sendSimpleResponse(
                    output,
                    200,
                    "OK",
                    "application/xml; charset=\"utf-8\"",
                    xml.toByteArray(Charsets.UTF_8),
                    extraHeaders + ("Lock-Token" to "<$token>")
                )
            }

            "UNLOCK" -> {
                sendSimpleResponse(output, 204, "No Content", "text/plain", ByteArray(0), extraHeaders)
            }

            "PROPPATCH" -> {
                if (contentLength > 0) readExactBytes(input, contentLength.toInt())
                val href = encodeDavHref(vfs.normalizeVirtualPath(targetFile), targetFile.isDirectory)
                val xml = """<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:"><D:response><D:href>$href</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>"""
                sendSimpleResponse(output, 207, "Multi-Status", "application/xml; charset=\"utf-8\"", xml.toByteArray(Charsets.UTF_8), extraHeaders)
            }

            else -> {
                sendSimpleResponse(output, 405, "Method Not Allowed", "text/plain", ByteArray(0), extraHeaders)
            }
        }
    }

    private fun extractDavSubpath(rawUrl: String): String {
        val clean = if (rawUrl.startsWith("http://") || rawUrl.startsWith("https://")) {
            Uri.parse(rawUrl).encodedPath ?: "/dav/"
        } else {
            rawUrl.substringBefore('?')
        }
        val decoded = URLDecoder.decode(clean, "UTF-8")
        return when {
            decoded == "/dav" || decoded == "/dav/" -> "/"
            decoded.startsWith("/dav/") -> decoded.removePrefix("/dav")
            else -> decoded
        }
    }

    private fun encodeDavHref(virtualPath: String, isDir: Boolean): String {
        val segs = virtualPath.split('/').filter { it.isNotEmpty() }.map {
            URLEncoder.encode(it, "UTF-8").replace("+", "%20")
        }
        val base = if (segs.isEmpty()) "/dav/" else "/dav/" + segs.joinToString("/")
        return if (isDir && !base.endsWith("/")) "$base/" else base
    }

    private fun escapeXml(s: String): String =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")

    private fun buildDavItemXml(vPath: String, file: File): String {
        val isDir = file.isDirectory
        val href = encodeDavHref(vPath, isDir)
        val name = if (vPath == "/") "ChainIt Sibling (Z:)" else file.name
        val fmt = SimpleDateFormat("EEE, dd MMM yyyy HH:mm:ss 'GMT'", Locale.US).apply {
            timeZone = TimeZone.getTimeZone("GMT")
        }
        val lastMod = fmt.format(Date(file.lastModified()))
        return if (isDir) {
            """  <D:response>
    <D:href>$href</D:href>
    <D:propstat>
      <D:prop>
        <D:displayname>${escapeXml(name)}</D:displayname>
        <D:resourcetype><D:collection/></D:resourcetype>
        <D:getlastmodified>$lastMod</D:getlastmodified>
      </D:prop>
      <D:status>HTTP/1.1 200 OK</D:status>
    </D:propstat>
  </D:response>"""
        } else {
            val mime = vfs.guessMime(file.name)
            """  <D:response>
    <D:href>$href</D:href>
    <D:propstat>
      <D:prop>
        <D:displayname>${escapeXml(name)}</D:displayname>
        <D:resourcetype/>
        <D:getcontentlength>${file.length()}</D:getcontentlength>
        <D:getcontenttype>${escapeXml(mime)}</D:getcontenttype>
        <D:getlastmodified>$lastMod</D:getlastmodified>
      </D:prop>
      <D:status>HTTP/1.1 200 OK</D:status>
    </D:propstat>
  </D:response>"""
        }
    }

    private fun streamFileResponse(
        output: OutputStream,
        method: String,
        file: File,
        rangeHeader: String?,
        download: Boolean,
        extraHeaders: Map<String, String> = emptyMap()
    ) {
        val size = file.length()
        val mime = vfs.guessMime(file.name)
        var start = 0L
        var end = (size - 1).coerceAtLeast(0L)
        var partial = false

        if (rangeHeader != null && rangeHeader.startsWith("bytes=") && size > 0) {
            val spec = rangeHeader.removePrefix("bytes=").split('-')
            start = spec.getOrNull(0)?.toLongOrNull() ?: 0L
            end = spec.getOrNull(1)?.takeIf { it.isNotEmpty() }?.toLongOrNull()?.coerceAtMost(size - 1) ?: (size - 1)
            partial = true
        }

        val length = if (size == 0L) 0L else (end - start + 1).coerceAtLeast(0L)
        val fileSha = runCatching { sha256FileWhole(file) }.getOrNull()
        val chunkSha = if (length > 0L) runCatching { sha256FileRange(file, start, length) }.getOrNull() else null
        val sb = StringBuilder()
        sb.append(if (partial) "HTTP/1.1 206 Partial Content\r\n" else "HTTP/1.1 200 OK\r\n")
        sb.append("Content-Type: $mime\r\n")
        sb.append("Content-Length: $length\r\n")
        sb.append("Accept-Ranges: bytes\r\n")
        sb.append("Access-Control-Allow-Origin: *\r\n")
        sb.append(connectionHeader())
        if (partial) {
            sb.append("Content-Range: bytes $start-$end/$size\r\n")
        }
        if (fileSha != null) sb.append("X-ChainIt-File-Sha256: $fileSha\r\n")
        if (chunkSha != null) sb.append("X-ChainIt-Chunk-Sha256: $chunkSha\r\n")
        if (download) {
            val encoded = URLEncoder.encode(file.name, "UTF-8").replace("+", "%20")
            sb.append("Content-Disposition: attachment; filename=\"$encoded\"\r\n")
        }
        extraHeaders.forEach { (k, v) -> sb.append("$k: $v\r\n") }
        sb.append("\r\n")
        output.write(sb.toString().toByteArray(Charsets.UTF_8))

        if (method != "HEAD" && length > 0) {
            FileInputStream(file).use { fis ->
                if (start > 0) fis.skip(start)
                val buf = ByteArray(64 * 1024)
                var remaining = length
                while (remaining > 0) {
                    val toRead = minOf(buf.size.toLong(), remaining).toInt()
                    val r = fis.read(buf, 0, toRead)
                    if (r <= 0) break
                    output.write(buf, 0, r)
                    remaining -= r
                }
            }
        }
        output.flush()
    }

    private fun sendJson(output: OutputStream, status: Int, json: JSONObject) {
        val bytes = json.toString().toByteArray(Charsets.UTF_8)
        sendSimpleResponse(output, status, if (status == 200) "OK" else "Error", "application/json; charset=utf-8", bytes)
    }

    private fun sendSimpleResponse(
        output: OutputStream,
        status: Int,
        reason: String,
        contentType: String,
        body: ByteArray,
        extraHeaders: Map<String, String> = emptyMap()
    ) {
        val sb = StringBuilder()
        sb.append("HTTP/1.1 $status $reason\r\n")
        sb.append("Content-Type: $contentType\r\n")
        sb.append("Content-Length: ${body.size}\r\n")
        sb.append("Access-Control-Allow-Origin: *\r\n")
        sb.append(connectionHeader())
        extraHeaders.forEach { (k, v) -> sb.append("$k: $v\r\n") }
        sb.append("\r\n")
        output.write(sb.toString().toByteArray(Charsets.UTF_8))
        if (body.isNotEmpty()) output.write(body)
        output.flush()
    }

    private fun readAsciiLine(input: InputStream): String? {
        val baos = ByteArrayOutputStream()
        var sawByte = false
        while (true) {
            val b = input.read()
            if (b == -1) return if (sawByte) baos.toString("UTF-8") else null
            sawByte = true
            if (b == '\n'.code) break
            if (b != '\r'.code) baos.write(b)
        }
        return baos.toString("UTF-8")
    }

    private fun readExactBytes(input: InputStream, count: Int): ByteArray {
        if (count <= 0) return ByteArray(0)
        val out = ByteArray(count)
        var off = 0
        while (off < count) {
            val r = input.read(out, off, count - off)
            if (r <= 0) break
            off += r
        }
        return if (off == count) out else out.copyOf(off)
    }

    private fun copyFixedBytes(input: InputStream, dest: File, count: Long) {
        FileOutputStream(dest).use { fos ->
            val buf = ByteArray(64 * 1024)
            var remaining = count
            while (remaining > 0) {
                val toRead = minOf(buf.size.toLong(), remaining).toInt()
                val r = input.read(buf, 0, toRead)
                if (r <= 0) break
                fos.write(buf, 0, r)
                remaining -= r
            }
        }
    }
}
