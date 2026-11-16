package com.chainit.android.vfs

import android.os.Environment
import android.os.StatFs
import android.webkit.MimeTypeMap
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class AndroidVfsEntry(
    val name: String,
    val path: String,
    val isDirectory: Boolean,
    val sizeBytes: Long,
    val modifiedUnixMs: Long,
    val mimeType: String,
    val ext: String
) {
    fun toJson(): JSONObject {
        val obj = JSONObject()
        obj.put("name", name)
        obj.put("path", path)
        obj.put("kind", if (isDirectory) "directory" else "file")
        obj.put("sizeBytes", sizeBytes)
        obj.put("modifiedMs", modifiedUnixMs)
        obj.put("mimeType", mimeType)
        obj.put("ext", ext)
        return obj
    }
}

class AndroidStorageVfsProvider(
    val storageRoot: File = Environment.getExternalStorageDirectory()
) {
    private val canonicalRoot: String
        get() = try {
            storageRoot.canonicalPath
        } catch (_: Exception) {
            storageRoot.absolutePath
        }

    fun resolveSafe(virtualPath: String?): File {
        val raw = (virtualPath ?: "/").replace('\\', '/').trim()
        val rel = raw.trimStart('/')
        val candidate = if (rel.isEmpty()) storageRoot else File(storageRoot, rel)
        val canonical = candidate.canonicalPath
        val root = canonicalRoot
        require(canonical == root || canonical.startsWith("$root/")) {
            "Blocked path outside storage root: $virtualPath"
        }
        return candidate
    }

    fun normalizeVirtualPath(file: File): String {
        val root = canonicalRoot
        val can = file.canonicalPath
        if (can == root) return "/"
        return "/" + can.removePrefix(root).trimStart('/').replace('\\', '/')
    }

    fun listDirectory(virtualPath: String?): Pair<String, List<AndroidVfsEntry>> {
        val dir = resolveSafe(virtualPath)
        require(dir.exists() && dir.isDirectory) {
            "Directory does not exist: ${virtualPath ?: "/"}"
        }
        val children = (dir.listFiles() ?: emptyArray())
            .filter { !it.name.startsWith(".") }
            .sortedWith(compareBy<File>({ !it.isDirectory }, { it.name.lowercase() }))
            .map { toEntry(it) }
        return Pair(normalizeVirtualPath(dir), children)
    }

    fun toEntry(file: File): AndroidVfsEntry {
        val isDir = file.isDirectory
        val ext = if (isDir || !file.name.contains('.')) "" else "." + file.extension.lowercase()
        val mime = if (isDir) {
            "inode/directory"
        } else {
            guessMime(file.name)
        }
        return AndroidVfsEntry(
            name = file.name,
            path = normalizeVirtualPath(file),
            isDirectory = isDir,
            sizeBytes = if (file.isFile) file.length() else 0L,
            modifiedUnixMs = file.lastModified(),
            mimeType = mime,
            ext = ext
        )
    }

    fun guessMime(filename: String): String {
        val ext = filename.substringAfterLast('.', "").lowercase()
        if (ext.isEmpty()) return "application/octet-stream"
        val sys = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext)
        if (!sys.isNullOrBlank()) return sys
        return when (ext) {
            "md" -> "text/markdown"
            "json" -> "application/json"
            "txt", "log", "ini", "kt", "rs", "ts", "js", "ps1" -> "text/plain"
            "csv" -> "text/csv"
            "svg" -> "image/svg+xml"
            else -> "application/octet-stream"
        }
    }

    fun isEditableText(filename: String): Boolean {
        val ext = filename.substringAfterLast('.', "").lowercase()
        return ext in setOf("md", "txt", "json", "csv", "xml", "svg", "html", "js", "ts", "kt", "rs", "log", "ini", "yml", "yaml")
    }

    fun makeDirectory(parentDir: String, folderName: String): AndroidVfsEntry {
        val clean = folderName.replace(Regex("[<>:\"/\\\\|?*]"), "").trim()
        require(clean.isNotEmpty()) { "Invalid folder name" }
        val parent = resolveSafe(parentDir)
        val target = File(parent, clean)
        require(!target.exists()) { "\"$clean\" already exists" }
        require(target.mkdirs()) { "Unable to create directory" }
        return toEntry(target)
    }

    fun renameItem(itemPath: String, newName: String): AndroidVfsEntry {
        val clean = newName.replace(Regex("[<>:\"/\\\\|?*]"), "").trim()
        require(clean.isNotEmpty()) { "Invalid file name" }
        val src = resolveSafe(itemPath)
        require(src.exists()) { "Item not found" }
        val dst = File(src.parentFile, clean)
        if (dst.canonicalPath != src.canonicalPath && dst.exists()) {
            throw IllegalArgumentException("\"$clean\" already exists")
        }
        require(src.renameTo(dst)) { "Rename failed" }
        return toEntry(dst)
    }

    fun deleteItems(paths: List<String>): Int {
        var count = 0
        for (p in paths) {
            val target = resolveSafe(p)
            if (target.canonicalPath == canonicalRoot) continue
            if (target.exists()) {
                if (target.deleteRecursively()) count++
            }
        }
        return count
    }

    fun copyOrMove(op: String, fromPaths: List<String>, toDir: String): Pair<Long, List<String>> {
        val destDir = resolveSafe(toDir)
        destDir.mkdirs()
        var totalBytes = 0L
        val names = mutableListOf<String>()

        for (p in fromPaths) {
            val src = resolveSafe(p)
            if (!src.exists()) continue
            var dst = File(destDir, src.name)
            if (op == "copy" && src.parentFile?.canonicalPath == destDir.canonicalPath) {
                val base = src.nameWithoutExtension
                val ext = if (src.extension.isEmpty()) "" else ".${src.extension}"
                var idx = 1
                while (dst.exists()) {
                    val suffix = if (idx == 1) " - Copy" else " - Copy ($idx)"
                    dst = File(destDir, "$base$suffix$ext")
                    idx++
                }
            }
            val bytes = computeSize(src)
            if (op == "move") {
                if (src.canonicalPath != dst.canonicalPath) {
                    if (dst.exists()) dst.deleteRecursively()
                    if (!src.renameTo(dst)) {
                        src.copyRecursively(dst, overwrite = true)
                        src.deleteRecursively()
                    }
                }
            } else {
                src.copyRecursively(dst, overwrite = true)
            }
            totalBytes += bytes
            names.add(dst.name)
        }
        return Pair(totalBytes, names)
    }

    private fun computeSize(f: File): Long {
        if (!f.exists()) return 0L
        if (f.isFile) return f.length()
        var sum = 0L
        f.listFiles()?.forEach { sum += computeSize(it) }
        return sum
    }

    fun getStorageCapacity(): Pair<Long, Long> {
        return try {
            val stat = StatFs(storageRoot.absolutePath)
            val total = stat.totalBytes
            val used = total - stat.availableBytes
            Pair(total, used)
        } catch (_: Exception) {
            Pair(128L * 1024 * 1024 * 1024, 48L * 1024 * 1024 * 1024)
        }
    }

    fun listAsJson(virtualPath: String?): JSONObject {
        val (cur, items) = listDirectory(virtualPath)
        val arr = JSONArray()
        items.forEach { arr.put(it.toJson()) }
        val res = JSONObject()
        res.put("currentPath", cur)
        res.put("items", arr)
        return res
    }
}
