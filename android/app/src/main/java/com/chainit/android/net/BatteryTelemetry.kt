package com.chainit.android.net

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager

/**
 * The phone's own battery facts, reported to the PC so it can show whether a link will survive.
 *
 * Everything here comes from the platform: the sticky `ACTION_BATTERY_CHANGED` broadcast (status,
 * plug, temperature) with `BatteryManager.BATTERY_PROPERTY_CAPACITY` as the percentage fallback.
 * Nothing is estimated or simulated - when the platform does not report a value, [Reading] carries
 * `0` / `unknown` and the PC says "no report" instead of inventing a plausible number.
 *
 * Why the PC cares: a long Bluetooth session is exactly when a dying or hot phone stops answering,
 * and "86%" alone does not distinguish "on charge, will run all night" from "draining, 40 min left".
 */
object BatteryTelemetry {

    /** One snapshot of the battery. [tempC] is 0.0 when the broadcast carries no temperature. */
    data class Reading(
        val pct: Int,
        val charging: Boolean,
        /** "ac" | "usb" | "wireless" | "none" | "unknown" */
        val source: String,
        val tempC: Double
    )

    /**
     * Reads the sticky battery broadcast. `registerReceiver(null, filter)` returns the last broadcast
     * without registering anything, which is what makes this safe to call from a request thread.
     * `ACTION_BATTERY_CHANGED` is a protected system broadcast, so no export flag is needed.
     */
    fun read(ctx: Context): Reading {
        var pct = 0
        var charging = false
        var source = "unknown"
        var tempC = 0.0
        try {
            val sticky = ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            if (sticky != null) {
                val level = sticky.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = sticky.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                if (level >= 0 && scale > 0) pct = ((level * 100) / scale).coerceIn(0, 100)

                val status = sticky.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
                charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                    status == BatteryManager.BATTERY_STATUS_FULL

                when (sticky.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1)) {
                    BatteryManager.BATTERY_PLUGGED_AC -> source = "ac"
                    BatteryManager.BATTERY_PLUGGED_USB -> source = "usb"
                    BatteryManager.BATTERY_PLUGGED_WIRELESS -> source = "wireless"
                    0 -> source = "none"
                    else -> source = "unknown"
                }
                if (!charging && source == "unknown") source = "none"

                // EXTRA_TEMPERATURE is tenths of a degree Celsius; 0 means "not reported".
                val tenths = sticky.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
                if (tenths != Int.MIN_VALUE && tenths > 0) tempC = tenths / 10.0
            }
        } catch (_: Exception) {
            // A device that refuses the broadcast still reports its percentage below.
        }

        if (pct <= 0) {
            try {
                val bm = ctx.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
                if (bm != null) {
                    pct = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY).coerceIn(0, 100)
                }
            } catch (_: Exception) {
            }
        }
        if (source == "unknown") source = "none"
        return Reading(pct.coerceIn(0, 100), charging, source, tempC)
    }

    /** Percentage only, for callers that already report a single number. */
    fun percent(ctx: Context): Int = read(ctx).pct

    /** "Charging from mains (AC)" / "On battery" - one honest line for the phone's own UI. */
    fun describe(r: Reading): String {
        if (r.pct <= 0) return "battery not reported"
        val base = "${r.pct}%"
        if (!r.charging) return "$base on battery"
        return when (r.source) {
            "ac" -> "$base charging from mains (AC)"
            "usb" -> "$base charging over USB"
            "wireless" -> "$base charging on a wireless pad"
            else -> "$base charging"
        }
    }
}
