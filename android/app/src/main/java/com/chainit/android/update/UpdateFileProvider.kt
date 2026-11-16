package com.chainit.android.update

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.FileNotFoundException

/**
 * Minimal dependency-free replacement for androidx FileProvider.
 *
 * Android 7+ refuses `file://` URIs in install intents, so the downloaded update APK is handed to the
 * system package installer through this provider as `content://com.chainit.android.updatefile/<name>`.
 * Because the update keeps the same package name, the same signing key and a higher versionCode, the
 * installer performs a true **in-place replacement** — the old build is upgraded, never duplicated.
 */
class UpdateFileProvider : ContentProvider() {

    companion object {
        const val AUTHORITY = "com.chainit.android.updatefile"
    }

    override fun onCreate(): Boolean = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val ctx = context ?: throw FileNotFoundException("no context")
        val name = uri.lastPathSegment?.replace("..", "") ?: throw FileNotFoundException("no segment")
        val file = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, name)
        if (!file.exists()) throw FileNotFoundException(file.absolutePath)
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String = "application/vnd.android.package-archive"

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0
}
