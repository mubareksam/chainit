package com.chainit.android.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.chainit.android.service.SiblingDaemonService

/**
 * Restarts the Sibling daemon after a reboot and — importantly — right after an in-place app update
 * (ACTION_MY_PACKAGE_REPLACED), so a fresh build is serving Z:\ again without the user opening the app.
 *
 * Every path is guarded: on Android 12+ a background foreground-service start can be refused, and that
 * must never surface as an app crash.
 */
class BootAndBleWakeReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        val interesting = action == Intent.ACTION_BOOT_COMPLETED ||
            action == Intent.ACTION_MY_PACKAGE_REPLACED ||
            action == "android.intent.action.REPLACE"

        if (!interesting) return

        val prefs = runCatching { context.getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE) }.getOrNull()
        val enabled = prefs?.getBoolean("sibling_enabled", true) ?: true
        if (!enabled) return

        SiblingDaemonService.start(context)
    }
}
