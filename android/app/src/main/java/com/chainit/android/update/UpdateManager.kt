package com.chainit.android.update

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import com.chainit.android.BuildConfig
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * In-place self-updater.
 *
 * Downloads the newest signed `ChainIt-Android.apk` and hands it to the system installer. Because the
 * package name (`com.chainit.android`), the signing key and the data directory never change, Android
 * **replaces** the installed build instead of adding a second app icon — and every setting (Sibling
 * key, last PC IP, granted permissions) survives the update.
 *
 * The release key is committed at `android/keystore/chainit-release.jks` and `android/build-apk.sh`
 * always signs with it, so an update can never be rejected as "signed by a different certificate".
 */
object UpdateManager {

    const val DEFAULT_UPDATE_URL =
        "https://github.com/mubareksam/chainit/raw/arena/37fd3890-chainit/releases/ChainIt-Android.apk"

    /** Tried in order so the updater keeps working before and after the release branch merges. */
    private val FALLBACK_UPDATE_URLS = listOf(
        "https://github.com/mubareksam/chainit/raw/main/releases/ChainIt-Android.apk",
        DEFAULT_UPDATE_URL
    )

    private const val PREFS = "chainit_prefs"
    private const val KEY_URL = "update_url"
    const val APK_NAME = "ChainIt-Android-latest.apk"

    sealed class State {
        object Idle : State()
        data class Checking(val note: String) : State()
        data class Downloading(val percent: Int, val bytes: Long) : State()
        data class Ready(val versionName: String, val versionCode: Int, val file: File) : State()
        data class UpToDate(val versionName: String) : State()
        data class Failed(val message: String) : State()
    }

    fun updateUrl(ctx: Context): String =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_URL, DEFAULT_UPDATE_URL)
            ?.takeIf { it.isNotBlank() } ?: DEFAULT_UPDATE_URL

    fun candidateUrls(ctx: Context): List<String> {
        val configured = updateUrl(ctx)
        val urls = mutableListOf(configured)
        for (candidate in FALLBACK_UPDATE_URLS) if (candidate != configured) urls.add(candidate)
        return urls
    }

    fun setUpdateUrl(ctx: Context, url: String) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY_URL, url.trim()).apply()
    }

    fun currentVersion(ctx: Context): Pair<String, Int> {
        return try {
            val info = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            val code = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode.toInt() else @Suppress("DEPRECATION") info.versionCode
            (info.versionName ?: BuildConfig.VERSION_LABEL) to code
        } catch (_: Exception) {
            BuildConfig.VERSION_LABEL to BuildConfig.VERSION_CODE
        }
    }

    fun targetFile(ctx: Context): File =
        File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, APK_NAME)

    /** Blocking work — call from a background thread. Reports progress on the caller's thread. */
    fun checkAndDownload(ctx: Context, onState: (State) -> Unit) {
        onState(State.Checking("Contacting release server…"))
        val out = targetFile(ctx)
        runCatching { if (out.exists()) out.delete() }

        var downloadError: String? = "No release URL is configured."
        for (url in candidateUrls(ctx)) {
            val error = downloadOnce(url, out, onState)
            if (error == null) {
                downloadError = null
                break
            }
            downloadError = error
        }
        if (downloadError != null) {
            runCatching { if (out.exists()) out.delete() }
            onState(State.Failed(downloadError))
            return
        }

        // Read the downloaded APK's own versionCode so we never downgrade or reinstall the same build.
        val archive = archiveVersion(ctx, out)
        if (archive == null) {
            onState(State.Failed("Downloaded file is not a valid Android package."))
            return
        }
        val (_, installedCode) = currentVersion(ctx)
        if (archive.second <= installedCode) {
            runCatching { out.delete() }
            onState(State.UpToDate(archive.first))
            return
        }
        onState(State.Ready(archive.first, archive.second, out))
    }

    /** @return null on success, otherwise a human-readable reason for the failure. */
    private fun downloadOnce(url: String, out: File, onState: (State) -> Unit): String? {
        var conn: HttpURLConnection? = null
        try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 12000
                readTimeout = 120000
                instanceFollowRedirects = true
                setRequestProperty("Accept", "*/*")
                setRequestProperty("User-Agent", "ChainIt-Android/${BuildConfig.VERSION_LABEL}")
            }
            val code = conn.responseCode
            if (code !in 200..299) {
                return "Release server answered HTTP $code."
            }
            val total = conn.contentLengthLong.let { if (it > 0) it else conn.getHeaderField("Content-Length")?.toLongOrNull() ?: -1L }
            var done = 0L
            conn.inputStream.use { input ->
                out.outputStream().use { o ->
                    val buf = ByteArray(64 * 1024)
                    var lastPct = -1
                    while (true) {
                        val r = input.read(buf)
                        if (r <= 0) break
                        o.write(buf, 0, r)
                        done += r
                        if (total > 0) {
                            val pct = ((done * 100) / total).toInt()
                            if (pct != lastPct) {
                                lastPct = pct
                                onState(State.Downloading(pct, done))
                            }
                        } else {
                            onState(State.Downloading(-1, done))
                        }
                    }
                }
            }
        } catch (e: Exception) {
            return e.javaClass.simpleName + ": " + (e.message ?: "network error")
        } finally {
            runCatching { conn?.disconnect() }
        }
        return null
    }

    private fun archiveVersion(ctx: Context, apk: File): Pair<String, Int>? {
        return try {
            val info = ctx.packageManager.getPackageArchiveInfo(apk.absolutePath, 0) ?: return null
            val code = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode.toInt() else @Suppress("DEPRECATION") info.versionCode
            (info.versionName ?: "?") to code
        } catch (_: Exception) {
            null
        }
    }

    /** Launches the system installer, which upgrades the existing app in place. */
    fun install(ctx: Context, apk: File): String? {
        if (Build.VERSION.SDK_INT >= 26 && !ctx.packageManager.canRequestPackageInstalls()) {
            try {
                ctx.startActivity(
                    Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
                return "Allow \"Install unknown apps\" for ChainIt once, then tap Update again."
            } catch (_: Exception) {
            }
        }
        return try {
            val uri = Uri.parse("content://${UpdateFileProvider.AUTHORITY}/${apk.name}")
            val intent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "application/vnd.android.package-archive")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            ctx.startActivity(intent)
            null
        } catch (e: Exception) {
            // Last resort: some OEM builds only accept the direct file path.
            try {
                val intent = Intent(Intent.ACTION_INSTALL_PACKAGE).apply {
                    data = Uri.fromFile(apk)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
                }
                ctx.startActivity(intent)
                null
            } catch (_: Exception) {
                "Could not open the installer: ${e.javaClass.simpleName}"
            }
        }
    }

    fun hasInstallPermission(ctx: Context): Boolean =
        Build.VERSION.SDK_INT < 26 || ctx.packageManager.canRequestPackageInstalls()
}
