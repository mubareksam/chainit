package com.chainit.android.net

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException

/**
 * Hands a file that ChainIt saved to the phone to another app as `content://`, without androidx.
 *
 * Android 7+ refuses `file://` URIs in intents, so "Open with…" on a file just downloaded from the PC
 * needs a provider. [com.chainit.android.update.UpdateFileProvider] already does this for update
 * APKs; this one is the general-purpose sibling used by the native PC file browser, and it only ever
 * serves paths inside ChainIt's own directories (plus the ChainIt folder in Downloads), so it cannot
 * be used to read arbitrary storage.
 */
class ChainItFileProvider : ContentProvider() {

    companion object {
        const val AUTHORITY = "com.chainit.android.files"

        /** `content://com.chainit.android.files/<url-encoded absolute path>` */
        fun uriFor(file: File): Uri =
            Uri.parse("content://$AUTHORITY/" + Uri.encode(file.absolutePath))

        fun mimeFor(name: String): String {
            val dot = name.lastIndexOf('.')
            val ext = if (dot >= 0) name.substring(dot).lowercase() else ""
            return when (ext) {
                ".txt", ".log", ".md", ".csv" -> "text/plain"
                ".json" -> "application/json"
                ".xml" -> "text/xml"
                ".html", ".htm" -> "text/html"
                ".pdf" -> "application/pdf"
                ".doc" -> "application/msword"
                ".docx" -> "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
                ".xls" -> "application/vnd.ms-excel"
                ".xlsx" -> "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
                ".ppt" -> "application/vnd.ms-powerpoint"
                ".pptx" -> "application/vnd.openxmlformats-officedocument.presentationml.presentation"
                ".jpg", ".jpeg" -> "image/jpeg"
                ".png" -> "image/png"
                ".gif" -> "image/gif"
                ".webp" -> "image/webp"
                ".bmp" -> "image/bmp"
                ".svg" -> "image/svg+xml"
                ".mp3" -> "audio/mpeg"
                ".wav" -> "audio/wav"
                ".flac" -> "audio/flac"
                ".m4a" -> "audio/mp4"
                ".aac" -> "audio/aac"
                ".ogg" -> "audio/ogg"
                ".mp4" -> "video/mp4"
                ".mkv" -> "video/x-matroska"
                ".avi" -> "video/x-msvideo"
                ".mov" -> "video/quicktime"
                ".webm" -> "video/webm"
                ".zip" -> "application/zip"
                ".rar" -> "application/vnd.rar"
                ".7z" -> "application/x-7z-compressed"
                ".apk" -> "application/vnd.android.package-archive"
                else -> "application/octet-stream"
            }
        }
    }

    override fun onCreate(): Boolean = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val ctx = context ?: throw FileNotFoundException("no context")
        val file = resolve(ctx, uri) ?: throw FileNotFoundException("path outside ChainIt's folders")
        if (!file.exists()) throw FileNotFoundException(file.absolutePath)
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String {
        val path = decode(uri)
        val name = if (path == null) "" else File(path).name
        return mimeFor(name)
    }

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?
    ): Cursor? {
        val ctx = context ?: return null
        val file = resolve(ctx, uri) ?: return null
        if (!file.exists()) return null
        val cursor = MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE))
        cursor.addRow(arrayOf<Any>(file.name, java.lang.Long.valueOf(file.length())))
        return cursor
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    private fun decode(uri: Uri): String? {
        val segment = uri.lastPathSegment ?: return null
        return Uri.decode(segment)
    }

    /** Resolves and then confines the path to a directory ChainIt itself wrote to. */
    private fun resolve(ctx: Context, uri: Uri): File? {
        val path = decode(uri) ?: return null
        val candidate = File(path)
        val wanted = runCatching { candidate.canonicalPath }.getOrDefault(candidate.absolutePath)
        for (root in allowedRoots(ctx)) {
            val rootPath = runCatching { root.canonicalPath }.getOrDefault(root.absolutePath)
            if (rootPath.isNotEmpty() && wanted.startsWith(rootPath + File.separator)) return candidate
            if (wanted == rootPath) return candidate
        }
        return null
    }

    private fun allowedRoots(ctx: Context): List<File> {
        val roots = ArrayList<File>()
        roots.add(ctx.filesDir)
        roots.add(ctx.cacheDir)
        ctx.getExternalFilesDir(null)?.let { roots.add(it) }
        ctx.externalCacheDir?.let { roots.add(it) }
        runCatching {
            roots.add(File(android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), "ChainIt"))
        }
        return roots
    }
}
