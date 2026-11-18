package com.chainit.android

import android.app.Application
import android.content.Context
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * App-wide crash guard.
 *
 * A "ChainIt Sibling keeps stopping" dialog with no explanation is useless in the field, so every
 * uncaught exception is captured to internal storage *before* the process dies. The next launch
 * shows the exact stack trace on a recovery screen, and the normal UI keeps working.
 */
class ChainItApp : Application() {

    companion object {
        private const val PREFS = "chainit_prefs"
        private const val KEY_LAST_CRASH = "last_crash_report"
        private const val KEY_CRASH_COUNT = "crash_count"
        const val CRASH_FILE = "chainit-crash.log"

        fun crashFile(ctx: Context): File = File(ctx.filesDir, CRASH_FILE)

        fun readLastCrash(ctx: Context): String? {
            val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            return prefs.getString(KEY_LAST_CRASH, null)
        }

        fun clearLastCrash(ctx: Context) {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_LAST_CRASH).apply()
            runCatching { crashFile(ctx).delete() }
        }

        fun crashCount(ctx: Context): Int =
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(KEY_CRASH_COUNT, 0)
    }

    override fun onCreate() {
        // Never let the guard itself take the app down.
        try {
            super.onCreate()
        } catch (t: Throwable) {
            record(t, "Application.onCreate")
            throw t
        }

        try {
            val previous = Thread.getDefaultUncaughtExceptionHandler()
            Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
                record(throwable, thread.name)
                previous?.uncaughtException(thread, throwable)
            }
        } catch (_: Throwable) {
        }
    }

    private fun record(t: Throwable, where: String) {
        try {
            val sw = StringWriter()
            sw.write("ChainIt Sibling crash on thread '$where'\n")
            sw.write("android=${android.os.Build.VERSION.SDK_INT} device=${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}\n")
            sw.write("version=${BuildConfig.VERSION_LABEL}\n\n")
            t.printStackTrace(PrintWriter(sw))
            val report = sw.toString().take(12000)

            getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_LAST_CRASH, report)
                .putInt(KEY_CRASH_COUNT, crashCount(this) + 1)
                .apply()

            crashFile(this).writeText(report)
        } catch (_: Throwable) {
        }
    }
}

/** Small compile-time constants injected by android/build-apk.sh (kept in sync with the manifest). */
object BuildConfig {
    const val VERSION_LABEL = "1.9.0"
    const val VERSION_CODE = 7
}
