package com.chainit.android.ui

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.chainit.android.BuildConfig
import com.chainit.android.ChainItApp
import com.chainit.android.R
import com.chainit.android.net.BluetoothRfcommTransport
import com.chainit.android.service.SiblingDaemonService
import com.chainit.android.update.UpdateManager
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * ChainIt Sibling — single-screen, card-based companion UI.
 *
 * Layout order (top → bottom): brand header, live link hero, one-time setup checklist,
 * active transports, share-to-PC actions, Sibling key, app update, crash recovery.
 * Built with the shared [Ds] design tokens so it matches the Windows app pixel-for-pixel in tone.
 */
class MainActivity : Activity() {

    companion object {
        private const val REQ_PICK_FILE_AUTO = 2001
        private const val REQ_PICK_FILE_BT_ONLY = 2003
        private const val REQ_PERMS = 2002
        private const val PREFS = "chainit_prefs"
    }

    private val ui = Handler(Looper.getMainLooper())

    // Hero
    private lateinit var heroPill: TextView
    private lateinit var heroTitle: TextView
    private lateinit var heroSubtitle: TextView
    private lateinit var metricLink: TextView
    private lateinit var metricStorage: TextView
    private lateinit var metricScreen: TextView
    private lateinit var metricBattery: TextView
    private lateinit var siblingSwitch: Switch

    // Cards
    private lateinit var rowStorage: SetupRow
    private lateinit var rowBattery: SetupRow
    private lateinit var rowBluetooth: SetupRow
    private lateinit var endpointsText: TextView
    private lateinit var pcStatusText: TextView
    private var btStatusText: TextView? = null
    private lateinit var pcIpInput: android.widget.EditText
    private lateinit var keyInput: android.widget.EditText
    private lateinit var updateTitle: TextView
    private lateinit var updateStatus: TextView
    private lateinit var updateButton: TextView
    private lateinit var crashCard: LinearLayout
    private lateinit var crashText: TextView
    private lateinit var sendAutoBtn: TextView
    private lateinit var sendBtBtn: TextView
    private lateinit var browsePcBtn: TextView

    @Volatile
    private var busy = false

    private val refreshTick = object : Runnable {
        override fun run() {
            runCatching { refreshStatusUi() }
            ui.postDelayed(this, 2000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        window.statusBarColor = Ds.bg
        window.navigationBarColor = Ds.bg
        window.decorView.setBackgroundColor(Ds.bg)

        var fatalError: String? = null
        try {
            requestInitialRuntimePermissions()
            val prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            if (prefs.getBoolean("sibling_enabled", true) && !SiblingDaemonService.isRunning) {
                SiblingDaemonService.start(this)
            }
            setContentView(buildScreen())
        } catch (t: Throwable) {
            fatalError = "${t.javaClass.name}: ${t.message}"
            setContentView(buildFatalScreen(fatalError))
        }

        if (fatalError == null) {
            showCrashCardIfNeeded()
            runCatching { refreshStatusUi() }
        }
    }

    override fun onResume() {
        super.onResume()
        if (::heroTitle.isInitialized) {
            runCatching { refreshStatusUi() }
            ui.post(refreshTick)
        }
    }

    override fun onPause() {
        ui.removeCallbacks(refreshTick)
        super.onPause()
    }

    // ---------------------------------------------------------------------------------------------
    // Layout
    // ---------------------------------------------------------------------------------------------

    private fun buildScreen(): View {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(Ds.bg)
            isFillViewport = true
            isVerticalScrollBarEnabled = false
            setPadding(0, 0, 0, Ds.dp(this@MainActivity, 24f))
        }

        val root = Ds.col(this).apply {
            setPadding(Ds.dp(context, 18f), Ds.dp(context, 18f), Ds.dp(context, 18f), 0)
        }

        root.addView(buildHeader())
        root.addView(Ds.spacer(this, 18f))
        root.addView(buildHero())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildSetupCard())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildLinksCard())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildPcFilesCard())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildShareCard())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildKeyCard())
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildUpdateCard())
        root.addView(Ds.spacer(this, 12f))

        crashCard = buildCrashCard()
        root.addView(crashCard)
        root.addView(Ds.spacer(this, 12f))
        root.addView(buildFooter())

        scroll.addView(root)
        return scroll
    }

    private fun buildHeader(): View {
        val bar = Ds.row(this)
        bar.addView(Ds.logo(this, 38f))

        val titles = Ds.col(this).apply {
            layoutParams = Ds.lp(this@MainActivity, weight = 1f, startDp = 12f)
        }
        titles.addView(Ds.text(this, getString(R.string.app_name), 19f, Ds.textPrimary, bold = true))
        titles.addView(Ds.text(this, "Sibling link • works with the screen off", 11.5f, Ds.textMuted))
        bar.addView(titles)

        val (name, code) = UpdateManager.currentVersion(this)
        bar.addView(
            Ds.pill(this, "v$name", Ds.accentSoft).apply {
                layoutParams = Ds.lp(this@MainActivity, width = ViewGroup.LayoutParams.WRAP_CONTENT)
            }
        )
        return bar
    }

    private fun buildHero(): View {
        val card = Ds.card(this, 20f, Ds.surface, Ds.border)

        val top = Ds.row(this)
        heroPill = Ds.pill(this, "Starting", Ds.textMuted)
        top.addView(heroPill)
        val spacer = View(this).apply { layoutParams = Ds.lp(this@MainActivity, weight = 1f) }
        top.addView(spacer)

        siblingSwitch = Switch(this).apply {
            isChecked = getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("sibling_enabled", true)
            text = "Sibling Mode"
            textSize = 12.5f
            setTextColor(Ds.textSecondary)
            switchPadding = Ds.dp(this@MainActivity, 10f)
            thumbTintList = android.content.res.ColorStateList.valueOf(Ds.accent)
            trackTintList = android.content.res.ColorStateList.valueOf(Ds.border)
            setOnCheckedChangeListener { _, checked ->
                getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean("sibling_enabled", checked).apply()
                if (checked) {
                    SiblingDaemonService.start(this@MainActivity)
                    toast("Sibling Mode ON — listening for your Windows PC")
                } else {
                    runCatching { stopService(Intent(this@MainActivity, SiblingDaemonService::class.java)) }
                    toast("Sibling Mode paused")
                }
                refreshStatusUi()
            }
        }
        top.addView(siblingSwitch)
        card.addView(top)

        heroTitle = Ds.text(this, "Looking for your Windows PC…", 21f, Ds.textPrimary, bold = true)
        heroTitle.setPadding(0, Ds.dp(this, 14f), 0, 0)
        card.addView(heroTitle)

        heroSubtitle = Ds.text(this, "Turn on Wi-Fi, hotspot, USB tethering or Bluetooth on both devices.", 12.5f, Ds.textSecondary)
        heroSubtitle.setPadding(0, Ds.dp(this, 5f), 0, 0)
        heroSubtitle.setLineSpacing(0f, 1.25f)
        card.addView(heroSubtitle)

        // Metric tiles
        val tiles = Ds.row(this)
        tiles.setPadding(0, Ds.dp(this, 16f), 0, 0)
        metricLink = addMetricTile(tiles, "LINK", "—", 0)
        metricStorage = addMetricTile(tiles, "STORAGE", "—", Ds.dp(this, 8f))
        metricScreen = addMetricTile(tiles, "SCREEN", "—", Ds.dp(this, 8f))
        // The same reading the PC shows on its Link tab: this phone's own charge and plug, so a
        // session that dies at 3% is never a mystery on either screen.
        metricBattery = addMetricTile(tiles, "BATTERY", "—", Ds.dp(this, 8f))
        card.addView(tiles)

        return card
    }

    private fun addMetricTile(parent: LinearLayout, label: String, value: String, startMargin: Int): TextView {
        val tile = Ds.col(this, 12f).apply {
            background = Ds.surface(this@MainActivity, Ds.surfaceSunken, Ds.borderSoft, 16f)
            layoutParams = Ds.lp(this@MainActivity, weight = 1f, startDp = if (startMargin == 0) 0f else 8f)
        }
        tile.addView(Ds.text(this, label, 9.5f, Ds.textMuted, bold = true, letterSpacing = 0.07f))
        val valueView = Ds.text(this, value, 13f, Ds.textPrimary, medium = true)
        valueView.setPadding(0, Ds.dp(this, 4f), 0, 0)
        tile.addView(valueView)
        parent.addView(tile)
        return valueView
    }

    private class SetupRow(val root: LinearLayout, val pill: TextView, val onClick: () -> Unit)

    private fun buildSetupCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "One-time setup"))

        rowStorage = addSetupRow(card, "All-files access", "Lets Windows browse Z:\\ without prompts") { openStoragePermissionSettings() }
        rowBattery = addSetupRow(card, "Screen-off standby", "Keeps the link alive while locked") { requestBatteryExemption() }
        rowBluetooth = addSetupRow(card, "Bluetooth", "Native RFCOMM link, zero Wi-Fi needed") { requestBluetoothPermissionsExplicitly() }

        return card
    }

    private fun addSetupRow(parent: LinearLayout, title: String, subtitle: String, action: () -> Unit): SetupRow {
        parent.addView(Ds.divider(this))

        val row = Ds.row(this)
        row.setPadding(0, Ds.dp(this, 13f), 0, Ds.dp(this, 13f))
        row.background = Ds.ripple(this, Ds.surface(this, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
        row.isClickable = true
        row.isFocusable = true

        val labels = Ds.col(this).apply { layoutParams = Ds.lp(this@MainActivity, weight = 1f) }
        labels.addView(Ds.text(this, title, 14f, Ds.textPrimary, medium = true))
        labels.addView(Ds.text(this, subtitle, 11.5f, Ds.textMuted).apply {
            setPadding(0, Ds.dp(this@MainActivity, 3f), 0, 0)
        })
        row.addView(labels)

        val pill = Ds.pill(this, "Tap to allow", Ds.amber)
        pill.layoutParams = Ds.lp(this, width = ViewGroup.LayoutParams.WRAP_CONTENT, startDp = 10f)
        row.addView(pill)

        row.setOnClickListener { action() }
        parent.addView(row)
        return SetupRow(row, pill, action)
    }

    private fun buildLinksCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "Active links"))

        endpointsText = Ds.text(this, "Scanning interfaces…", 12f, Ds.textSecondary, mono = true)
        endpointsText.setPadding(0, Ds.dp(this, 10f), 0, 0)
        endpointsText.setLineSpacing(0f, 1.35f)
        card.addView(endpointsText)

        card.addView(Ds.divider(this).apply {
            (layoutParams as LinearLayout.LayoutParams).topMargin = Ds.dp(this@MainActivity, 12f)
        })

        pcStatusText = Ds.text(this, "Waiting for ChainIt-Windows.exe…", 12f, Ds.textSecondary)
        pcStatusText.setPadding(0, Ds.dp(this, 12f), 0, 0)
        pcStatusText.setLineSpacing(0f, 1.3f)
        card.addView(pcStatusText)

        return card
    }

    private fun buildShareCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "Send to Windows"))

        pcIpInput = Ds.input(this, "PC IP  (blank = auto-discovery)", getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("last_pc_ip", "") ?: "")
        pcIpInput.layoutParams = Ds.lp(this, topDp = 10f)
        pcIpInput.inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI
        card.addView(pcIpInput)

        sendAutoBtn = Ds.button(this, "Send file to PC") { pickFile(REQ_PICK_FILE_AUTO) }
        sendAutoBtn.layoutParams = Ds.lp(this, topDp = 10f)
        card.addView(sendAutoBtn)

        sendBtBtn = Ds.button(this, "Send via Bluetooth only", Ds.ButtonVariant.SECONDARY) { pickFile(REQ_PICK_FILE_BT_ONLY) }
        sendBtBtn.layoutParams = Ds.lp(this, topDp = 8f)
        card.addView(sendBtBtn)

        card.addView(
            Ds.text(this, "Sends to the PC over Wi-Fi, and over Bluetooth RFCOMM when Wi-Fi is gone.", 11f, Ds.textMuted).apply {
                layoutParams = Ds.lp(this@MainActivity, topDp = 8f)
                setLineSpacing(0f, 1.3f)
            }
        )

        return card
    }

    /**
     * PC files — the native browser.
     *
     * This card used to hold a button that fired `Intent(ACTION_VIEW, http://<pc>:4173/mobile)`,
     * i.e. it sent the user to Edge/Chrome and let a browser draw ChainIt's files. That is the same
     * mistake Windows made before it grew its own file manager: the recent-apps tile belongs to the
     * browser, and the feature dies with Wi-Fi. [PcBrowserActivity] now draws the listing itself and
     * reaches the PC over Wi-Fi *or* Bluetooth RFCOMM, so no browser is involved at any point.
     */
    private fun buildPcFilesCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "PC files"))

        card.addView(
            Ds.text(
                this,
                "Browse the folders ChainIt shares on your Windows PC. Drawn by ChainIt itself — no browser, no WebView.",
                12f,
                Ds.textSecondary
            ).apply {
                layoutParams = Ds.lp(this@MainActivity, topDp = 10f)
                setLineSpacing(0f, 1.3f)
            }
        )

        browsePcBtn = Ds.button(this, "Browse PC files", Ds.ButtonVariant.PRIMARY) { openPcBrowser() }
        browsePcBtn.layoutParams = Ds.lp(this, topDp = 12f)
        card.addView(browsePcBtn)

        card.addView(
            Ds.text(
                this,
                "Works over Wi-Fi, hotspot, USB tethering or paired Bluetooth. The PC shares your Windows profile folder as the root: Desktop, Documents, Downloads, Pictures…",
                11f,
                Ds.textMuted
            ).apply {
                layoutParams = Ds.lp(this@MainActivity, topDp = 8f)
                setLineSpacing(0f, 1.3f)
            }
        )

        return card
    }

    /** Opens the native browser. A blank IP is fine: Bluetooth alone can still reach a paired PC. */
    private fun openPcBrowser() {
        val ip = resolveTargetPcIp()
        val pc = SiblingDaemonService.getDiscoveredPc()
        val intent = Intent(this, PcBrowserActivity::class.java)
        if (ip.isNotBlank()) intent.putExtra(PcBrowserActivity.EXTRA_IP, ip)
        if (pc != null && pc.pcName.isNotBlank()) intent.putExtra(PcBrowserActivity.EXTRA_PC_NAME, pc.pcName)
        val started = runCatching { startActivity(intent) }.isSuccess
        if (!started) toast("ChainIt could not open its own PC file browser")
    }

    private fun buildKeyCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "Sibling security key"))

        keyInput = Ds.input(
            this,
            "CHAINIT-SIBLING",
            getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("sibling_key", "CHAINIT-SIBLING") ?: "CHAINIT-SIBLING",
            mono = true
        )
        keyInput.layoutParams = Ds.lp(this, topDp = 10f)
        card.addView(keyInput)

        card.addView(
            Ds.text(this, "Must match the key shown by ChainIt-Windows.exe.", 11f, Ds.textMuted).apply {
                layoutParams = Ds.lp(this@MainActivity, topDp = 8f)
            }
        )

        val save = Ds.button(this, "Save key", Ds.ButtonVariant.SECONDARY) {
            val k = keyInput.text.toString().trim().ifEmpty { "CHAINIT-SIBLING" }
            keyInput.setText(k)
            getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("sibling_key", k).apply()
            toast("Sibling key saved")
        }
        save.layoutParams = Ds.lp(this, topDp = 12f)
        card.addView(save)

        return card
    }

    private fun buildUpdateCard(): View {
        val card = Ds.card(this)
        card.addView(Ds.sectionLabel(this, "App update"))

        val (name, code) = UpdateManager.currentVersion(this)
        updateTitle = Ds.text(this, "Installed: v$name (build $code)", 13.5f, Ds.textPrimary, medium = true)
        updateTitle.setPadding(0, Ds.dp(this, 10f), 0, 0)
        card.addView(updateTitle)

        updateStatus = Ds.text(this, "Updates install over the current app — nothing is duplicated.", 11.5f, Ds.textMuted)
        updateStatus.setPadding(0, Ds.dp(this, 4f), 0, 0)
        updateStatus.setLineSpacing(0f, 1.3f)
        card.addView(updateStatus)

        updateButton = Ds.button(this, "Check for update", Ds.ButtonVariant.SECONDARY) { checkForUpdate() }
        updateButton.layoutParams = Ds.lp(this, topDp = 12f)
        card.addView(updateButton)

        return card
    }

    private fun buildCrashCard(): LinearLayout {
        val card = Ds.card(this, 18f, 0xFF1A0F14.toInt(), 0xFF4C1D2A.toInt())
        card.visibility = View.GONE

        card.addView(Ds.text(this, "Recovered from a crash", 13.5f, 0xFFFDA4AF.toInt(), bold = true))
        crashText = Ds.text(this, "", 10.5f, 0xFFFECDD3.toInt(), mono = true)
        crashText.setPadding(0, Ds.dp(this, 8f), 0, Ds.dp(this, 10f))
        crashText.setTextIsSelectable(true)
        card.addView(crashText)

        val actions = Ds.row(this)
        val dismiss = Ds.button(this, "Dismiss", Ds.ButtonVariant.SECONDARY) {
            ChainItApp.clearLastCrash(this)
            card.visibility = View.GONE
        }
        dismiss.layoutParams = Ds.lp(this, weight = 1f)
        actions.addView(dismiss)

        val share = Ds.button(this, "Share report", Ds.ButtonVariant.GHOST) {
            val report = ChainItApp.readLastCrash(this) ?: return@button
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_SUBJECT, "ChainIt crash report")
                putExtra(Intent.EXTRA_TEXT, report)
            }
            runCatching { startActivity(Intent.createChooser(send, "Send crash report")) }
        }
        share.layoutParams = Ds.lp(this, weight = 1f, startDp = 8f)
        actions.addView(share)

        card.addView(actions)
        return card
    }

    private fun buildFooter(): View {
        val col = Ds.col(this)

        // Attribution - the same credit the Windows app shows on its About tab.
        col.addView(
            Ds.text(this, "Built by Qmer", 13f, Ds.accentSoft, bold = true).apply {
                gravity = Gravity.CENTER
                letterSpacing = 0.04f
            }
        )
        col.addView(
            Ds.text(
                this,
                "ChainIt Sibling v" + BuildConfig.VERSION_LABEL + " (build " + BuildConfig.VERSION_CODE +
                    ")  •  offline-first by design",
                11f,
                Ds.textMuted,
                medium = true
            ).apply {
                gravity = Gravity.CENTER
                layoutParams = Ds.lp(this@MainActivity, topDp = 3f)
            }
        )

        // Live Bluetooth readout: proves RFCOMM is serving Z:\ rather than merely "connected".
        btStatusText = Ds.text(this, "Bluetooth RFCOMM: starting…", 10.5f, Ds.textSecondary, mono = true).apply {
            gravity = Gravity.CENTER
            layoutParams = Ds.lp(this@MainActivity, topDp = 7f)
            setLineSpacing(0f, 1.25f)
        }
        col.addView(btStatusText)
        col.addView(
            Ds.text(
                this,
                "VFS :49180   Discovery :49179   Phone UUID c4a1b170-0001-…34fb   PC UUID c4a1b170-0002-…34fb",
                9.5f,
                0xFF475569.toInt(),
                mono = true
            ).apply {
                gravity = Gravity.CENTER
                layoutParams = Ds.lp(this@MainActivity, topDp = 5f)
                setLineSpacing(0f, 1.3f)
            }
        )
        return col
    }

    /** Last-resort screen: shown only if the real UI could not be constructed. */
    private fun buildFatalScreen(error: String): View {
        val root = Ds.col(this, 22f).apply { setBackgroundColor(Ds.bg) }
        root.addView(Ds.logo(this, 44f))
        root.addView(Ds.text(this, "ChainIt could not start its interface", 17f, Ds.textPrimary, bold = true).apply {
            layoutParams = Ds.lp(this@MainActivity, topDp = 16f)
        })
        root.addView(Ds.text(this, "The background link may still be running. Share this detail so it can be fixed:", 12.5f, Ds.textSecondary).apply {
            layoutParams = Ds.lp(this@MainActivity, topDp = 8f)
            setLineSpacing(0f, 1.3f)
        })
        val box = Ds.text(this, error, 11f, 0xFFFDA4AF.toInt(), mono = true).apply {
            background = Ds.surface(this@MainActivity, Ds.surfaceSunken, 0xFF4C1D2A.toInt(), 14f)
            setPadding(Ds.dp(this@MainActivity, 12f), Ds.dp(this@MainActivity, 12f), Ds.dp(this@MainActivity, 12f), Ds.dp(this@MainActivity, 12f))
            setTextIsSelectable(true)
            layoutParams = Ds.lp(this@MainActivity, topDp = 12f)
        }
        root.addView(box)
        root.addView(Ds.button(this, "Retry", Ds.ButtonVariant.SECONDARY) { recreate() }.apply {
            layoutParams = Ds.lp(this@MainActivity, topDp = 14f)
        })
        return root
    }

    // ---------------------------------------------------------------------------------------------
    // Live status
    // ---------------------------------------------------------------------------------------------

    private fun refreshStatusUi() {
        val prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val siblingOn = prefs.getBoolean("sibling_enabled", true)

        // Setup rows
        val storageOk = hasAllFilesPermission()
        setRow(rowStorage, storageOk, "Granted", "Tap to allow")

        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        val battOk = runCatching { pm.isIgnoringBatteryOptimizations(packageName) }.getOrDefault(false)
        setRow(rowBattery, battOk, "Unrestricted", "Tap to allow")

        val btPermOk = BluetoothRfcommTransport.hasBluetoothPermission(this)
        val btAdapterOn = runCatching { BluetoothRfcommTransport.getLocalAdapter(this)?.isEnabled == true }.getOrDefault(false)
        val btOk = btPermOk && btAdapterOn
        setRow(
            rowBluetooth, btOk,
            if (SiblingDaemonService.isBluetoothListening()) "Listening" else "Ready",
            when {
                !btPermOk -> "Tap to allow"
                !btAdapterOn -> "Radio off"
                else -> "Ready"
            }
        )

        // Endpoints
        val lines = mutableListOf<String>()
        for (ep in SiblingDaemonService.getEndpoints(this)) {
            lines.add("${ep.transportKind.padEnd(14)}  ${ep.ip}:49180   ${ep.ifaceName}")
        }
        if (SiblingDaemonService.isBluetoothListening()) {
            val peer = SiblingDaemonService.getLastBluetoothPeer()
            lines.add("${"bluetooth".padEnd(14)}  RFCOMM listening${if (peer != null) "  ⇄ $peer" else ""}")
        }
        endpointsText.text = if (lines.isEmpty()) {
            if (siblingOn) "No active interface yet — enable Wi-Fi, hotspot, USB tethering or Bluetooth."
            else "Sibling Mode is paused."
        } else {
            lines.joinToString("\n")
        }
        endpointsText.setTextColor(if (lines.isEmpty()) Ds.textMuted else Ds.accentSoft)

        // PC status
        val pc = SiblingDaemonService.getDiscoveredPc()
        if (pc != null) {
            pcStatusText.text = "Windows PC: ${pc.pcName}  (${pc.ip}:${pc.httpPort})\nLast Z:\\ activity: ${SiblingDaemonService.getLastVfsSummary()}"
            pcStatusText.setTextColor(Ds.textSecondary)
            if (pcIpInput.text.toString().isBlank()) {
                pcIpInput.setText(pc.ip)
                prefs.edit().putString("last_pc_ip", pc.ip).apply()
            }
        } else {
            pcStatusText.text = "Last Z:\\ activity: ${SiblingDaemonService.getLastVfsSummary()}"
            pcStatusText.setTextColor(Ds.textMuted)
        }

        // Hero
        val serviceUp = SiblingDaemonService.isRunning
        val btLive = SiblingDaemonService.isBluetoothListening()
        val linkLive = pc != null || lines.isNotEmpty()

        val pillState = when {
            !siblingOn -> Triple("Paused", Ds.textMuted, "Sibling Mode is off — turn it on to share storage.")
            serviceUp && pc != null -> Triple("Connected", Ds.accent, "Windows PC ${pc.pcName} is mounted as Z:\\")
            serviceUp && btLive -> Triple("Bluetooth live", Ds.info, "RFCOMM link serving Z:\\ with zero Wi-Fi")
            serviceUp && linkLive -> Triple("Listening", Ds.info, "Serving on ${lines.size} local link(s) — waiting for Windows")
            serviceUp -> Triple("Starting", Ds.amber, "Daemon is up, no usable radio yet")
            else -> Triple("Stopped", Ds.danger, SiblingDaemonService.startupError ?: "Daemon is not running — tap Sibling Mode to restart")
        }
        heroPill.text = pillState.first.uppercase()
        heroPill.setTextColor(pillState.second)
        heroPill.background = Ds.surface(this, tintBg(pillState.second), Color.TRANSPARENT, Ds.radiusPill, 0f)
        heroTitle.text = when {
            !siblingOn -> "Sibling Mode paused"
            pc != null -> "Connected to ${pc.pcName}"
            else -> pillState.third
        }
        heroSubtitle.text = when {
            !siblingOn -> "Enable Sibling Mode so Windows can mount this phone as Z:\\ — even while locked."
            pc != null -> "Z:\\ is live on Windows. Files stay reachable with the screen off."
            serviceUp -> "Keep ChainIt-Windows.exe open on the PC. It finds this phone automatically on USB, Wi-Fi, hotspot or Bluetooth."
            else -> "Restarting the background daemon…"
        }

        metricLink.text = when {
            pc != null -> pc.ip
            lines.isNotEmpty() -> lines.size.toString() + " local"
            else -> "None"
        }
        metricStorage.text = if (storageOk) "All files" else "Limited"
        metricStorage.setTextColor(if (storageOk) Ds.textPrimary else Ds.amber)
        val keyguard = getSystemService(Context.KEYGUARD_SERVICE) as android.app.KeyguardManager
        metricScreen.text = if (!pm.isInteractive || keyguard.isKeyguardLocked) "Locked • served" else "On"

        val batt = com.chainit.android.net.BatteryTelemetry.read(this)
        metricBattery.text = when {
            batt.pct <= 0 -> "not reported"
            batt.charging -> "${batt.pct}% • ${batt.source.uppercase(java.util.Locale.US)}"
            else -> "${batt.pct}% • battery"
        }
        metricBattery.setTextColor(
            when {
                batt.charging || batt.pct >= 40 -> Ds.textPrimary
                batt.pct >= 15 -> Ds.amber
                else -> Ds.danger
            }
        )

        // Bluetooth is a full Z:\ transport, not a one-shot file push - show that it is really serving.
        btStatusText?.let { tv ->
            val listening = SiblingDaemonService.isBluetoothListening()
            val mode = SiblingDaemonService.bluetoothListenMode()
            val sessions = SiblingDaemonService.bluetoothSessions()
            val served = SiblingDaemonService.servedRequests()
            val open = SiblingDaemonService.openVfsSessions()
            val beats = SiblingDaemonService.heartbeatsSurvived()
            val err = SiblingDaemonService.bluetoothLastError()
            val text = when {
                listening -> buildString {
                    append("Bluetooth RFCOMM: LIVE (")
                    append(mode)
                    append(" channel)  •  ")
                    append(sessions)
                    append(" session(s) accepted  •  ")
                    append(open)
                    append(" keep-alive open now")
                    append("\nVFS requests served: ")
                    append(served)
                    append("  •  Doze heartbeats survived: ")
                    append(beats)
                }
                err != null -> "Bluetooth RFCOMM: re-arming — $err"
                else -> "Bluetooth RFCOMM: $mode"
            }
            tv.text = text
            tv.setTextColor(
                when {
                    listening -> Ds.accentSoft
                    err != null -> Ds.amber
                    else -> Ds.textMuted
                }
            )
        }
    }

    private fun tintBg(fg: Int): Int {
        val a = 0x1A
        return Color.argb(a, Color.red(fg), Color.green(fg), Color.blue(fg))
    }

    private fun setRow(row: SetupRow, ok: Boolean, okLabel: String, badLabel: String) {
        row.pill.text = (if (ok) okLabel else badLabel).uppercase()
        row.pill.setTextColor(if (ok) Ds.accentSoft else Ds.amber)
        row.pill.background = Ds.surface(this, tintBg(if (ok) Ds.accentSoft else Ds.amber), Color.TRANSPARENT, Ds.radiusPill, 0f)
    }

    private fun showCrashCardIfNeeded() {
        val report = ChainItApp.readLastCrash(this) ?: return
        crashCard.visibility = View.VISIBLE
        crashText.text = report.take(1400)
    }

    // ---------------------------------------------------------------------------------------------
    // Update
    // ---------------------------------------------------------------------------------------------

    private fun checkForUpdate() {
        if (busy) return
        busy = true
        updateButton.text = "Checking…"
        updateButton.alpha = 0.5f
        updateStatus.text = "Downloading the newest signed build…"
        updateStatus.setTextColor(Ds.textMuted)

        Thread {
            UpdateManager.checkAndDownload(this) { state ->
                ui.post { renderUpdateState(state) }
            }
        }.start()
    }

    private fun renderUpdateState(state: UpdateManager.State) {
        when (state) {
            is UpdateManager.State.Checking -> {
                updateStatus.text = state.note
                updateStatus.setTextColor(Ds.textMuted)
            }
            is UpdateManager.State.Downloading -> {
                updateStatus.text = if (state.percent >= 0) "Downloading update… ${state.percent}%" else "Downloading update… ${state.bytes / 1024} KB"
                updateStatus.setTextColor(Ds.textMuted)
            }
            is UpdateManager.State.UpToDate -> {
                busy = false
                updateButton.text = "Check for update"
                updateButton.alpha = 1f
                updateStatus.text = "You are on the latest build (v${state.versionName})."
                updateStatus.setTextColor(Ds.accentSoft)
            }
            is UpdateManager.State.Ready -> {
                busy = false
                updateButton.text = "Install v${state.versionName}"
                updateButton.alpha = 1f
                updateStatus.text = "New build ${state.versionCode} downloaded — installing replaces the current app and keeps your settings."
                updateStatus.setTextColor(Ds.info)
                updateButton.setOnClickListener {
                    val msg = UpdateManager.install(this@MainActivity, state.file)
                    toast(msg ?: "Opening installer — tap Install to replace the current version")
                    updateButton.setOnClickListener { checkForUpdate() }
                }
            }
            is UpdateManager.State.Failed -> {
                busy = false
                updateButton.text = "Check for update"
                updateButton.alpha = 1f
                updateStatus.text = "Update failed: ${state.message}"
                updateStatus.setTextColor(0xFFFDA4AF.toInt())
            }
            UpdateManager.State.Idle -> {}
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Permissions & helpers
    // ---------------------------------------------------------------------------------------------

    private fun hasAllFilesPermission(): Boolean = runCatching {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
    }.getOrDefault(false)

    private fun openStoragePermissionSettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val opened = runCatching {
                startActivity(
                    Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName"))
                )
            }.isSuccess
            if (!opened) {
                runCatching { startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)) }
                    .onFailure { toast("Open Settings → Apps → ChainIt → Permissions → All files access") }
            }
        } else {
            runCatching {
                requestPermissions(
                    arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE),
                    REQ_PERMS
                )
            }
        }
    }

    private fun requestBatteryExemption() {
        val ok = runCatching {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
            )
        }.isSuccess
        if (!ok) {
            runCatching { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) }
                .onFailure { toast("Open Settings → Battery → Unrestricted for ChainIt") }
        }
    }

    private fun requestBluetoothPermissionsExplicitly() {
        if (Build.VERSION.SDK_INT >= 31) {
            runCatching {
                requestPermissions(
                    arrayOf(
                        Manifest.permission.BLUETOOTH_CONNECT,
                        Manifest.permission.BLUETOOTH_SCAN,
                        Manifest.permission.BLUETOOTH_ADVERTISE
                    ),
                    REQ_PERMS
                )
            }
        } else {
            runCatching { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
                .onFailure { toast("Turn on Bluetooth in Settings, then pair with your PC once") }
        }
    }

    private fun requestInitialRuntimePermissions() {
        val perms = mutableListOf<String>()
        runCatching {
            if (Build.VERSION.SDK_INT >= 33 &&
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
            ) {
                perms.add(Manifest.permission.POST_NOTIFICATIONS)
            }
            if (Build.VERSION.SDK_INT >= 31) {
                if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) perms.add(Manifest.permission.BLUETOOTH_CONNECT)
                if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) perms.add(Manifest.permission.BLUETOOTH_SCAN)
            }
            if (Build.VERSION.SDK_INT <= 29 &&
                checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED
            ) {
                perms.add(Manifest.permission.READ_EXTERNAL_STORAGE)
                perms.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            }
        }
        if (perms.isNotEmpty()) {
            runCatching { requestPermissions(perms.toTypedArray(), REQ_PERMS) }
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (::heroTitle.isInitialized) runCatching { refreshStatusUi() }
    }

    private fun toast(msg: String) {
        runCatching { Toast.makeText(this, msg, Toast.LENGTH_LONG).show() }
    }

    private fun pickFile(requestCode: Int) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        val ok = runCatching { startActivityForResult(intent, requestCode) }.isSuccess
        if (!ok) toast("No file picker available on this device")
    }

    private fun resolveTargetPcIp(): String {
        val manual = if (::pcIpInput.isInitialized) pcIpInput.text.toString().trim() else ""
        if (manual.isNotEmpty()) {
            getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("last_pc_ip", manual).apply()
            return manual
        }
        return SiblingDaemonService.getDiscoveredPc()?.ip ?: ""
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return

        var fileName = "phone_file_${System.currentTimeMillis()}"
        runCatching {
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) fileName = cursor.getString(idx) ?: fileName
            }
        }

        if (requestCode == REQ_PICK_FILE_BT_ONLY) {
            sendOverBluetoothThread(uri, fileName)
        } else {
            val ip = resolveTargetPcIp()
            if (ip.isBlank()) sendOverBluetoothThread(uri, fileName) else uploadUriToPcWithBtFallback(uri, fileName, ip)
        }
    }

    private fun sendOverBluetoothThread(uri: Uri, fileName: String) {
        toast("Sending $fileName over Bluetooth…")
        Thread {
            val result = runCatching { BluetoothRfcommTransport.sendUriOverBluetoothToPairedPc(this, uri, fileName) }
            val msg = result.getOrNull()?.second ?: "Bluetooth send failed: ${result.exceptionOrNull()?.javaClass?.simpleName}"
            ui.post { toast(msg) }
        }.start()
    }

    private fun uploadUriToPcWithBtFallback(uri: Uri, fileName: String, pcIp: String) {
        toast("Sending $fileName to PC…")
        Thread {
            try {
                val encoded = URLEncoder.encode(fileName, "UTF-8").replace("+", "%20")
                val url = URL("http://$pcIp:4173/api/vfs/pc/upload?dir=%2FDesktop&filename=$encoded")
                val conn = (url.openConnection() as HttpURLConnection).apply {
                    requestMethod = "POST"
                    doOutput = true
                    connectTimeout = 4000
                    readTimeout = 120000
                    setChunkedStreamingMode(64 * 1024)
                    setRequestProperty("Content-Type", "application/octet-stream")
                }
                contentResolver.openInputStream(uri)?.use { input ->
                    conn.outputStream.use { out -> input.copyTo(out, 64 * 1024) }
                }
                val code = conn.responseCode
                conn.disconnect()
                if (code in 200..299) {
                    ui.post { toast("Sent $fileName to the Windows Desktop") }
                    return@Thread
                }
            } catch (_: Exception) {
                // Fast IP link unreachable → fall back to native Bluetooth RFCOMM.
            }
            val result = runCatching { BluetoothRfcommTransport.sendUriOverBluetoothToPairedPc(this, uri, fileName) }
            val msg = result.getOrNull()?.second ?: "Both links failed — is ChainIt-Windows.exe running?"
            ui.post { toast(msg) }
        }.start()
    }
}
