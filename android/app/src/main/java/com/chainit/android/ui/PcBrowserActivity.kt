package com.chainit.android.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Color
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.provider.Settings
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.chainit.android.net.ChainItFileProvider
import com.chainit.android.net.PcVfsClient
import com.chainit.android.vfs.FileOps
import org.json.JSONObject
import java.io.File

/**
 * Native PC file browser — the phone's half of "no browser, no WebView, no runtime".
 *
 * Before this screen existed, the "Browse PC files" button on the main screen fired
 * `Intent(ACTION_VIEW, http://<pc>:4173/mobile)`. That handed the phone to Edge/Chrome: the files
 * were drawn by somebody else's UI, the recent-apps tile said "Chrome", and the whole feature died
 * the moment Wi-Fi dropped. This activity draws the listing itself with the same [Ds] tokens as the
 * rest of the app and talks to the PC through [PcVfsClient], which uses Wi-Fi when it can and
 * Bluetooth RFCOMM when it cannot.
 *
 * What it does, all natively:
 *   • list / navigate the PC's shared root (the Windows profile folder, served as `/`)
 *   • sort by name, size, type or modified date — folders first, like Explorer
 *   • open text and images inside ChainIt, or hand a saved copy to another app via content://
 *   • save any file to the phone's Downloads, with a live byte count
 *   • new folder, rename, delete (single or multi-select), and send a phone file into the folder
 *   • say exactly why nothing happened when nothing happens (no link, wrong key, Sibling off, 429)
 */
class PcBrowserActivity : Activity() {

    companion object {
        const val EXTRA_IP = "chainit.pc.ip"
        const val EXTRA_PC_NAME = "chainit.pc.name"
        private const val REQ_SEND_HERE = 3101
        private const val PREFS = "chainit_prefs"

        private const val COL_SIZE_DP = 64f
        private const val COL_DATE_DP = 94f
        private const val BADGE_DP = 34f
    }

    private enum class SortMode(val label: String) {
        NAME("Name"), SIZE("Size"), TYPE("Type"), DATE("Modified")
    }

    private class SheetAction(val label: String, val color: Int, val run: () -> Unit)

    private val ui = Handler(Looper.getMainLooper())

    // --- state -----------------------------------------------------------------------------------
    private var target = PcVfsClient.Target("", PcVfsClient.PC_HTTP_PORT, "")
    private var cwd = "/"
    private var items: List<PcVfsClient.Item> = emptyList()
    private var sortMode = SortMode.NAME
    private var sortAsc = true
    private var foldersFirst = true
    private var selecting = false
    private val selection = LinkedHashSet<String>()

    @Volatile
    private var busy = false
    private var lastTransport = PcVfsClient.Transport.NONE
    private var lastRttMs = 0L
    private var lastProblem: String? = null
    private var lastProgressPostMs = 0L

    /**
     * Volumes the PC reported with its last listing (v1.7.0).
     *
     * "/" is the PC's profile folder, not the whole machine - a second partition, a card reader or a
     * mapped share is addressed as "/D:", "/E:" and so on. Every listing carries this array, so the
     * drive sheet costs no extra round trip (over Bluetooth that round trip is the slow part).
     */
    private var pcDrives: List<PcVfsClient.Drive> = emptyList()
    private var allowOtherDrives = true
    private var recentEntries: List<PcVfsClient.HistoryEntry> = emptyList()

    // --- views -----------------------------------------------------------------------------------
    private lateinit var headerSubtitle: TextView
    private lateinit var transportPill: TextView
    private lateinit var pathText: TextView
    private lateinit var upButton: TextView
    private lateinit var refreshButton: TextView
    private lateinit var newFolderButton: TextView
    private lateinit var sendHereButton: TextView
    private lateinit var sortButton: TextView
    private lateinit var drivesButton: TextView
    private lateinit var recentButton: TextView
    private lateinit var headerName: TextView
    private lateinit var headerSize: TextView
    private lateinit var headerDate: TextView
    private lateinit var listContainer: LinearLayout
    private lateinit var statusText: TextView
    private lateinit var selectionBar: LinearLayout
    private lateinit var selectionText: TextView
    private lateinit var sheetOverlay: FrameLayout
    private lateinit var sheetCard: LinearLayout
    private lateinit var viewerOverlay: FrameLayout
    private lateinit var viewerBody: LinearLayout
    private lateinit var viewerTitle: TextView
    private lateinit var promptOverlay: FrameLayout
    private lateinit var busyOverlay: FrameLayout
    private lateinit var busyText: TextView

    // ---------------------------------------------------------------------------------------------
    // Construction
    // ---------------------------------------------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        window.statusBarColor = Ds.bg
        window.navigationBarColor = Ds.bg
        window.decorView.setBackgroundColor(Ds.bg)

        target = PcVfsClient.resolveTarget(this, intent?.getStringExtra(EXTRA_IP))
        val extraName = intent?.getStringExtra(EXTRA_PC_NAME) ?: ""
        if (extraName.isNotBlank() && target.pcName.isBlank()) {
            target = PcVfsClient.Target(target.ip, target.port, extraName)
        }

        setContentView(buildScreen())
        renderHeader()
        navigate(cwd)
    }

    private fun buildScreen(): View {
        val frame = FrameLayout(this).apply { setBackgroundColor(Ds.bg) }

        val main = Ds.col(this)
        main.addView(buildHeaderBar())
        main.addView(buildPathBar())
        main.addView(buildActionBar())
        main.addView(buildColumnHeader())
        main.addView(Ds.divider(this, Ds.border))

        val scroll = ScrollView(this).apply {
            isFillViewport = false
            isVerticalScrollBarEnabled = false
            layoutParams = Ds.lp(this@PcBrowserActivity, height = 0, weight = 1f)
        }
        listContainer = Ds.col(this)
        scroll.addView(listContainer)
        main.addView(scroll)

        selectionBar = buildSelectionBar()
        main.addView(selectionBar)

        statusText = Ds.text(this, "Connecting to the PC…", 11f, Ds.textMuted, mono = true).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 8f), Ds.dp(context, 14f), Ds.dp(context, 10f))
            setLineSpacing(0f, 1.25f)
            maxLines = 3
            ellipsize = TextUtils.TruncateAt.END
        }
        main.addView(statusText)

        frame.addView(main, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        busyOverlay = buildBusyOverlay()
        frame.addView(busyOverlay, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        sheetOverlay = buildSheetOverlay()
        frame.addView(sheetOverlay, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        viewerOverlay = buildViewerOverlay()
        frame.addView(viewerOverlay, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        promptOverlay = buildPromptOverlay()
        frame.addView(promptOverlay, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        return frame
    }

    private fun buildHeaderBar(): View {
        val bar = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 14f), Ds.dp(context, 14f), Ds.dp(context, 10f))
        }
        bar.addView(Ds.logo(this, 32f))

        val titles = Ds.col(this).apply { layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 10f) }
        titles.addView(Ds.text(this, "PC files", 17f, Ds.textPrimary, bold = true))
        headerSubtitle = Ds.text(this, "Connecting…", 11f, Ds.textMuted).apply {
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        titles.addView(headerSubtitle)
        bar.addView(titles)

        transportPill = Ds.pill(this, "NO LINK", Ds.textMuted)
        transportPill.layoutParams = Ds.lp(this, width = ViewGroup.LayoutParams.WRAP_CONTENT, startDp = 8f)
        bar.addView(transportPill)

        val close = Ds.text(this, "Close", 12.5f, Ds.textSecondary, bold = true).apply {
            setPadding(Ds.dp(context, 12f), Ds.dp(context, 9f), Ds.dp(context, 4f), Ds.dp(context, 9f))
            background = Ds.ripple(context, Ds.surface(context, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
            layoutParams = Ds.lp(this@PcBrowserActivity, width = ViewGroup.LayoutParams.WRAP_CONTENT, startDp = 4f)
            setOnClickListener { finish() }
        }
        bar.addView(close)
        return bar
    }

    private fun buildPathBar(): View {
        val row = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 2f), Ds.dp(context, 14f), Ds.dp(context, 8f))
        }
        upButton = toolButton("Up") { goUp() }
        row.addView(upButton)

        pathText = Ds.text(this, "/", 12.5f, Ds.accentSoft, mono = true).apply {
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.START
            setPadding(Ds.dp(context, 10f), Ds.dp(context, 9f), Ds.dp(context, 10f), Ds.dp(context, 9f))
            background = Ds.surface(context, Ds.surfaceSunken, Ds.borderSoft, Ds.radiusControl)
            layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 8f, endDp = 8f)
            setOnClickListener { showPathSheet() }
        }
        row.addView(pathText)

        refreshButton = toolButton("Refresh") { navigate(cwd) }
        row.addView(refreshButton)
        return row
    }

    private fun buildActionBar(): View {
        val row = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 0f), Ds.dp(context, 14f), Ds.dp(context, 8f))
        }
        newFolderButton = actionButton("New folder") { promptNewFolder() }
        row.addView(newFolderButton)
        sendHereButton = actionButton("Send file here") { pickPhoneFile() }
        sendHereButton.layoutParams = Ds.lp(this, weight = 1f, startDp = 8f)
        row.addView(sendHereButton)
        sortButton = actionButton("Sort: Name") { showSortSheet() }
        sortButton.layoutParams = Ds.lp(this, weight = 1f, startDp = 8f)
        row.addView(sortButton)
        newFolderButton.layoutParams = Ds.lp(this, weight = 1f)

        // Second row: which drive on the PC, and what has already been shared. Both are answers to
        // "the PC is not only C:" - the first reaches the other volumes, the second shows the trail.
        val row2 = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 0f), Ds.dp(context, 14f), Ds.dp(context, 8f))
        }
        drivesButton = actionButton("Drives") { showDrivesSheet() }
        drivesButton.layoutParams = Ds.lp(this, weight = 1f)
        row2.addView(drivesButton)
        recentButton = actionButton("Recent shared") { showRecentSheet() }
        recentButton.layoutParams = Ds.lp(this, weight = 1f, startDp = 8f)
        row2.addView(recentButton)

        val col = Ds.col(this)
        col.addView(row)
        col.addView(row2)
        return col
    }

    private fun buildColumnHeader(): View {
        val row = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 2f), Ds.dp(context, 14f), Ds.dp(context, 6f))
        }
        val badgeSpacer = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(Ds.dp(context, BADGE_DP), 1)
        }
        row.addView(badgeSpacer)

        headerName = columnHeader("Name", SortMode.NAME, 0f, true)
        row.addView(headerName)
        headerSize = columnHeader("Size", SortMode.SIZE, COL_SIZE_DP, false)
        row.addView(headerSize)
        headerDate = columnHeader("Modified", SortMode.DATE, COL_DATE_DP, false)
        row.addView(headerDate)
        return row
    }

    private fun columnHeader(label: String, mode: SortMode, widthDp: Float, flexible: Boolean): TextView {
        val tv = Ds.text(this, label, 10f, Ds.textMuted, bold = true, letterSpacing = 0.07f).apply {
            setPadding(Ds.dp(context, 4f), Ds.dp(context, 4f), Ds.dp(context, 4f), Ds.dp(context, 4f))
            if (!flexible) gravity = Gravity.END
            layoutParams = if (flexible) {
                Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 10f)
            } else {
                Ds.lp(this@PcBrowserActivity, width = Ds.dp(this@PcBrowserActivity, widthDp))
            }
            background = Ds.ripple(context, Ds.surface(context, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
            setOnClickListener { toggleSort(mode) }
        }
        return tv
    }

    private fun buildSelectionBar(): LinearLayout {
        val bar = Ds.row(this).apply {
            visibility = View.GONE
            setBackgroundColor(Ds.surfaceAlt)
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 10f), Ds.dp(context, 14f), Ds.dp(context, 10f))
        }
        selectionText = Ds.text(this, "0 selected", 12.5f, Ds.textPrimary, bold = true).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f)
        }
        bar.addView(selectionText)

        val save = miniAction("Save") { saveSelection() }
        bar.addView(save)
        val del = miniAction("Delete") { confirmDeleteSelection() }
        del.setTextColor(Ds.danger)
        bar.addView(del)
        val clear = miniAction("Clear") { exitSelection() }
        bar.addView(clear)
        return bar
    }

    private fun miniAction(label: String, action: () -> Unit): TextView =
        Ds.text(this, label, 12f, Ds.accentSoft, bold = true).apply {
            setPadding(Ds.dp(context, 12f), Ds.dp(context, 8f), Ds.dp(context, 12f), Ds.dp(context, 8f))
            background = Ds.ripple(context, Ds.surface(context, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
            layoutParams = Ds.lp(this@PcBrowserActivity, width = ViewGroup.LayoutParams.WRAP_CONTENT)
            setOnClickListener { action() }
        }

    private fun toolButton(label: String, action: () -> Unit): TextView =
        Ds.text(this, label, 12f, Ds.textSecondary, bold = true).apply {
            gravity = Gravity.CENTER
            setPadding(Ds.dp(context, 13f), Ds.dp(context, 9f), Ds.dp(context, 13f), Ds.dp(context, 9f))
            background = Ds.ripple(context, Ds.surface(context, Ds.surfaceAlt, Ds.border, Ds.radiusControl))
            layoutParams = Ds.lp(this@PcBrowserActivity, width = ViewGroup.LayoutParams.WRAP_CONTENT)
            setOnClickListener { action() }
        }

    private fun actionButton(label: String, action: () -> Unit): TextView =
        Ds.text(this, label, 12f, Ds.textPrimary, medium = true).apply {
            gravity = Gravity.CENTER
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(Ds.dp(context, 10f), Ds.dp(context, 11f), Ds.dp(context, 10f), Ds.dp(context, 11f))
            background = Ds.ripple(context, Ds.surface(context, Ds.surface, Ds.border, Ds.radiusControl))
            layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f)
            setOnClickListener { action() }
        }

    private fun buildBusyOverlay(): FrameLayout = FrameLayout(this).apply {
        visibility = View.GONE
        setBackgroundColor(0xB3000000.toInt())
        isClickable = true
        val card = Ds.col(this@PcBrowserActivity, 18f).apply {
            background = Ds.surface(context, Ds.surface, Ds.border, 18f)
            gravity = Gravity.CENTER
            layoutParams = Ds.frameLp(this@PcBrowserActivity, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER)
        }
        busyText = Ds.text(this@PcBrowserActivity, "Working…", 13f, Ds.textPrimary, medium = true).apply {
            gravity = Gravity.CENTER
            layoutParams = Ds.lp(this@PcBrowserActivity, width = ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        card.addView(busyText)
        addView(card)
    }

    private fun buildSheetOverlay(): FrameLayout = FrameLayout(this).apply {
        visibility = View.GONE
        setBackgroundColor(0xB3000000.toInt())
        setOnClickListener { hideSheet() }
        sheetCard = Ds.col(this@PcBrowserActivity).apply {
            background = Ds.surface(context, Ds.surface, Ds.border, 22f)
            setPadding(Ds.dp(context, 18f), Ds.dp(context, 18f), Ds.dp(context, 18f), Ds.dp(context, 14f))
            layoutParams = Ds.frameLp(this@PcBrowserActivity, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM)
            isClickable = true
        }
        addView(sheetCard)
    }

    private fun buildViewerOverlay(): FrameLayout = FrameLayout(this).apply {
        visibility = View.GONE
        setBackgroundColor(Ds.bg)
        isClickable = true
        val col = Ds.col(this@PcBrowserActivity)
        val bar = Ds.row(this@PcBrowserActivity).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 14f), Ds.dp(context, 14f), Ds.dp(context, 10f))
        }
        viewerTitle = Ds.text(this@PcBrowserActivity, "", 15f, Ds.textPrimary, bold = true).apply {
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.MIDDLE
            layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f)
        }
        bar.addView(viewerTitle)
        bar.addView(toolButton("Close") { hideViewer() })
        col.addView(bar)
        col.addView(Ds.divider(this@PcBrowserActivity, Ds.border))
        viewerBody = Ds.col(this@PcBrowserActivity).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, height = 0, weight = 1f)
        }
        col.addView(viewerBody)
        addView(col, Ds.frameLp(this@PcBrowserActivity, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    private fun buildPromptOverlay(): FrameLayout = FrameLayout(this).apply {
        visibility = View.GONE
        setBackgroundColor(0xB3000000.toInt())
        setOnClickListener { hidePrompt() }
    }

    // ---------------------------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------------------------

    private fun renderHeader() {
        headerSubtitle.text = when {
            target.ip.isNotBlank() -> target.label
            PcVfsClient.bluetoothReady(this) -> "Paired Bluetooth PC (no Wi-Fi needed)"
            else -> "No PC link yet"
        }
        val (label, color) = when (lastTransport) {
            PcVfsClient.Transport.LAN -> Pair("WI-FI", Ds.accent)
            PcVfsClient.Transport.BLUETOOTH -> Pair("BLUETOOTH", Ds.info)
            PcVfsClient.Transport.NONE -> Pair("NO LINK", Ds.textMuted)
        }
        transportPill.text = label
        transportPill.setTextColor(color)
        transportPill.background = Ds.surface(this, Ds.withAlpha(color), Color.TRANSPARENT, Ds.radiusPill, 0f)

        pathText.text = cwd
        upButton.alpha = if (cwd == "/") 0.4f else 1f
        sortButton.text = "Sort: ${sortMode.label}${if (sortAsc) " ↑" else " ↓"}"
        headerName.text = "NAME${if (sortMode == SortMode.NAME) (if (sortAsc) " ↑" else " ↓") else ""}"
        headerSize.text = "SIZE${if (sortMode == SortMode.SIZE) (if (sortAsc) " ↑" else " ↓") else ""}"
        headerDate.text = "MODIFIED${if (sortMode == SortMode.DATE) (if (sortAsc) " ↑" else " ↓") else ""}"
    }

    private fun renderList() {
        listContainer.removeAllViews()

        if (items.isEmpty()) {
            listContainer.addView(emptyCard())
            renderStatus()
            return
        }

        val shown = sortedItems()
        for (item in shown) {
            listContainer.addView(buildRow(item))
        }
        listContainer.addView(
            Ds.text(this, "${shown.size} item(s) listed  •  the PC serves at most 400 entries per folder", 10f, Ds.textMuted, mono = true).apply {
                setPadding(Ds.dp(context, 14f), Ds.dp(context, 12f), Ds.dp(context, 14f), Ds.dp(context, 18f))
                layoutParams = Ds.lp(this@PcBrowserActivity)
            }
        )
        renderStatus()
    }

    private fun renderStatus() {
        val problem = lastProblem
        if (problem != null) {
            statusText.text = problem
            statusText.setTextColor(Ds.amber)
            return
        }
        var folders = 0
        var bytes = 0L
        for (item in items) {
            if (item.isDirectory) folders++ else bytes += item.sizeBytes
        }
        val transport = when (lastTransport) {
            PcVfsClient.Transport.LAN -> "Wi-Fi ${target.ip}:${target.port}"
            PcVfsClient.Transport.BLUETOOTH -> "Bluetooth RFCOMM"
            PcVfsClient.Transport.NONE -> "no link"
        }
        statusText.text = "${items.size} item(s)  •  $folders folder(s)  •  ${PcVfsClient.humanSize(bytes)}  •  $transport  •  ${lastRttMs} ms"
        statusText.setTextColor(Ds.textMuted)
    }

    private fun sortedItems(): List<PcVfsClient.Item> {
        val comparator = object : Comparator<PcVfsClient.Item> {
            override fun compare(a: PcVfsClient.Item, b: PcVfsClient.Item): Int {
                if (foldersFirst && a.isDirectory != b.isDirectory) return if (a.isDirectory) -1 else 1
                val raw = when (sortMode) {
                    SortMode.NAME -> a.name.lowercase().compareTo(b.name.lowercase())
                    SortMode.SIZE -> {
                        if (a.sizeBytes == b.sizeBytes) a.name.lowercase().compareTo(b.name.lowercase())
                        else if (a.sizeBytes < b.sizeBytes) -1 else 1
                    }
                    SortMode.TYPE -> {
                        val byType = a.ext.lowercase().compareTo(b.ext.lowercase())
                        if (byType != 0) byType else a.name.lowercase().compareTo(b.name.lowercase())
                    }
                    SortMode.DATE -> {
                        if (a.modifiedMs == b.modifiedMs) a.name.lowercase().compareTo(b.name.lowercase())
                        else if (a.modifiedMs < b.modifiedMs) -1 else 1
                    }
                }
                val directed = if (sortAsc) raw else -raw
                return if (directed != 0) directed else a.name.compareTo(b.name)
            }
        }
        return items.sortedWith(comparator)
    }

    private fun buildRow(item: PcVfsClient.Item): View {
        val selected = selection.contains(item.path)
        val row = Ds.row(this).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 8f), Ds.dp(context, 14f), Ds.dp(context, 8f))
            minimumHeight = Ds.dp(context, 52f)
            val fill = if (selected) Ds.withAlpha(Ds.accent, 0x22) else Color.TRANSPARENT
            val stroke = if (selected) Ds.accent else Color.TRANSPARENT
            background = Ds.ripple(context, Ds.surface(context, fill, stroke, Ds.radiusControl, if (selected) 1f else 0f))
            isClickable = true
            isFocusable = true
        }

        val tint = PcVfsClient.tintFor(item)
        val badge = Ds.text(this, item.badge, 9.5f, tint, bold = true).apply {
            gravity = Gravity.CENTER
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            background = Ds.surface(context, Ds.withAlpha(tint, 0x1F), Color.TRANSPARENT, 11f, 0f)
            layoutParams = LinearLayout.LayoutParams(Ds.dp(context, BADGE_DP), Ds.dp(context, BADGE_DP))
        }
        row.addView(badge)

        val labels = Ds.col(this).apply { layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 10f) }
        labels.addView(
            Ds.text(this, item.name, 13.5f, if (selected) Ds.accentSoft else Ds.textPrimary, medium = true).apply {
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.MIDDLE
            }
        )
        labels.addView(
            Ds.text(this, typeLabel(item), 10.5f, Ds.textMuted).apply {
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
                setPadding(0, Ds.dp(context, 2f), 0, 0)
            }
        )
        row.addView(labels)

        row.addView(
            Ds.text(this, if (item.isDirectory) "—" else PcVfsClient.humanSize(item.sizeBytes), 11f, Ds.textSecondary, mono = true).apply {
                gravity = Gravity.END
                maxLines = 1
                layoutParams = Ds.lp(this@PcBrowserActivity, width = Ds.dp(this@PcBrowserActivity, COL_SIZE_DP))
            }
        )
        row.addView(
            Ds.text(this, PcVfsClient.humanTime(item.modifiedMs), 9.5f, Ds.textMuted, mono = true).apply {
                gravity = Gravity.END
                maxLines = 1
                layoutParams = Ds.lp(this@PcBrowserActivity, width = Ds.dp(this@PcBrowserActivity, COL_DATE_DP), startDp = 6f)
            }
        )

        row.setOnClickListener { onRowTap(item) }
        row.setOnLongClickListener {
            onRowLongPress(item)
            true
        }
        return row
    }

    private fun typeLabel(item: PcVfsClient.Item): String {
        if (item.isDirectory) return "File folder"
        val ext = item.ext.removePrefix(".").uppercase()
        val kind = when {
            PcVfsClient.isImage(item) -> "image"
            item.mimeType.startsWith("video") -> "video"
            item.mimeType.startsWith("audio") -> "audio"
            PcVfsClient.isTextLike(item) -> "document"
            else -> "file"
        }
        return if (ext.isBlank()) kind else "$ext $kind"
    }

    /** The state that used to send the user to a browser now explains itself and offers a way in. */
    private fun emptyCard(): View {
        val card = Ds.card(this, 18f, Ds.surface, Ds.border).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 12f, startDp = 14f, endDp = 14f)
        }
        val problem = lastProblem

        card.addView(Ds.sectionLabel(this, if (problem != null) "Cannot reach the PC" else "This folder is empty"))
        card.addView(
            Ds.text(
                this,
                problem ?: "Nothing to list yet. Create a folder on the PC, or send one from this phone.",
                13f,
                if (problem != null) Ds.amber else Ds.textSecondary
            ).apply {
                layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 10f)
                setLineSpacing(0f, 1.3f)
            }
        )

        if (problem != null) {
            val input = Ds.input(this, "PC IP address (optional)", target.ip).apply {
                layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 12f)
                inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI
            }
            card.addView(input)
            card.addView(
                Ds.text(
                    this,
                    "ChainIt on Windows shares your profile folder (Desktop, Documents, Downloads, Pictures) as the root. " +
                        "It answers on Wi-Fi, hotspot, USB tethering and Bluetooth — no browser is involved.",
                    11f,
                    Ds.textMuted
                ).apply {
                    layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 8f)
                    setLineSpacing(0f, 1.3f)
                }
            )
            card.addView(Ds.button(this, "Try again", Ds.ButtonVariant.PRIMARY) {
                val typed = input.text.toString().trim()
                target = PcVfsClient.resolveTarget(this, typed)
                renderHeader()
                navigate(cwd)
            }.apply { layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 12f) })

            val secondary = Ds.row(this)
            secondary.addView(
                Ds.button(this, "Bluetooth settings", Ds.ButtonVariant.SECONDARY) {
                    runCatching { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
                        .onFailure { toast("Open Settings → Bluetooth and pair with your PC once") }
                }.apply { layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f) }
            )
            secondary.addView(
                Ds.button(this, "Wi-Fi settings", Ds.ButtonVariant.GHOST) {
                    runCatching { startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) }
                        .onFailure { toast("Open Settings → Wi-Fi and join the same network as the PC") }
                }.apply { layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 8f) }
            )
            secondary.layoutParams = Ds.lp(this, topDp = 8f)
            card.addView(secondary)
        }
        return card
    }

    // ---------------------------------------------------------------------------------------------
    // Navigation
    // ---------------------------------------------------------------------------------------------

    private fun navigate(dir: String) {
        if (busy) return
        busy = true
        hideSheet()
        showBusy("Listing ${if (dir == "/") "the PC root" else dir}…")
        Thread {
            val listing = PcVfsClient.list(this, target, dir)
            ui.post {
                busy = false
                hideBusy()
                lastTransport = listing.transport
                lastRttMs = listing.rttMs
                lastProblem = listing.problem
                if (listing.drives.isNotEmpty()) pcDrives = listing.drives
                allowOtherDrives = listing.allowOtherDrives
                if (listing.ok) {
                    cwd = listing.currentPath.ifBlank { dir }
                    items = listing.items
                    selection.clear()
                    selecting = false
                    updateSelectionBar()
                    renderList()
                } else {
                    cwd = dir
                    items = emptyList()
                    renderList()
                }
                renderHeader()
            }
        }.start()
    }

    private fun goUp() {
        if (cwd == "/") {
            toast("Already at the PC's shared root (your Windows profile folder)")
            return
        }
        navigate(PcVfsClient.parentOf(cwd))
    }

    private fun toggleSort(mode: SortMode) {
        if (sortMode == mode) {
            sortAsc = !sortAsc
        } else {
            sortMode = mode
            sortAsc = true
        }
        renderHeader()
        renderList()
    }

    override fun onBackPressed() {
        when {
            viewerOverlay.visibility == View.VISIBLE -> hideViewer()
            promptOverlay.visibility == View.VISIBLE -> hidePrompt()
            sheetOverlay.visibility == View.VISIBLE -> hideSheet()
            selecting -> exitSelection()
            cwd != "/" -> goUp()
            else -> super.onBackPressed()
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Row interaction
    // ---------------------------------------------------------------------------------------------

    private fun onRowTap(item: PcVfsClient.Item) {
        if (selecting) {
            if (selection.contains(item.path)) selection.remove(item.path) else selection.add(item.path)
            updateSelectionBar()
            renderList()
            return
        }
        if (item.isDirectory) {
            navigate(item.path)
        } else {
            showFileSheet(item)
        }
    }

    private fun onRowLongPress(item: PcVfsClient.Item) {
        if (selecting) {
            if (selection.contains(item.path)) selection.remove(item.path) else selection.add(item.path)
            updateSelectionBar()
            renderList()
            return
        }
        if (item.isDirectory) showFolderSheet(item) else showFileSheet(item)
    }

    private fun updateSelectionBar() {
        if (selecting && selection.isNotEmpty()) {
            selectionBar.visibility = View.VISIBLE
            selectionText.text = "${selection.size} selected"
        } else {
            selectionBar.visibility = View.GONE
        }
    }

    private fun enterSelection(item: PcVfsClient.Item) {
        selecting = true
        selection.clear()
        selection.add(item.path)
        updateSelectionBar()
        renderList()
    }

    private fun exitSelection() {
        selecting = false
        selection.clear()
        updateSelectionBar()
        renderList()
    }

    // ---------------------------------------------------------------------------------------------
    // Sheets
    // ---------------------------------------------------------------------------------------------

    private fun showSheet(title: String, subtitle: String, actions: List<SheetAction>, extra: View? = null) {
        sheetCard.removeAllViews()
        sheetCard.addView(Ds.text(this, title, 15.5f, Ds.textPrimary, bold = true).apply {
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.MIDDLE
        })
        sheetCard.addView(Ds.text(this, subtitle, 11f, Ds.textMuted, mono = true).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 4f)
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.START
        })
        if (extra != null) sheetCard.addView(extra)
        sheetCard.addView(Ds.divider(this).apply {
            (layoutParams as LinearLayout.LayoutParams).topMargin = Ds.dp(this@PcBrowserActivity, 12f)
            (layoutParams as LinearLayout.LayoutParams).bottomMargin = Ds.dp(this@PcBrowserActivity, 6f)
        })
        for (action in actions) {
            sheetCard.addView(
                Ds.text(this, action.label, 13.5f, action.color, medium = true).apply {
                    setPadding(Ds.dp(context, 10f), Ds.dp(context, 13f), Ds.dp(context, 10f), Ds.dp(context, 13f))
                    background = Ds.ripple(context, Ds.surface(context, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
                    layoutParams = Ds.lp(this@PcBrowserActivity)
                    setOnClickListener {
                        hideSheet()
                        action.run()
                    }
                }
            )
        }
        sheetCard.addView(
            Ds.text(this, "Cancel", 13f, Ds.textMuted, bold = true).apply {
                gravity = Gravity.CENTER
                setPadding(Ds.dp(context, 10f), Ds.dp(context, 13f), Ds.dp(context, 10f), Ds.dp(context, 13f))
                background = Ds.ripple(context, Ds.surface(context, Color.TRANSPARENT, Color.TRANSPARENT, Ds.radiusControl, 0f))
                layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 4f)
                setOnClickListener { hideSheet() }
            }
        )
        sheetOverlay.visibility = View.VISIBLE
    }

    private fun hideSheet() {
        sheetOverlay.visibility = View.GONE
    }

    private fun showFileSheet(item: PcVfsClient.Item) {
        val actions = ArrayList<SheetAction>()
        if (PcVfsClient.isTextLike(item)) {
            actions.add(SheetAction("Open in ChainIt", Ds.accentSoft) { openText(item) })
        }
        if (PcVfsClient.isImage(item)) {
            actions.add(SheetAction("Open in ChainIt", Ds.accentSoft) { openImage(item) })
        }
        actions.add(SheetAction("Save to phone (Downloads)", Ds.textPrimary) { saveToPhone(item) })
        actions.add(SheetAction("Open with another app", Ds.textPrimary) { openWithAnotherApp(item) })
        actions.add(SheetAction("Select", Ds.textPrimary) { enterSelection(item) })
        actions.add(SheetAction("Rename…", Ds.textPrimary) { promptRename(item) })
        actions.add(SheetAction("Delete on the PC", Ds.danger) { confirmDelete(listOf(item.path), item.name) })
        actions.add(SheetAction("Details", Ds.textSecondary) { showDetails(item) })
        showSheet(item.name, "${item.path}  •  ${PcVfsClient.humanSize(item.sizeBytes)}", actions)
    }

    private fun showFolderSheet(item: PcVfsClient.Item) {
        val actions = ArrayList<SheetAction>()
        actions.add(SheetAction("Open", Ds.accentSoft) { navigate(item.path) })
        actions.add(SheetAction("New folder inside…", Ds.textPrimary) { promptNewFolderIn(item.path) })
        actions.add(SheetAction("Send a phone file here", Ds.textPrimary) { pickPhoneFileFor(item.path) })
        actions.add(SheetAction("Select", Ds.textPrimary) { enterSelection(item) })
        actions.add(SheetAction("Rename…", Ds.textPrimary) { promptRename(item) })
        actions.add(SheetAction("Delete on the PC", Ds.danger) { confirmDelete(listOf(item.path), item.name) })
        actions.add(SheetAction("Details", Ds.textSecondary) { showDetails(item) })
        showSheet(item.name, item.path, actions)
    }

    private fun showPathSheet() {
        val actions = ArrayList<SheetAction>()
        actions.add(SheetAction("Go to the PC root (/)", Ds.accentSoft) { navigate("/") })
        for (quick in listOf("Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos")) {
            actions.add(SheetAction("Go to /$quick", Ds.textPrimary) { navigate("/$quick") })
        }
        actions.add(SheetAction(if (foldersFirst) "Folders first: on" else "Folders first: off", Ds.textSecondary) {
            foldersFirst = !foldersFirst
            renderList()
        })
        showSheet(cwd, "The PC shares your Windows profile folder as /", actions)
    }

    /**
     * The PC's volumes, with real capacity, and the switch that decides whether "/" may name one.
     *
     * The switch belongs to the PC (it is persisted there and shown on its Link tab); changing it from
     * the phone is offered because the phone is usually the reason somebody walks over to a PC.
     */
    private fun showDrivesSheet() {
        val actions = ArrayList<SheetAction>()
        val here = if (cwd.startsWith("/") && cwd.length > 2 && cwd[2] == ':') cwd.substring(0, 3) else "/"
        for (d in pcDrives) {
            val current = (d.virtualPath == here) || (d.virtualPath != "/" && cwd.startsWith(d.virtualPath))
            val blocked = !allowOtherDrives && !d.isPcRoot
            actions.add(
                SheetAction(
                    (if (current) "\u2713 " else "") + d.caption + (if (blocked) "  \u2014 blocked on the PC" else ""),
                    if (current) Ds.accentSoft else if (blocked) Ds.textMuted else Ds.textPrimary
                ) {
                    if (blocked) {
                        toast("The PC only shares its profile folder right now. Allow other drives below, then try again.")
                    } else if (!d.ready) {
                        toast("${d.letter}: has no media, or the PC could not read it")
                    } else {
                        navigate(d.virtualPath)
                    }
                }
            )
        }
        if (pcDrives.isEmpty()) {
            actions.add(SheetAction("Ask the PC which drives it has", Ds.accentSoft) { refreshDrives() })
        }
        actions.add(
            SheetAction(
                if (allowOtherDrives) "Block other drives on the PC" else "Allow other drives on the PC",
                Ds.textSecondary
            ) { toggleDriveScope() }
        )
        actions.add(SheetAction("Refresh the drive list", Ds.textSecondary) { refreshDrives() })
        val sub = if (pcDrives.isEmpty()) "No volume list from the PC yet"
        else "${pcDrives.size} volume(s)  \u2022  / is the PC's profile folder on ${pcDrives.firstOrNull { it.isPcRoot }?.letter ?: "?"}:"
        showSheet("Drives on the PC", sub, actions)
    }

    private fun refreshDrives() {
        if (busy) return
        busy = true
        showBusy("Asking the PC which drives it has\u2026")
        Thread {
            val pair = PcVfsClient.drives(this, target)
            ui.post {
                busy = false
                hideBusy()
                if (pair.first.isEmpty()) {
                    toast("The PC did not report any volumes over this link")
                } else {
                    pcDrives = pair.first
                    allowOtherDrives = pair.second
                    toast("${pair.first.size} volume(s) reported by the PC")
                    showDrivesSheet()
                }
            }
        }.start()
    }

    private fun toggleDriveScope() {
        if (busy) return
        val want = !allowOtherDrives
        busy = true
        showBusy(if (want) "Asking the PC to allow other drives\u2026" else "Asking the PC to block other drives\u2026")
        Thread {
            val reply = PcVfsClient.setDriveScope(this, target, want)
            ui.post {
                busy = false
                hideBusy()
                if (reply.ok) {
                    allowOtherDrives = want
                    toast(
                        if (want) "The PC now accepts /D:, /E: and removable drives"
                        else "The PC only shares its profile folder again"
                    )
                    navigate(cwd)
                } else {
                    toast(reply.message)
                }
            }
        }.start()
    }

    /**
     * What has already moved between this phone and the PC, newest first, read from the PC's own
     * persisted history - so shares made while this phone's screen was off, or from the PC's File
     * Manager, are here too.
     */
    private fun showRecentSheet() {
        if (busy) return
        busy = true
        showBusy("Reading the PC's shared-file history\u2026")
        Thread {
            val result = PcVfsClient.history(this, target, 24)
            ui.post {
                busy = false
                hideBusy()
                if (!result.ok) {
                    toast(result.message)
                    return@post
                }
                recentEntries = result.entries
                if (recentEntries.isEmpty()) {
                    showSheet(
                        "Recent shared files",
                        "Nothing has moved between this phone and the PC yet",
                        listOf(SheetAction("Go to the PC root", Ds.accentSoft) { navigate("/") })
                    )
                    return@post
                }
                val actions = ArrayList<SheetAction>()
                for (e in recentEntries) {
                    val arrow = if (e.toPhone) "\u2191 to this phone" else "\u2193 to the PC"
                    val when_ = PcVfsClient.humanTime(e.timestampMs)
                    actions.add(
                        SheetAction("${e.label}  \u2022  ${PcVfsClient.humanSize(e.bytes)}  \u2022  $arrow  \u2022  $when_", Ds.textPrimary) {
                            showRecentEntrySheet(e)
                        }
                    )
                }
                showSheet(
                    "Recent shared files",
                    "${recentEntries.size} newest of the PC's history  \u2022  kept on the PC across restarts",
                    actions
                )
            }
        }.start()
    }

    private fun showRecentEntrySheet(e: PcVfsClient.HistoryEntry) {
        val actions = ArrayList<SheetAction>()
        val who = e.who
        val sub = buildString {
            append(e.verb).append(' ').append(if (e.toPhone) "to this phone" else "to the PC")
            if (e.transportUsed.isNotBlank()) append("  \u2022  ").append(e.transportUsed)
            if (who.isNotBlank()) append("  \u2022  ").append(who)
            append("  \u2022  ").append(PcVfsClient.humanTime(e.timestampMs))
        }
        if (e.fetchable) {
            actions.add(SheetAction("Save to phone again (Downloads)", Ds.accentSoft) { refetch(e) })
            actions.add(SheetAction("Show it on the PC (Explorer)", Ds.textPrimary) { revealOnPc(e) })
            actions.add(SheetAction("Open its PC folder here", Ds.textPrimary) {
                navigate(PcVfsClient.parentOf(e.virtualPath))
            })
        } else {
            actions.add(SheetAction("Details", Ds.textPrimary) { showText(e.label, sub) })
        }
        showSheet(e.label, sub, actions)
    }

    /** Pulls a file from the PC's history again - the entry's own virtual path, not a Windows one. */
    private fun refetch(e: PcVfsClient.HistoryEntry) {
        val item = PcVfsClient.Item(
            name = e.label,
            path = e.virtualPath,
            isDirectory = false,
            sizeBytes = e.bytes,
            modifiedMs = e.timestampMs,
            mimeType = "",
            ext = if (e.label.contains('.')) e.label.substringAfterLast('.', "") else ""
        )
        saveToPhone(item)
    }

    private fun revealOnPc(e: PcVfsClient.HistoryEntry) {
        if (busy) return
        busy = true
        showBusy("Asking the PC to open that folder\u2026")
        Thread {
            val reply = PcVfsClient.reveal(this, target, e.virtualPath)
            ui.post {
                busy = false
                hideBusy()
                toast(
                    if (reply.ok) "The PC opened Explorer at ${e.virtualPath}"
                    else reply.message
                )
            }
        }.start()
    }

    private fun showSortSheet() {
        val actions = ArrayList<SheetAction>()
        for (mode in SortMode.values()) {
            val current = sortMode == mode
            actions.add(
                SheetAction(
                    "${mode.label}${if (current) (if (sortAsc) "  (A→Z, small→large)" else "  (Z→A, large→small)") else ""}",
                    if (current) Ds.accentSoft else Ds.textPrimary
                ) {
                    if (current) sortAsc = !sortAsc else {
                        sortMode = mode
                        sortAsc = true
                    }
                    renderHeader()
                    renderList()
                }
            )
        }
        actions.add(SheetAction(if (foldersFirst) "Folders first: on" else "Folders first: off", Ds.textSecondary) {
            foldersFirst = !foldersFirst
            renderList()
        })
        showSheet("Sort and order", "${items.size} item(s) in $cwd", actions)
    }

    private fun showDetails(item: PcVfsClient.Item) {
        val lines = StringBuilder()
        lines.append("Name:      ").append(item.name).append('\n')
        lines.append("On PC:     ").append(item.path).append('\n')
        lines.append("Type:      ").append(typeLabel(item)).append('\n')
        if (!item.isDirectory) {
            lines.append("Size:      ").append(PcVfsClient.humanSize(item.sizeBytes)).append(" (").append(item.sizeBytes).append(" bytes)\n")
        }
        lines.append("Modified:  ").append(PcVfsClient.humanTime(item.modifiedMs)).append('\n')
        if (item.mimeType.isNotBlank()) lines.append("MIME:      ").append(item.mimeType).append('\n')
        lines.append("PC:        ").append(target.label).append('\n')
        lines.append("Link:      ").append(lastTransport.label).append("  •  ").append(lastRttMs).append(" ms")
        val detail = lines.toString()

        val block = Ds.text(this, detail, 11f, Ds.textSecondary, mono = true).apply {
            setPadding(Ds.dp(context, 12f), Ds.dp(context, 10f), Ds.dp(context, 12f), Ds.dp(context, 10f))
            background = Ds.surface(context, Ds.surfaceSunken, Ds.borderSoft, 14f)
            layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 10f)
            setTextIsSelectable(true)
        }
        val actions = ArrayList<SheetAction>()
        actions.add(SheetAction("Copy details", Ds.accentSoft) {
            val clip = android.content.ClipData.newPlainText("ChainIt file details", detail)
            runCatching {
                (getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(clip)
                toast("Details copied to the clipboard")
            }.onFailure { toast("This phone refused the clipboard write") }
        })
        showSheet(item.name, "Details", actions, block)
    }

    private fun confirmDelete(paths: List<String>, label: String) {
        val actions = ArrayList<SheetAction>()
        actions.add(SheetAction("Delete permanently on the PC", Ds.danger) {
            runOnPc("Deleting $label…") { PcVfsClient.delete(this, target, paths) }
        })
        actions.add(SheetAction("Keep it", Ds.textSecondary) {})
        val note = if (paths.size > 1) "${paths.size} items" else label
        showSheet("Delete $note?", "This removes it from the PC. There is no Recycle Bin round trip.", actions)
    }

    private fun confirmDeleteSelection() {
        if (selection.isEmpty()) return
        val paths = ArrayList<String>()
        for (p in selection) paths.add(p)
        confirmDelete(paths, "${paths.size} item(s)")
    }

    // ---------------------------------------------------------------------------------------------
    // Prompts (native, in-memory dialogs — the phone's version of the Windows prompt dialog)
    // ---------------------------------------------------------------------------------------------

    private fun promptNewFolder() = promptNewFolderIn(cwd)

    private fun promptNewFolderIn(dir: String) {
        showPrompt("New folder on the PC", "Folder name", "New folder") { value ->
            val name = FileOps.stripIllegal(value)
            if (name.isEmpty()) {
                toast("A folder name cannot be empty or use < > : \" / \\ | ? *")
            } else {
                runOnPc("Creating $name…") { PcVfsClient.mkdir(this, target, dir, name) }
            }
        }
    }

    private fun promptRename(item: PcVfsClient.Item) {
        showPrompt("Rename on the PC", "New name", item.name) { value ->
            val name = FileOps.stripIllegal(value)
            if (name.isEmpty() || name == item.name) {
                toast("Nothing to rename")
            } else {
                runOnPc("Renaming to $name…") { PcVfsClient.rename(this, target, item.path, name) }
            }
        }
    }

    private fun showPrompt(title: String, hint: String, initial: String, onOk: (String) -> Unit) {
        promptOverlay.removeAllViews()
        val card = Ds.col(this, 18f).apply {
            background = Ds.surface(context, Ds.surface, Ds.border, 20f)
            layoutParams = Ds.frameLp(this@PcBrowserActivity, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER)
            isClickable = true
        }
        val wrapper = FrameLayout(this).apply {
            setPadding(Ds.dp(context, 22f), 0, Ds.dp(context, 22f), 0)
            layoutParams = Ds.frameLp(this@PcBrowserActivity, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER)
        }
        card.addView(Ds.text(this, title, 15.5f, Ds.textPrimary, bold = true))
        val input = Ds.input(this, hint, initial).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, topDp = 12f)
            setSelectAllOnFocus(true)
        }
        card.addView(input)
        val buttons = Ds.row(this)
        buttons.addView(
            Ds.button(this, "Cancel", Ds.ButtonVariant.GHOST) { hidePrompt() }.apply {
                layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f)
            }
        )
        buttons.addView(
            Ds.button(this, "OK", Ds.ButtonVariant.PRIMARY) {
                val value = input.text.toString()
                hidePrompt()
                onOk(value)
            }.apply { layoutParams = Ds.lp(this@PcBrowserActivity, weight = 1f, startDp = 8f) }
        )
        buttons.layoutParams = Ds.lp(this, topDp = 14f)
        card.addView(buttons)
        wrapper.addView(card)
        promptOverlay.addView(wrapper, Ds.frameLp(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        promptOverlay.visibility = View.VISIBLE
        ui.postDelayed({
            runCatching {
                input.requestFocus()
                val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
                imm.showSoftInput(input, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT)
            }
        }, 90L)
    }

    private fun hidePrompt() {
        promptOverlay.visibility = View.GONE
        runCatching {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
            imm.hideSoftInputFromWindow(promptOverlay.windowToken, 0)
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Work against the PC
    // ---------------------------------------------------------------------------------------------

    private fun runOnPc(progressLabel: String, call: () -> PcVfsClient.Reply) {
        if (busy) return
        busy = true
        showBusy(progressLabel)
        Thread {
            val reply = runCatching { call() }.getOrElse { t ->
                PcVfsClient.Reply(0, ByteArray(0), PcVfsClient.Transport.NONE, 0L, "The request failed: ${t.javaClass.simpleName}")
            }
            ui.post {
                busy = false
                hideBusy()
                lastTransport = reply.transport
                lastRttMs = reply.rttMs
                if (reply.ok) {
                    lastProblem = null
                    navigate(cwd)
                } else {
                    lastProblem = reply.message
                    toast(reply.message)
                    renderStatus()
                    renderHeader()
                }
            }
        }.start()
    }

    private fun saveToPhone(item: PcVfsClient.Item) {
        saveItemsToPhone(listOf(item), downloadRoot())
    }

    private fun saveSelection() {
        val chosen = ArrayList<PcVfsClient.Item>()
        for (item in items) if (selection.contains(item.path)) chosen.add(item)
        if (chosen.isEmpty()) return
        val files = ArrayList<PcVfsClient.Item>()
        val skipped = ArrayList<String>()
        for (item in chosen) {
            if (item.isDirectory) skipped.add(item.name) else files.add(item)
        }
        if (skipped.isNotEmpty()) {
            toast("Folders cannot be downloaded yet — the PC serves single files: skipped ${skipped.joinToString(", ")}")
        }
        if (files.isEmpty()) return
        saveItemsToPhone(files, downloadRoot())
    }

    private fun saveItemsToPhone(targets: List<PcVfsClient.Item>, root: File) {
        if (busy) return
        busy = true
        showBusy(if (targets.size == 1) "Saving ${targets[0].name}…" else "Saving ${targets.size} files…")
        Thread {
            var saved = 0
            var failed = 0
            var lastMessage = ""
            var lastDest: File? = null
            for (item in targets) {
                val dest = uniqueFile(File(root, FileOps.sanitizeName(item.name)))
                val result = PcVfsClient.download(this, target, item.path, dest) { bytes ->
                    val now = System.currentTimeMillis()
                    if (now - lastProgressPostMs > 180L) {
                        lastProgressPostMs = now
                        ui.post { busyText.text = "Saving ${item.name} — ${PcVfsClient.humanSize(bytes)}" }
                    }
                }
                if (result.ok) {
                    saved++
                    lastDest = dest
                    MediaScannerConnection.scanFile(this, arrayOf(dest.absolutePath), null, null)
                } else {
                    failed++
                    lastMessage = result.message
                    runCatching { dest.delete() }
                }
            }
            val destPath = lastDest?.absolutePath ?: root.absolutePath
            ui.post {
                busy = false
                hideBusy()
                if (failed == 0) {
                    lastProblem = null
                    toast(if (saved == 1) "Saved to $destPath" else "Saved $saved files to ${root.absolutePath}")
                    statusText.text = "Saved $saved file(s) to ${root.absolutePath}"
                    statusText.setTextColor(Ds.accentSoft)
                } else {
                    lastProblem = lastMessage
                    toast(lastMessage)
                    renderStatus()
                }
            }
        }.start()
    }

    private fun openWithAnotherApp(item: PcVfsClient.Item) {
        val root = downloadRoot()
        val dest = uniqueFile(File(root, FileOps.sanitizeName(item.name)))
        if (busy) return
        busy = true
        showBusy("Saving ${item.name} so another app can open it…")
        Thread {
            val result = PcVfsClient.download(this, target, item.path, dest) { bytes ->
                val now = System.currentTimeMillis()
                if (now - lastProgressPostMs > 180L) {
                    lastProgressPostMs = now
                    ui.post { busyText.text = "Saving ${item.name} — ${PcVfsClient.humanSize(bytes)}" }
                }
            }
            ui.post {
                busy = false
                hideBusy()
                if (!result.ok) {
                    runCatching { dest.delete() }
                    lastProblem = result.message
                    toast(result.message)
                    renderStatus()
                    return@post
                }
                lastProblem = null
                MediaScannerConnection.scanFile(this, arrayOf(dest.absolutePath), null, null)
                val uri = ChainItFileProvider.uriFor(dest)
                val view = Intent(Intent.ACTION_VIEW).apply {
                    setDataAndType(uri, ChainItFileProvider.mimeFor(dest.name))
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                val chooser = Intent.createChooser(view, "Open ${dest.name} with")
                val started = runCatching { startActivity(chooser) }.isSuccess
                toast(
                    if (started) "Saved to ${dest.absolutePath} and handed to another app"
                    else "Saved to ${dest.absolutePath} — no app on this phone opens that type"
                )
            }
        }.start()
    }

    // ---------------------------------------------------------------------------------------------
    // In-app viewers: text and images are drawn by ChainIt, not by a browser
    // ---------------------------------------------------------------------------------------------

    private fun openText(item: PcVfsClient.Item) {
        if (busy) return
        busy = true
        showBusy("Reading ${item.name} from the PC…")
        Thread {
            val reply = PcVfsClient.readText(this, target, item.path)
            ui.post {
                busy = false
                hideBusy()
                lastTransport = reply.transport
                lastRttMs = reply.rttMs
                if (!reply.ok) {
                    lastProblem = reply.message
                    toast(reply.message)
                    renderStatus()
                    return@post
                }
                lastProblem = null
                var content = ""
                var editable = false
                var reason = "The PC did not explain why."
                runCatching {
                    val json = JSONObject(reply.text)
                    editable = json.optBoolean("editable", false)
                    if (json.isNull("content")) {
                        reason = if (editable) "The PC sent no text for this file." else
                            "The PC only serves editable text up to 2 MB inline. Save this file to the phone to read it."
                    } else {
                        content = json.optString("content", "")
                    }
                }
                if (!editable || content.isEmpty()) {
                    showText(item.name, if (content.isEmpty()) reason else content)
                } else {
                    showText(item.name, content)
                }
            }
        }.start()
    }

    private fun showText(title: String, body: String) {
        viewerBody.removeAllViews()
        val scroll = ScrollView(this).apply {
            layoutParams = Ds.lp(this@PcBrowserActivity, height = ViewGroup.LayoutParams.MATCH_PARENT)
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 12f), Ds.dp(context, 14f), Ds.dp(context, 20f))
        }
        scroll.addView(
            Ds.text(this, body, 12.5f, Ds.textSecondary, mono = true).apply {
                setTextIsSelectable(true)
                setLineSpacing(0f, 1.3f)
            }
        )
        viewerBody.addView(scroll)
        viewerTitle.text = title
        viewerOverlay.visibility = View.VISIBLE
    }

    private fun openImage(item: PcVfsClient.Item) {
        val cache = File(cacheDir, "pc-images")
        cache.mkdirs()
        val dest = uniqueFile(File(cache, FileOps.sanitizeName(item.name)))
        if (busy) return
        busy = true
        showBusy("Loading ${item.name}…")
        Thread {
            val result = PcVfsClient.download(this, target, item.path, dest) { bytes ->
                val now = System.currentTimeMillis()
                if (now - lastProgressPostMs > 180L) {
                    lastProgressPostMs = now
                    ui.post { busyText.text = "Loading ${item.name} — ${PcVfsClient.humanSize(bytes)}" }
                }
            }
            ui.post {
                busy = false
                hideBusy()
                if (!result.ok) {
                    runCatching { dest.delete() }
                    lastProblem = result.message
                    toast(result.message)
                    renderStatus()
                    return@post
                }
                lastProblem = null
                lastTransport = result.transport
                showImage(item.name, dest)
            }
        }.start()
    }

    private fun showImage(title: String, file: File) {
        val options = BitmapFactory.Options()
        options.inJustDecodeBounds = true
        runCatching { BitmapFactory.decodeFile(file.absolutePath, options) }
        var sample = 1
        while (options.outWidth / (sample * 2) >= 1080 && sample < 32) sample *= 2

        val decode = BitmapFactory.Options()
        decode.inSampleSize = sample
        val bitmap = runCatching { BitmapFactory.decodeFile(file.absolutePath, decode) }.getOrNull()

        viewerBody.removeAllViews()
        if (bitmap == null) {
            viewerBody.addView(
                Ds.text(this, "Saved to ${file.absolutePath}, but this phone could not decode it as an image.", 12.5f, Ds.amber).apply {
                    setPadding(Ds.dp(context, 16f), Ds.dp(context, 16f), Ds.dp(context, 16f), 0)
                    setLineSpacing(0f, 1.3f)
                }
            )
        } else {
            val view = ImageView(this).apply {
                setImageBitmap(bitmap)
                scaleType = ImageView.ScaleType.FIT_CENTER
                adjustViewBounds = true
                layoutParams = Ds.lp(this@PcBrowserActivity, height = ViewGroup.LayoutParams.MATCH_PARENT)
            }
            viewerBody.addView(view)
        }
        val footer = Ds.text(this, "${title}  •  ${PcVfsClient.humanSize(file.length())}  •  cached at ${file.absolutePath}", 10f, Ds.textMuted, mono = true).apply {
            setPadding(Ds.dp(context, 14f), Ds.dp(context, 8f), Ds.dp(context, 14f), Ds.dp(context, 12f))
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.START
        }
        viewerBody.addView(footer)
        viewerTitle.text = title
        viewerOverlay.visibility = View.VISIBLE
    }

    private fun hideViewer() {
        viewerOverlay.visibility = View.GONE
        viewerBody.removeAllViews()
    }

    // ---------------------------------------------------------------------------------------------
    // Phone → PC into the folder being browsed
    // ---------------------------------------------------------------------------------------------

    private var sendTargetDir: String = "/"

    private fun pickPhoneFile() = pickPhoneFileFor(cwd)

    private fun pickPhoneFileFor(dir: String) {
        sendTargetDir = dir
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        val ok = runCatching { startActivityForResult(intent, REQ_SEND_HERE) }.isSuccess
        if (!ok) toast("No file picker available on this device")
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_SEND_HERE || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        var fileName = "phone_file_${System.currentTimeMillis()}"
        runCatching {
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) fileName = cursor.getString(idx) ?: fileName
            }
        }
        uploadToPc(uri, fileName, sendTargetDir)
    }

    private fun uploadToPc(uri: Uri, fileName: String, dir: String) {
        if (busy) return
        busy = true
        showBusy("Sending $fileName to $dir…")
        Thread {
            val reply = PcVfsClient.uploadUri(this, target, uri, fileName, dir) { sent, total ->
                val now = System.currentTimeMillis()
                if (now - lastProgressPostMs > 180L) {
                    lastProgressPostMs = now
                    val label = if (total > 0) {
                        "Sending $fileName — ${PcVfsClient.humanSize(sent)} of ${PcVfsClient.humanSize(total)}"
                    } else {
                        "Sending $fileName — ${PcVfsClient.humanSize(sent)}"
                    }
                    ui.post { busyText.text = label }
                }
            }
            ui.post {
                busy = false
                hideBusy()
                lastTransport = reply.transport
                lastRttMs = reply.rttMs
                if (reply.ok) {
                    lastProblem = null
                    toast("Sent $fileName to $dir on the PC")
                    navigate(cwd)
                } else {
                    lastProblem = reply.message
                    toast(reply.message)
                    renderStatus()
                    renderHeader()
                }
            }
        }.start()
    }

    // ---------------------------------------------------------------------------------------------
    // Storage helpers
    // ---------------------------------------------------------------------------------------------

    /** Public Downloads when the app may write there, otherwise app-private storage — and we say which. */
    private fun downloadRoot(): File {
        if (canWritePublicStorage()) {
            val public = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "ChainIt")
            runCatching { public.mkdirs() }
            if (public.isDirectory) return public
        }
        val base = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: filesDir
        val private = File(base, "ChainIt")
        runCatching { private.mkdirs() }
        return private
    }

    private fun canWritePublicStorage(): Boolean = runCatching {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED
    }.getOrDefault(false)

    private fun uniqueFile(candidate: File): File {
        if (!candidate.exists()) return candidate
        val parent = candidate.parentFile
        val dot = candidate.name.lastIndexOf('.')
        val stem = if (dot > 0) candidate.name.substring(0, dot) else candidate.name
        val ext = if (dot > 0) candidate.name.substring(dot) else ""
        var index = 1
        while (index < 1000) {
            val next = File(parent, "$stem ($index)$ext")
            if (!next.exists()) return next
            index++
        }
        return File(parent, "$stem ${System.currentTimeMillis()}$ext")
    }

    // ---------------------------------------------------------------------------------------------
    // Small UI plumbing
    // ---------------------------------------------------------------------------------------------

    private fun showBusy(label: String) {
        busyText.text = label
        busyOverlay.visibility = View.VISIBLE
    }

    private fun hideBusy() {
        busyOverlay.visibility = View.GONE
    }

    private fun toast(message: String) {
        runCatching { Toast.makeText(this, message, Toast.LENGTH_LONG).show() }
    }
}
