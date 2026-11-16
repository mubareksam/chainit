package com.chainit.android.ui

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.chainit.android.net.BluetoothRfcommTransport
import com.chainit.android.service.SiblingDaemonService
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

class MainActivity : Activity() {

    companion object {
        private const val REQ_PICK_FILE_AUTO = 2001
        private const val REQ_PICK_FILE_BT_ONLY = 2003
        private const val REQ_PERMS = 2002
    }

    private val uiHandler = Handler(Looper.getMainLooper())
    private lateinit var statusBadgeText: TextView
    private lateinit var storageBtn: Button
    private lateinit var batteryBtn: Button
    private lateinit var btPermBtn: Button
    private lateinit var endpointsText: TextView
    private lateinit var pcStatusText: TextView
    private lateinit var pcIpInput: EditText
    private lateinit var keyInput: EditText

    private val refreshTick = object : Runnable {
        override fun run() {
            refreshStatusUi()
            uiHandler.postDelayed(this, 2000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Color.parseColor("#090D16")
        window.navigationBarColor = Color.parseColor("#090D16")

        requestInitialRuntimePermissions()

        val prefs = getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        val siblingEnabled = prefs.getBoolean("sibling_enabled", true)
        if (siblingEnabled && !SiblingDaemonService.isRunning) {
            startForegroundService(Intent(this, SiblingDaemonService::class.java))
        }

        buildScreenLayout()
    }

    override fun onResume() {
        super.onResume()
        refreshStatusUi()
        uiHandler.post(refreshTick)
    }

    override fun onPause() {
        uiHandler.removeCallbacks(refreshTick)
        super.onPause()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun cardBg(borderHex: String = "#1E293B", fillHex: String = "#0F172A"): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(16).toFloat()
            setColor(Color.parseColor(fillHex))
            setStroke(dp(1), Color.parseColor(borderHex))
        }

    private fun buttonBg(hex: String): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(12).toFloat()
            setColor(Color.parseColor(hex))
        }

    private fun buildScreenLayout() {
        val prefs = getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)

        val scroll = ScrollView(this).apply {
            setBackgroundColor(Color.parseColor("#090D16"))
            isFillViewport = true
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(22), dp(18), dp(28))
        }

        val title = TextView(this).apply {
            text = "ChainIt Sibling"
            textSize = 24f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.WHITE)
        }
        val subtitle = TextView(this).apply {
            text = "USB • Wi-Fi • Hotspot • Bluetooth — Seamless Windows Z:\\ Drive"
            textSize = 13f
            setTextColor(Color.parseColor("#94A3B8"))
            setPadding(0, dp(4), 0, dp(14))
        }
        root.addView(title)
        root.addView(subtitle)

        // Status Card
        val statusCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg("#10B981", "#06281E")
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        val rowSwitch = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        statusBadgeText = TextView(this).apply {
            text = "Sibling Mode: ACTIVE"
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.parseColor("#6EE7B7"))
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        val masterSwitch = Switch(this).apply {
            isChecked = prefs.getBoolean("sibling_enabled", true)
            setOnCheckedChangeListener { _, checked ->
                prefs.edit().putBoolean("sibling_enabled", checked).apply()
                if (checked) {
                    startForegroundService(Intent(this@MainActivity, SiblingDaemonService::class.java))
                } else {
                    stopService(Intent(this@MainActivity, SiblingDaemonService::class.java))
                }
                refreshStatusUi()
            }
        }
        rowSwitch.addView(statusBadgeText)
        rowSwitch.addView(masterSwitch)

        val statusDesc = TextView(this).apply {
            text = "Your Windows PC auto-connects over USB, Wi-Fi, Hotspot, or Bluetooth and mounts Z:\\ even while your phone screen is locked."
            textSize = 12f
            setTextColor(Color.parseColor("#D1FAE5"))
            setPadding(0, dp(6), 0, 0)
        }
        statusCard.addView(rowSwitch)
        statusCard.addView(statusDesc)
        root.addView(statusCard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(14)
        })

        // Setup Checklist Card
        val permCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg()
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        permCard.addView(TextView(this).apply {
            text = "One-Time Setup Checklist"
            textSize = 15f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, dp(10))
        })

        storageBtn = Button(this).apply {
            isAllCaps = false
            textSize = 13f
            setTextColor(Color.parseColor("#090D16"))
            background = buttonBg("#10B981")
            setOnClickListener { openStoragePermissionSettings() }
        }
        permCard.addView(storageBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(8)
        })

        batteryBtn = Button(this).apply {
            isAllCaps = false
            textSize = 13f
            setTextColor(Color.WHITE)
            background = buttonBg("#1E293B")
            setOnClickListener { requestBatteryExemption() }
        }
        permCard.addView(batteryBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(8)
        })

        btPermBtn = Button(this).apply {
            isAllCaps = false
            textSize = 13f
            setTextColor(Color.WHITE)
            background = buttonBg("#1E293B")
            setOnClickListener { requestBluetoothPermissionsExplicitly() }
        }
        permCard.addView(btPermBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        root.addView(permCard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(14)
        })

        // Active Links Card (USB / Wi-Fi / Hotspot / Bluetooth)
        val netCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg()
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        netCard.addView(TextView(this).apply {
            text = "Active Transports (IP & Native Bluetooth)"
            textSize = 15f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.WHITE)
        })
        endpointsText = TextView(this).apply {
            text = "Scanning interfaces..."
            textSize = 12.5f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.parseColor("#38BDF8"))
            setPadding(0, dp(8), 0, dp(8))
        }
        pcStatusText = TextView(this).apply {
            text = "Waiting for ChainIt-Windows.exe..."
            textSize = 12.5f
            setTextColor(Color.parseColor("#CBD5E1"))
        }
        netCard.addView(endpointsText)
        netCard.addView(pcStatusText)
        root.addView(netCard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(14)
        })

        // Instant Share Card
        val actionCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg()
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        actionCard.addView(TextView(this).apply {
            text = "Instant Share to Windows PC"
            textSize = 15f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, dp(8))
        })

        pcIpInput = EditText(this).apply {
            hint = "Optional PC IP (blank = Auto Discovery / Bluetooth)"
            setText(prefs.getString("last_pc_ip", ""))
            setHintTextColor(Color.parseColor("#64748B"))
            setTextColor(Color.WHITE)
            textSize = 13f
            inputType = InputType.TYPE_CLASS_TEXT
            background = cardBg("#334155", "#020617")
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        actionCard.addView(pcIpInput, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(10)
        })

        val sendAutoBtn = Button(this).apply {
            text = "Send File to PC (Auto: Fast Wi-Fi / Hotspot / Bluetooth)"
            isAllCaps = false
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.parseColor("#020617"))
            background = buttonBg("#06B6D4")
            setOnClickListener { pickFile(REQ_PICK_FILE_AUTO) }
        }
        actionCard.addView(sendAutoBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(8)
        })

        val sendBtBtn = Button(this).apply {
            text = "Send File via Bluetooth Only (Works with Wi-Fi OFF)"
            isAllCaps = false
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.WHITE)
            background = buttonBg("#2563EB")
            setOnClickListener { pickFile(REQ_PICK_FILE_BT_ONLY) }
        }
        actionCard.addView(sendBtBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(8)
        })

        val browsePcBtn = Button(this).apply {
            text = "Browse Windows PC Files (C:\\) in Mobile View"
            isAllCaps = false
            textSize = 13f
            setTextColor(Color.WHITE)
            background = buttonBg("#1E293B")
            setOnClickListener {
                val ip = resolveTargetPcIp()
                if (ip.isBlank()) {
                    Toast.makeText(this@MainActivity, "Connect Wi-Fi/Hotspot or enter PC IP above to open Web Explorer", Toast.LENGTH_LONG).show()
                } else {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("http://$ip:4173/mobile")))
                }
            }
        }
        actionCard.addView(browsePcBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        root.addView(actionCard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(14)
        })

        // Pairing Key Card
        val keyCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg()
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        keyCard.addView(TextView(this).apply {
            text = "Sibling Security Key (Must match Windows PC)"
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.parseColor("#E2E8F0"))
            setPadding(0, 0, 0, dp(6))
        })
        keyInput = EditText(this).apply {
            setText(prefs.getString("sibling_key", "CHAINIT-SIBLING"))
            setTextColor(Color.parseColor("#6EE7B7"))
            typeface = Typeface.MONOSPACE
            textSize = 13f
            background = cardBg("#334155", "#020617")
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }
        val saveKeyBtn = Button(this).apply {
            text = "Save Sibling Key"
            isAllCaps = false
            textSize = 12f
            setTextColor(Color.WHITE)
            background = buttonBg("#334155")
            setOnClickListener {
                val k = keyInput.text.toString().trim().ifEmpty { "CHAINIT-SIBLING" }
                prefs.edit().putString("sibling_key", k).apply()
                Toast.makeText(this@MainActivity, "Saved Sibling Key: $k", Toast.LENGTH_SHORT).show()
            }
        }
        keyCard.addView(keyInput, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(8)
        })
        keyCard.addView(saveKeyBtn)
        root.addView(keyCard)

        scroll.addView(root)
        setContentView(scroll)
    }

    private fun hasAllFilesPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun refreshStatusUi() {
        val prefs = getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        val enabled = prefs.getBoolean("sibling_enabled", true)
        statusBadgeText.text = if (enabled && SiblingDaemonService.isRunning) {
            "Sibling Mode: ACTIVE"
        } else {
            "Sibling Mode: PAUSED"
        }

        val storageOk = hasAllFilesPermission()
        storageBtn.text = if (storageOk) {
            "✓ 1. All-Files Storage Access Granted (Z:\\ Ready)"
        } else {
            "1. Tap to Grant All-Files Access (Required for Z:\\)"
        }
        storageBtn.background = buttonBg(if (storageOk) "#065F46" else "#10B981")
        storageBtn.setTextColor(if (storageOk) Color.parseColor("#A7F3D0") else Color.parseColor("#020617"))

        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        val battOk = pm.isIgnoringBatteryOptimizations(packageName)
        batteryBtn.text = if (battOk) {
            "✓ 2. Screen-Off Standby Enabled (Unrestricted)"
        } else {
            "2. Tap to Allow Screen-Off Background Standby"
        }

        val btPermOk = BluetoothRfcommTransport.hasBluetoothPermission(this)
        val btAdapterOn = BluetoothRfcommTransport.getLocalAdapter(this)?.isEnabled == true
        btPermBtn.text = when {
            !btPermOk -> "3. Tap to Allow Bluetooth Connectivity"
            !btAdapterOn -> "3. Bluetooth Radio is OFF (Turn ON for Bluetooth Mode)"
            else -> "✓ 3. Bluetooth Sibling Socket Ready (RFCOMM Active)"
        }
        btPermBtn.background = buttonBg(if (btPermOk && btAdapterOn) "#065F46" else "#1E293B")

        val lines = mutableListOf<String>()
        for (ep in SiblingDaemonService.getEndpoints(this)) {
            lines.add("• [${ep.transportKind}] ${ep.ip}:49180 (${ep.ifaceName})")
        }
        if (SiblingDaemonService.isBluetoothListening()) {
            val btPeer = SiblingDaemonService.getLastBluetoothPeer()
            lines.add("• [bluetooth] RFCOMM Listening${if (btPeer != null) " (Linked: $btPeer)" else " (Paired PC Ready)"}")
        }
        endpointsText.text = if (lines.isEmpty()) {
            "Turn on Wi-Fi, Hotspot, USB Tethering, or Bluetooth to connect!"
        } else {
            lines.joinToString("\n")
        }

        val pc = SiblingDaemonService.getDiscoveredPc()
        if (pc != null) {
            pcStatusText.text = "Connected PC: ${pc.pcName} (${pc.ip}:${pc.httpPort})\nLast Z:\\ Activity: ${SiblingDaemonService.getLastVfsSummary()}"
            if (pcIpInput.text.toString().isBlank()) {
                pcIpInput.setText(pc.ip)
                prefs.edit().putString("last_pc_ip", pc.ip).apply()
            }
        } else {
            pcStatusText.text = "Last Activity: ${SiblingDaemonService.getLastVfsSummary()}"
        }
    }

    private fun resolveTargetPcIp(): String {
        val manual = pcIpInput.text.toString().trim()
        if (manual.isNotEmpty()) {
            getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
                .edit().putString("last_pc_ip", manual).apply()
            return manual
        }
        return SiblingDaemonService.getDiscoveredPc()?.ip ?: ""
    }

    private fun pickFile(requestCode: Int) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, requestCode)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode == RESULT_OK && data?.data != null) {
            val uri = data.data!!
            var fileName = "phone_file_${System.currentTimeMillis()}"
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) {
                    fileName = cursor.getString(idx) ?: fileName
                }
            }

            if (requestCode == REQ_PICK_FILE_BT_ONLY) {
                sendOverBluetoothThread(uri, fileName)
            } else if (requestCode == REQ_PICK_FILE_AUTO) {
                val ip = resolveTargetPcIp()
                if (ip.isBlank()) {
                    // No IP available -> automatically send via Bluetooth RFCOMM!
                    sendOverBluetoothThread(uri, fileName)
                } else {
                    uploadUriToPcWithBtFallback(uri, fileName, ip)
                }
            }
        }
    }

    private fun sendOverBluetoothThread(uri: Uri, fileName: String) {
        Toast.makeText(this, "Sending $fileName over Bluetooth...", Toast.LENGTH_SHORT).show()
        Thread {
            val (ok, msg) = BluetoothRfcommTransport.sendUriOverBluetoothToPairedPc(this, uri, fileName)
            uiHandler.post {
                Toast.makeText(this, msg, if (ok) Toast.LENGTH_LONG else Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun uploadUriToPcWithBtFallback(uri: Uri, fileName: String, pcIp: String) {
        Toast.makeText(this, "Sending $fileName to PC...", Toast.LENGTH_SHORT).show()
        Thread {
            try {
                val encoded = URLEncoder.encode(fileName, "UTF-8")
                val url = URL("http://$pcIp:4173/api/vfs/pc/upload?dir=%2FDesktop&filename=$encoded")
                val conn = (url.openConnection() as HttpURLConnection).apply {
                    requestMethod = "POST"
                    doOutput = true
                    connectTimeout = 4000
                    readTimeout = 60000
                    setChunkedStreamingMode(64 * 1024)
                    setRequestProperty("Content-Type", "application/octet-stream")
                }
                contentResolver.openInputStream(uri)?.use { input ->
                    conn.outputStream.use { out ->
                        input.copyTo(out, 64 * 1024)
                    }
                }
                val code = conn.responseCode
                if (code in 200..299) {
                    uiHandler.post {
                        Toast.makeText(this, "✓ Sent $fileName to Windows Desktop!", Toast.LENGTH_LONG).show()
                    }
                    return@Thread
                }
            } catch (_: Exception) {
                // High-speed IP link unreachable -> Automatically fall back to Bluetooth RFCOMM!
            }
            val (_, btMsg) = BluetoothRfcommTransport.sendUriOverBluetoothToPairedPc(this, uri, fileName)
            uiHandler.post {
                Toast.makeText(this, btMsg, Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun openStoragePermissionSettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                val intent = Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:$packageName")
                )
                startActivity(intent)
            } catch (_: Exception) {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            requestPermissions(
                arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE),
                REQ_PERMS
            )
        }
    }

    private fun requestBatteryExemption() {
        try {
            val intent = Intent(
                Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:$packageName")
            )
            startActivity(intent)
        } catch (_: Exception) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }

    private fun requestBluetoothPermissionsExplicitly() {
        if (Build.VERSION.SDK_INT >= 31) {
            requestPermissions(
                arrayOf(
                    Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_ADVERTISE
                ),
                REQ_PERMS
            )
        } else {
            startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
        }
    }

    private fun requestInitialRuntimePermissions() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            perms.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                perms.add(Manifest.permission.BLUETOOTH_CONNECT)
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                perms.add(Manifest.permission.BLUETOOTH_SCAN)
            }
        }
        if (Build.VERSION.SDK_INT <= 29 &&
            checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED
        ) {
            perms.add(Manifest.permission.READ_EXTERNAL_STORAGE)
            perms.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
        if (perms.isNotEmpty()) {
            requestPermissions(perms.toTypedArray(), REQ_PERMS)
        }
    }
}
