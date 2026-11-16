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
    val bg = 0xFF070B12.toInt()
    val surface = 0xFF0E1420.toInt()
    val surfaceAlt = 0xFF131B29.toInt()
    val surfaceSunken = 0xFF0A0F19.toInt()
    val border = 0xFF1E293B.toInt()
    val borderSoft = 0xFF18202F.toInt()

    val textPrimary = 0xFFF1F5F9.toInt()
    val textSecondary = 0xFF9AA7BA.toInt()
    val textMuted = 0xFF64748B.toInt()

    val accent = 0xFF10B981.toInt()      // emerald - primary brand
    val accentSoft = 0xFF6EE7B7.toInt()
    val info = 0xFF22D3EE.toInt()        // cyan - links / bluetooth
    val blue = 0xFF3B82F6.toInt()
    val amber = 0xFFF59E0B.toInt()
    val danger = 0xFFF43F5E.toInt()

    val onAccent = 0xFF04120C.toInt()

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
        val mask = ColorStateList.valueOf(0x33FFFFFF.toInt())
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

    /**
     * Tints a color with an alpha byte. Replaces the old `Color.parseColor(hex + hex)` trick:
     * string color parsing throws IllegalArgumentException("Unknown color") on anything
     * malformed, which took the whole interface down at startup. Integer math cannot throw.
     */
    fun withAlpha(color: Int, alpha: Int = 0x1A): Int =
        Color.argb(alpha and 0xFF, Color.red(color), Color.green(color), Color.blue(color))

    /** Quiet status pill, e.g. READY / OFF / LIVE. */
    fun pill(ctx: Context, label: String, fg: Int, bgAlpha: Int = 0x1A): TextView =
        text(ctx, label.uppercase(), 10f, fg, bold = true, letterSpacing = 0.06f).apply {
            background = surface(ctx, withAlpha(fg, bgAlpha), Color.TRANSPARENT, radiusPill, 0f)
            setPadding(dp(ctx, 9f), dp(ctx, 5f), dp(ctx, 9f), dp(ctx, 5f))
        }

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
