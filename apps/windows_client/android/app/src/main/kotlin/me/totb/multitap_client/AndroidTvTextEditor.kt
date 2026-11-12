package me.totb.multitap_client

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.icu.text.BreakIterator
import android.text.InputFilter
import android.text.InputType
import android.text.method.PasswordTransformationMethod
import android.view.ContextThemeWrapper
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.LinearLayout
import io.flutter.plugin.common.MethodChannel

/** A real Android editor: the dialog and OS IME own remote navigation. */
class AndroidTvTextEditor(private val activity: Activity) {
    private class Request(val reply: MethodChannel.Result, val field: EditText) {
        var dialog: AlertDialog? = null
        var finished = false
    }
    private var current: Request? = null
    private var disposed = false

    fun edit(arguments: Any?, reply: MethodChannel.Result) {
        if (!AndroidTv.isTelevision(activity)) {
            reply.error("not_supported", MultitapText.get(activity, R.string.tv_only), null)
            return
        }
        if (current != null) {
            reply.error("busy", MultitapText.get(activity, R.string.tv_input_busy), null)
            return
        }
        if (disposed || activity.isFinishing || activity.isDestroyed) {
            reply.success(null)
            return
        }
        val args = arguments as? Map<*, *>
        val label = args?.get("label") as? String
        val initial = args?.get("initial") as? String
        val type = args?.get("type") as? String
        val action = args?.get("action") as? String
        val length = when (val value = args?.get("maxLength")) {
            is Int -> value.toLong()
            is Long -> value
            else -> null
        }
        val dark = args?.get("dark") as? Boolean
        if (label == null || label.length !in 1..160 || initial == null ||
            type !in setOf("text", "url", "password") ||
            action !in setOf("next", "go", "done") ||
            length == null || length !in 1L..8192L || initial.length > 8192 ||
            !validText(initial, length.toInt()) || dark == null) {
            reply.error("invalid_arguments", MultitapText.get(activity, R.string.tv_input_invalid), null)
            return
        }
        val dialogBackground = Color.parseColor(if (dark) "#171B20" else "#FFFFFF")
        val fieldBackground = Color.parseColor(if (dark) "#222830" else "#F2F4F6")
        val foreground = Color.parseColor(if (dark) "#EDF0F3" else "#17212B")
        val muted = Color.parseColor(if (dark) "#A9B2BE" else "#56616F")
        val amber = Color.parseColor(if (dark) "#F5A623" else "#865000")
        val context = ContextThemeWrapper(activity, if (dark)
            android.R.style.Theme_Material_Dialog_Alert else android.R.style.Theme_Material_Light_Dialog_Alert)
        val imeAction = when (action) {
            "next" -> EditorInfo.IME_ACTION_NEXT
            "go" -> EditorInfo.IME_ACTION_GO
            else -> EditorInfo.IME_ACTION_DONE
        }
        val field = EditText(context).apply {
            isSaveEnabled = false
            isSaveFromParentEnabled = false
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or when (type) {
                "url" -> InputType.TYPE_TEXT_VARIATION_URI
                "password" -> InputType.TYPE_TEXT_VARIATION_PASSWORD
                else -> InputType.TYPE_TEXT_VARIATION_NORMAL
            }
            if (type == "password") transformationMethod = PasswordTransformationMethod.getInstance()
            imeOptions = imeAction or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
            filters = arrayOf(clusterFilter(length.toInt()))
            setText(initial)
            setSelection(text.length)
            setTextColor(foreground)
            setHintTextColor(muted)
            contentDescription = label
            textSize = 20f
            minHeight = dp(56)
            setPadding(dp(16), dp(12), dp(16), dp(12))
            background = GradientDrawable().apply {
                setColor(fieldBackground)
                cornerRadius = dp(8).toFloat()
                setStroke(dp(1), amber)
            }
        }
        val request = Request(reply, field)
        current = request
        try {
            val container = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                isSaveEnabled = false
                setPadding(dp(24), dp(16), dp(24), dp(8))
                addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT))
            }
            val dialog = AlertDialog.Builder(context)
                .setTitle(label)
                .setView(container)
                .setPositiveButton(MultitapText.get(activity, when (action) { "next" -> R.string.tv_next; "go" -> R.string.tv_go; else -> R.string.tv_done }), null)
                .setNegativeButton(MultitapText.get(activity, R.string.cancel)) { _, _ -> finish(request, null) }
                .create()
            request.dialog = dialog
            dialog.setOnCancelListener { finish(request, null) }
            dialog.setOnDismissListener { finish(request, null) }
            field.setOnEditorActionListener { _, id, event ->
                val submit = id == imeAction || (id == EditorInfo.IME_NULL &&
                    event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
                if (submit) finish(request, field.text.toString())
                submit
            }
            dialog.setOnShowListener {
                dialog.window?.setBackgroundDrawable(GradientDrawable().apply {
                    setColor(dialogBackground)
                    cornerRadius = dp(12).toFloat()
                })
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).apply {
                    setTextColor(amber)
                    setOnClickListener { finish(request, field.text.toString()) }
                }
                dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setTextColor(muted)
                field.requestFocus()
                field.post {
                    if (current === request && !request.finished && dialog.isShowing && field.hasFocus()) {
                        (activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
                            .showSoftInput(field, InputMethodManager.SHOW_IMPLICIT)
                    }
                }
            }
            dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE or
                WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
            dialog.show()
        } catch (_: Exception) {
            finish(request, null)
        }
    }

    private fun finish(request: Request, value: String?) {
        if (request.finished) return
        request.finished = true
        if (current === request) current = null
        (activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager)
            ?.hideSoftInputFromWindow(request.field.windowToken, 0)
        request.field.text.clear()
        request.dialog?.dismiss()
        // No field content is logged, persisted, or included in an error.
        request.reply.success(value)
    }

    fun cancel() { current?.let { finish(it, null) } }
    fun dispose() { disposed = true; cancel() }
    private fun validText(value: String, limit: Int): Boolean {
        if (value.length > 8192) return false
        var index = 0
        while (index < value.length) {
            val char = value[index]
            if (Character.isHighSurrogate(char)) {
                if (index + 1 >= value.length || !Character.isLowSurrogate(value[index + 1])) return false
                index += 2
            } else {
                if (Character.isLowSurrogate(char)) return false
                index++
            }
        }
        val boundaries = BreakIterator.getCharacterInstance().apply { setText(value) }
        var count = 0
        boundaries.first()
        while (boundaries.next() != BreakIterator.DONE) if (++count > limit) return false
        return true
    }

    private fun clusterFilter(limit: Int) = InputFilter { source, start, end, dest, dstart, dend ->
        // Reject an oversized incoming slice without copying it. Preserve the
        // selected text on rejection; an empty replacement would delete it.
        val selected = dest.subSequence(dstart, dend)
        if (end - start > 8192 || dest.length > 8192) return@InputFilter selected
        val incoming = source.subSequence(start, end).toString()
        val prefix = dest.subSequence(0, dstart).toString()
        val suffix = dest.subSequence(dend, dest.length).toString()
        if (validText(prefix + incoming + suffix, limit)) return@InputFilter null
        val boundaries = BreakIterator.getCharacterInstance().apply { setText(incoming) }
        boundaries.first()
        var accepted = 0
        var cut = boundaries.next()
        while (cut != BreakIterator.DONE) {
            if (!validText(prefix + incoming.substring(0, cut) + suffix, limit)) break
            accepted = cut
            cut = boundaries.next()
        }
        if (accepted == 0) selected else source.subSequence(start, start + accepted)
    }
    private fun dp(value: Int) = (value * activity.resources.displayMetrics.density).toInt()
}
