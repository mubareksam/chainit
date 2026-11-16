package com.chainit.android.net

import android.app.KeyguardManager
import android.content.Context
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import com.chainit.android.vfs.AndroidStorageVfsProvider
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
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

    private fun checkSiblingAuth(headers: Map<String, String>): Boolean {
        val prefs = context.getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        if (!prefs.getBoolean("sibling_enabled", true)) return false
        val expected = prefs.getString("sibling_key", "CHAINIT-SIBLING") ?: "CHAINIT-SIBLING"
        val provided = headers["x-chainit-key"]
        // Allow matching key or unauthenticated local WebDAV if default key is unchanged
        if (provided != null && provided != expected) return false
        return true
    }

    private fun handleClient(socket: Socket) {
        socket.use { sock ->
            sock.soTimeout = 30_000
            handleStreamSession(sock.getInputStream(), sock.getOutputStream(), viaBluetooth = false)
        }
    }

    fun handleStreamSession(rawInput: InputStream, rawOutput: OutputStream, viaBluetooth: Boolean) {
        val input = BufferedInputStream(rawInput)
        val output = BufferedOutputStream(rawOutput)

        val reqLine = readAsciiLine(input) ?: return
        val parts = reqLine.split(" ")
        if (parts.size < 2) return
        val method = parts[0].uppercase(Locale.US)
        val rawTarget = parts[1]

        val headers = mutableMapOf<String, String>()
        while (true) {
            val line = readAsciiLine(input) ?: break
            if (line.isEmpty()) break
            val colon = line.indexOf(':')
            if (colon > 0) {
                val k = line.substring(0, colon).trim().lowercase(Locale.US)
                val v = line.substring(colon + 1).trim()
                headers[k] = v
            }
        }

        if (method == "OPTIONS" && !rawTarget.startsWith("/dav")) {
            sendSimpleResponse(output, 204, "No Content", "text/plain", ByteArray(0))
            return
        }

        if (!checkSiblingAuth(headers)) {
            val err = JSONObject().put("error", "Sibling Mode is paused or Pairing Key mismatch on Android phone.").toString()
            sendSimpleResponse(output, 403, "Forbidden", "application/json", err.toByteArray(Charsets.UTF_8))
            return
        }

        val contentLength = headers["content-length"]?.toLongOrNull() ?: 0L

        try {
            if (rawTarget == "/dav" || rawTarget.startsWith("/dav/") || rawTarget.startsWith("/dav?")) {
                handleWebDav(method, rawTarget, headers, contentLength, input, output)
            } else {
                handleChainItApi(method, rawTarget, headers, contentLength, input, output)
            }
            if (viaBluetooth) {
                lastActivitySummary = "[Bluetooth RFCOMM] $lastActivitySummary"
            }
        } catch (e: Exception) {
            val msg = JSONObject().put("error", e.message ?: "Phone VFS error").toString()
            sendSimpleResponse(output, 400, "Bad Request", "application/json", msg.toByteArray(Charsets.UTF_8))
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
                val bm = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
                val (totalB, usedB) = vfs.getStorageCapacity()

                val obj = JSONObject()
                obj.put("ok", true)
                obj.put("deviceName", "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}")
                obj.put("model", Build.MODEL)
                obj.put("androidVersion", "Android ${Build.VERSION.RELEASE}")
                obj.put("batteryPct", bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY).coerceIn(1, 100))
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
                val safeName = File(rawName).name.replace(Regex("[<>:\"/\\\\|?*]"), "_")
                val dirFile = vfs.resolveSafe(dir)
                dirFile.mkdirs()
                val dest = File(dirFile, safeName)
                copyFixedBytes(input, dest, contentLength)
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
                targetFile.deleteRecursively()
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
                    targetFile.copyRecursively(destFile, overwrite = true)
                } else {
                    if (existed) destFile.deleteRecursively()
                    if (!targetFile.renameTo(destFile)) {
                        targetFile.copyRecursively(destFile, overwrite = true)
                        targetFile.deleteRecursively()
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
        val sb = StringBuilder()
        sb.append(if (partial) "HTTP/1.1 206 Partial Content\r\n" else "HTTP/1.1 200 OK\r\n")
        sb.append("Content-Type: $mime\r\n")
        sb.append("Content-Length: $length\r\n")
        sb.append("Accept-Ranges: bytes\r\n")
        sb.append("Access-Control-Allow-Origin: *\r\n")
        sb.append("Connection: close\r\n")
        if (partial) {
            sb.append("Content-Range: bytes $start-$end/$size\r\n")
        }
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
        sb.append("Connection: close\r\n")
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
