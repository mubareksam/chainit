package com.chainit.android.ui

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.content.res.ColorStateList
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.chainit.android.R

/**
 * ChainIt design tokens + tiny view factories.
 * Everything is built programmatically (no XML layouts, no AppCompat/androidx) so the APK stays
 * a single self-contained artifact while still looking like a modern Material-ish dark UI:
 * generous spacing, soft surfaces, 1dp hairline borders, rounded controls and quiet accents.
 */
object Ds {
    // --- Palette (matches the Windows / web app exactly) ---
    val bg = Color.parseColor("#070B12")
    val surface = Color.parseColor("#0E1420")
    val surfaceAlt = Color.parseColor("#131B29")
    val surfaceSunken = Color.parseColor("#0A0F19")
    val border = Color.parseColor("#1E293B")
    val borderSoft = Color.parseColor("#18202F")

    val textPrimary = Color.parseColor("#F1F5F9")
    val textSecondary = Color.parseColor("#9AA7BA")
    val textMuted = Color.parseColor("#64748B")

    val accent = Color.parseColor("#10B981")      // emerald - primary brand
    val accentSoft = Color.parseColor("#6EE7B7")
    val info = Color.parseColor("#22D3EE")        // cyan - links / bluetooth
    val blue = Color.parseColor("#3B82F6")
    val amber = Color.parseColor("#F59E0B")
    val danger = Color.parseColor("#F43F5E")

    val onAccent = Color.parseColor("#04120C")

    // --- Metrics ---
    const val radiusCard = 22f
    const val radiusControl = 14f
    const val radiusPill = 999f

    fun dp(ctx: Context, v: Float): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, ctx.resources.displayMetrics).toInt()

    fun sp(ctx: Context, v: Float): Float = v

    // --- Surfaces ---
    fun surface(ctx: Context, fill: Int = surface, stroke: Int = border, radius: Float = radiusCard, strokeWidth: Float = 1f): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(ctx, radius).toFloat()
            setColor(fill)
            if (strokeWidth > 0f) setStroke(dp(ctx, strokeWidth), stroke)
        }

    fun ripple(ctx: Context, bg: GradientDrawable): RippleDrawable {
        val mask = ColorStateList.valueOf(Color.parseColor("#33FFFFFF"))
        return RippleDrawable(mask, bg, bg)
    }

    // --- Text ---
    fun text(
        ctx: Context,
        value: CharSequence,
        sizeSp: Float = 14f,
        color: Int = textPrimary,
        bold: Boolean = false,
        medium: Boolean = false,
        mono: Boolean = false,
        letterSpacing: Float = 0f
    ): TextView = TextView(ctx).apply {
        text = value
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
        setTextColor(color)
        typeface = when {
            mono -> Typeface.MONOSPACE
            bold -> Typeface.create("sans-serif-medium", Typeface.BOLD)
            medium -> Typeface.create("sans-serif-medium", Typeface.NORMAL)
            else -> Typeface.create("sans-serif", Typeface.NORMAL)
        }
        if (letterSpacing != 0f) setLetterSpacing(letterSpacing)
        includeFontPadding = false
    }

    fun sectionLabel(ctx: Context, value: String): TextView =
        text(ctx, value.uppercase(), 11f, textMuted, bold = true, letterSpacing = 0.08f)

    // --- Layout helpers ---
    fun col(ctx: Context, padDp: Float = 0f): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        if (padDp > 0f) setPadding(dp(ctx, padDp), dp(ctx, padDp), dp(ctx, padDp), dp(ctx, padDp))
    }

    fun row(ctx: Context, gravity: Int = Gravity.CENTER_VERTICAL): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        this.gravity = gravity
    }

    fun lp(
        ctx: Context,
        width: Int = ViewGroup.LayoutParams.MATCH_PARENT,
        height: Int = ViewGroup.LayoutParams.WRAP_CONTENT,
        topDp: Float = 0f,
        bottomDp: Float = 0f,
        startDp: Float = 0f,
        endDp: Float = 0f,
        weight: Float = 0f
    ): LinearLayout.LayoutParams = LinearLayout.LayoutParams(width, height).apply {
        topMargin = dp(ctx, topDp)
        bottomMargin = dp(ctx, bottomDp)
        marginStart = dp(ctx, startDp)
        marginEnd = dp(ctx, endDp)
        if (weight > 0f) this.weight = weight
    }

    fun frameLp(
        ctx: Context,
        width: Int = ViewGroup.LayoutParams.MATCH_PARENT,
        height: Int = ViewGroup.LayoutParams.WRAP_CONTENT,
        gravity: Int = Gravity.NO_GRAVITY
    ): FrameLayout.LayoutParams = FrameLayout.LayoutParams(width, height).apply { this.gravity = gravity }

    fun divider(ctx: Context, color: Int = borderSoft, thicknessDp: Float = 1f): View = View(ctx).apply {
        setBackgroundColor(color)
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(ctx, thicknessDp))
    }

    fun spacer(ctx: Context, heightDp: Float): View = View(ctx).apply {
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(ctx, heightDp))
    }

    // --- Components ---
    fun card(ctx: Context, padDp: Float = 18f, fill: Int = surface, stroke: Int = border): LinearLayout =
        col(ctx).apply {
            background = surface(ctx, fill, stroke)
            setPadding(dp(ctx, padDp), dp(ctx, padDp - 2f), dp(ctx, padDp), dp(ctx, padDp - 2f))
        }

    /** Quiet status pill, e.g. READY / OFF / LIVE. */
    fun pill(ctx: Context, label: String, fg: Int, bgAlphaHex: String = "1A"): TextView =
        text(ctx, label.uppercase(), 10f, fg, bold = true, letterSpacing = 0.06f).apply {
            background = surface(ctx, Color.parseColor(bgAlphaHex + fgHex(fg)), Color.TRANSPARENT, radiusPill, 0f)
            setPadding(dp(ctx, 9f), dp(ctx, 5f), dp(ctx, 9f), dp(ctx, 5f))
        }

    private fun fgHex(c: Int): String = String.format("%06X", 0xFFFFFF and c)

    /** Filled primary / secondary / ghost button with a consistent 52dp touch height. */
    fun button(
        ctx: Context,
        label: String,
        variant: ButtonVariant = ButtonVariant.PRIMARY,
        enabled: Boolean = true,
        onClick: () -> Unit
    ): TextView {
        val (fill, fg, stroke) = when (variant) {
            ButtonVariant.PRIMARY -> Triple(accent, onAccent, accent)
            ButtonVariant.INFO -> Triple(blue, Color.WHITE, blue)
            ButtonVariant.SECONDARY -> Triple(surfaceAlt, textPrimary, border)
            ButtonVariant.GHOST -> Triple(Color.TRANSPARENT, textSecondary, borderSoft)
        }
        val bg = surface(ctx, fill, stroke, radiusControl)
        return text(ctx, label, 13.5f, fg, bold = true).apply {
            gravity = Gravity.CENTER
            minimumHeight = dp(ctx, 48f)
            setPadding(dp(ctx, 16f), dp(ctx, 13f), dp(ctx, 16f), dp(ctx, 13f))
            background = ripple(ctx, bg)
            isClickable = enabled
            isFocusable = enabled
            alpha = if (enabled) 1f else 0.45f
            setOnClickListener { onClick() }
        }
    }

    enum class ButtonVariant { PRIMARY, INFO, SECONDARY, GHOST }

    /** Rounded input that matches the card surfaces. */
    fun input(ctx: Context, hint: String, value: String = "", mono: Boolean = false): android.widget.EditText =
        android.widget.EditText(ctx).apply {
            this.hint = hint
            setText(value)
            setHintTextColor(textMuted)
            setTextColor(textPrimary)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f)
            if (mono) typeface = Typeface.MONOSPACE
            background = surface(ctx, surfaceSunken, border, radiusControl)
            setPadding(dp(ctx, 14f), dp(ctx, 12f), dp(ctx, 14f), dp(ctx, 12f))
            isSingleLine = true
            setTextColor(textPrimary)
            highlightColor = accent
        }

    /** Brand mark: the same chain-link artwork used by the launcher icon. */
    fun logo(ctx: Context, sizeDp: Float = 34f): ImageView = ImageView(ctx).apply {
        setImageResource(R.drawable.ic_launcher)
        layoutParams = LinearLayout.LayoutParams(dp(ctx, sizeDp), dp(ctx, sizeDp))
        contentDescription = ctx.getString(R.string.app_name)
    }

    /** Small monochrome glyph badge (no icon font / no vector assets needed). */
    fun glyph(ctx: Context, char: String, tint: Int, sizeDp: Float = 36f, bgTint: Int = surfaceAlt): TextView =
        text(ctx, char, sizeDp * 0.44f, tint, bold = true).apply {
            gravity = Gravity.CENTER
            background = surface(ctx, bgTint, Color.TRANSPARENT, radiusControl, 0f)
            layoutParams = LinearLayout.LayoutParams(dp(ctx, sizeDp), dp(ctx, sizeDp))
        }
}
