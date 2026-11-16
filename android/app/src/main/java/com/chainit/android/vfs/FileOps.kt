package com.chainit.android.vfs

import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * File helpers that deliberately avoid the handful of kotlin-stdlib functions whose published
 * bytecode still uses `invokedynamic` (see android/tools/verify_dex_safety.py).
 *
 * `File.copyRecursively` and `compareBy { }` are exactly two of them: the dx dexer can only emulate
 * their lambdas through a runtime `LambdaMetafactory` call, so ChainIt implements the same behaviour
 * with plain loops and an explicit Comparator. Same result, zero reflection, zero crash surface.
 */
object FileOps {

    /** Depth-first recursive copy. Overwrites existing files, creates directories as needed. */
    fun copyRecursive(src: File, dst: File): Boolean {
        if (!src.exists()) return false
        if (src.isDirectory) {
            if (!dst.exists() && !dst.mkdirs()) return false
            val children = src.listFiles() ?: return true
            var ok = true
            for (child in children) {
                if (!copyRecursive(child, File(dst, child.name))) ok = false
            }
            return ok
        }
        return try {
            dst.parentFile?.mkdirs()
            FileInputStream(src).use { input ->
                FileOutputStream(dst).use { output ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val r = input.read(buf)
                        if (r <= 0) break
                        output.write(buf, 0, r)
                    }
                    output.flush()
                }
            }
            dst.setLastModified(src.lastModified())
            true
        } catch (_: Throwable) {
            false
        }
    }

    /** Recursive delete (kept local so behaviour never depends on stdlib internals). */
    fun deleteRecursive(target: File): Boolean {
        if (!target.exists()) return true
        if (target.isDirectory) {
            val children = target.listFiles()
            if (children != null) {
                for (child in children) deleteRecursive(child)
            }
        }
        return try {
            target.delete()
        } catch (_: Throwable) {
            false
        }
    }

    /** Folders first, then case-insensitive A→Z — the order Windows Explorer shows in Z:\. */
    val EXPLORER_ORDER: Comparator<File> = object : Comparator<File> {
        override fun compare(a: File, b: File): Int {
            val aDir = a.isDirectory
            val bDir = b.isDirectory
            if (aDir != bDir) return if (aDir) -1 else 1
            val cmp = a.name.lowercase().compareTo(b.name.lowercase())
            return if (cmp != 0) cmp else a.name.compareTo(b.name)
        }
    }

    /** Total size of a file or directory tree. */
    fun sizeOf(target: File): Long {
        if (!target.exists()) return 0L
        if (target.isFile) return target.length()
        var sum = 0L
        val children = target.listFiles() ?: return 0L
        for (child in children) sum += sizeOf(child)
        return sum
    }

    /** Strips characters Windows cannot represent in a Z:\ path. */
    fun sanitizeName(raw: String, replacement: String = "_"): String {
        val sb = StringBuilder(raw.length)
        for (ch in raw) {
            sb.append(
                when (ch) {
                    '<', '>', ':', '"', '/', '\\', '|', '?', '*' -> replacement
                    else -> ch
                }
            )
        }
        return sb.toString().trim().ifEmpty { "item" }
    }

    /** Removes (rather than replaces) illegal characters, so an empty result can be rejected. */
    fun stripIllegal(raw: String): String {
        val sb = StringBuilder(raw.length)
        for (ch in raw) {
            when (ch) {
                '<', '>', ':', '"', '/', '\\', '|', '?', '*' -> {}
                else -> sb.append(ch)
            }
        }
        return sb.toString().trim()
    }
}
