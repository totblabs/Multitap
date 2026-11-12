package me.totb.multitap_client

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.net.http.SslError
import android.os.Build
import android.os.Bundle
import android.os.Message
import android.view.View
import android.view.ViewGroup
import android.view.MotionEvent
import android.view.KeyEvent
import android.view.WindowInsets
import android.webkit.ClientCertRequest
import android.webkit.CookieManager
import android.webkit.GeolocationPermissions
import android.webkit.HttpAuthHandler
import android.webkit.PermissionRequest
import android.webkit.RenderProcessGoneDetail
import android.webkit.SslErrorHandler
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import java.io.ByteArrayInputStream
import java.lang.ref.WeakReference
import java.net.URL
import javax.net.ssl.HttpsURLConnection

/** One app-owned console at a time; attachment secrets never enter an Intent. */
internal object ConsoleWebViews {
    internal class Request(val target: ConsoleWebViewTarget, var code: String?, val closed: (Map<String, String>) -> Unit) {
        val authenticationOwner = ClientAuthActivity.owner
        var retired = false
        val ownerId = ConsoleWebViewPolicy.ownerId(code ?: "")
    }
    private var request: Request? = null
    private var activity = WeakReference<ConsoleWebViewActivity>(null)
    fun hasPending() = request != null
    fun hide(owner: String) {
        val pending = request ?: return
        if (pending.authenticationOwner == owner) {
            pending.retired = true; pending.code = null
            activity.get()?.hideFromApp()
        }
    }
    fun open(owner: Activity, arguments: Any?, closed: (Map<String, String>) -> Unit): String? {
        if (!ClientAuthActivity.check()) return "authentication_owner_changed"
        val args = arguments as? Map<*, *> ?: return "invalid_console_webview"
        if (args.size != 4) return "invalid_console_webview"
        val target = ConsoleWebViewPolicy.target(args["origin"] as? String ?: "", args["desktopId"] as? String ?: "", args["attachPath"] as? String ?: "") ?: return "invalid_console_webview"
        val code = args["code"] as? String ?: return "invalid_console_webview"
        if (!ConsoleWebViewPolicy.code(code)) return "invalid_console_webview"
        request?.let {
            if (it.target != target) return "console_webview_busy"
            return try {
                owner.startActivity(Intent(owner, ConsoleWebViewActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP))
                null
            } catch (_: Exception) { "console_webview_unavailable" }
        }
        val pending = Request(target, code, closed)
        request = pending
        return try {
            owner.startActivity(Intent(owner, ConsoleWebViewActivity::class.java))
            null
        } catch (_: Exception) {
            pending.code = null; request = null
            "console_webview_unavailable"
        }
    }
    fun claim(screen: ConsoleWebViewActivity): Request? {
        val pending = request ?: return null
        if (pending.retired || ClientAuthActivity.isRetired(pending.authenticationOwner)) return null
        val existing = activity.get()
        if (existing != null && existing !== screen) return null
        activity = WeakReference(screen)
        return pending
    }
    fun close(arguments: Any?): String? {
        val args = arguments as? Map<*, *> ?: return "invalid_console_webview"
        if (args.isNotEmpty()) {
            if (args.size != 2) return "invalid_console_webview"
            val origin = args["origin"] as? String ?: return "invalid_console_webview"
            val id = args["desktopId"] as? String ?: return "invalid_console_webview"
            val target = ConsoleWebViewPolicy.target(origin, id, "/console/$id/attach") ?: return "invalid_console_webview"
            if (request?.target != target) return null
        }
        val current = activity.get()
        if (current != null) current.closeFromApp()
        else request?.let { finished(null, it) }
        return null
    }
    fun finished(screen: ConsoleWebViewActivity?, owned: Request) {
        if (request !== owned || (screen != null && activity.get() !== screen)) return
        request = null; activity.clear(); owned.code = null
        owned.closed(mapOf("origin" to owned.target.origin, "desktopId" to owned.target.desktopId, "ownerId" to owned.ownerId))
    }
}

class ConsoleWebViewActivity : Activity() {
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        ClientAuthActivity.human(request?.authenticationOwner)
        return super.dispatchTouchEvent(event)
    }
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_HOVER_MOVE || event.action == MotionEvent.ACTION_SCROLL || event.action == MotionEvent.ACTION_MOVE)
            ClientAuthActivity.human(request?.authenticationOwner)
        return super.dispatchGenericMotionEvent(event)
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.flags and KeyEvent.FLAG_FALLBACK == 0) ClientAuthActivity.human(request?.authenticationOwner)
        return super.dispatchKeyEvent(event)
    }
    fun hideFromApp() { web?.visibility = View.INVISIBLE; root?.setBackgroundColor(Color.BLACK) }
    private var request: ConsoleWebViews.Request? = null
    private var web: WebView? = null
    private var root: FrameLayout? = null
    private var progress: View? = null
    private var closed = false
    private var closingDialog: AlertDialog? = null
    private val charcoal = Color.rgb(24, 29, 32)
    private val amber = Color.rgb(225, 158, 23)
    private fun dp(value: Int) = (value * resources.displayMetrics.density + 0.5f).toInt()

    /**
     * Android 15+ draws this window edge to edge (target 35+): the page lay under the
     * status and navigation bars and the keyboard covered it. The frame keeps its
     * content inside the system bars, the display cutout and the keyboard. Where the
     * system still fits the window itself (Android 14 and older) the insets arrive
     * here already used up, as zero.
     */
    @Suppress("DEPRECATION")
    private fun fitInsets(target: View, insets: WindowInsets): WindowInsets {
        if (Build.VERSION.SDK_INT >= 30) {
            val safe = insets.getInsets(
                WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout() or WindowInsets.Type.ime()
            )
            target.setPadding(safe.left, safe.top, safe.right, safe.bottom)
            return WindowInsets.CONSUMED
        }
        target.setPadding(
            insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
            insets.systemWindowInsetRight, insets.systemWindowInsetBottom
        )
        return insets.consumeSystemWindowInsets()
    }

    @SuppressLint("SetJavaScriptEnabled")
    /** Text in the app's language (MultitapText). */
    private fun t(id: Int) = MultitapText.get(this, id)

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val owned = ConsoleWebViews.claim(this)
        if (owned == null) { finish(); return }
        request = owned
        title = "Multitap · Console"
        val frame = FrameLayout(this).also { it.setBackgroundColor(charcoal) }
        root = frame
        frame.setOnApplyWindowInsetsListener { target, insets -> fitInsets(target, insets) }
        setContentView(frame)
        val loading = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; gravity = android.view.Gravity.CENTER
            addView(ProgressBar(context), LinearLayout.LayoutParams(dp(64), dp(64)))
            addView(TextView(context).apply { text = t(R.string.console_connecting); setTextColor(Color.WHITE); textSize = 18f; setPadding(dp(24), dp(24), dp(24), dp(24)) })
        }
        progress = loading
        val view = try { WebView(this) } catch (_: Exception) { fail(t(R.string.console_webview_update)); return }
        web = view
        view.setBackgroundColor(Color.BLACK)
        view.isSaveEnabled = false
        view.importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        view.settings.apply {
            userAgentString = userAgentString + " Multitap-ConsoleWebView/1"
            javaScriptEnabled = true
            domStorageEnabled = false
            databaseEnabled = false
            allowFileAccess = false
            allowContentAccess = false
            mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
            javaScriptCanOpenWindowsAutomatically = false
            setSupportMultipleWindows(false)
            cacheMode = WebSettings.LOAD_NO_CACHE
            mediaPlaybackRequiresUserGesture = true
            safeBrowsingEnabled = true
        }
        CookieManager.getInstance().setAcceptThirdPartyCookies(view, false)
        view.setDownloadListener { _, _, _, _, _ -> fail(t(R.string.console_no_download)) }
        view.webChromeClient = object : WebChromeClient() {
            override fun onPermissionRequest(permission: PermissionRequest) = permission.deny()
            override fun onGeolocationPermissionsShowPrompt(origin: String?, callback: GeolocationPermissions.Callback?) { callback?.invoke(origin, false, false) }
            override fun onCreateWindow(view: WebView?, dialog: Boolean, userGesture: Boolean, result: Message?): Boolean = false
            override fun onShowFileChooser(view: WebView?, callback: android.webkit.ValueCallback<Array<android.net.Uri>>?, params: FileChooserParams?): Boolean { callback?.onReceiveValue(null); return true }
        }
        view.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, value: WebResourceRequest): Boolean = !owned.target.navigation(value.url.toString())
            override fun shouldInterceptRequest(view: WebView, value: WebResourceRequest): WebResourceResponse? {
                if (owned.target.resource(value.url.toString())) return null
                return WebResourceResponse("text/plain", "UTF-8", 403, "Blocked", emptyMap(), ByteArrayInputStream(ByteArray(0)))
            }
            override fun onPageFinished(view: WebView, url: String) {
                if (!owned.target.navigation(url)) { fail(t(R.string.console_address_refused)); return }
                progress?.let { frame.removeView(it) }; progress = null
                view.requestFocus()
            }
            override fun onReceivedError(view: WebView, value: WebResourceRequest, error: WebResourceError) { if (value.isForMainFrame) fail(t(R.string.console_disconnected)) }
            override fun onReceivedSslError(view: WebView, handler: SslErrorHandler, error: SslError) { handler.cancel(); fail(t(R.string.console_certificate)) }
            override fun onReceivedHttpAuthRequest(view: WebView, handler: HttpAuthHandler, host: String, realm: String) { handler.cancel(); fail(t(R.string.console_auth_failed)) }
            override fun onReceivedClientCertRequest(view: WebView, value: ClientCertRequest) = value.cancel()
            override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                (view.parent as? ViewGroup)?.removeView(view); view.destroy(); if (web === view) web = null
                fail(t(R.string.console_frozen)); return true
            }
        }
        frame.addView(view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        frame.addView(loading, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        val code = owned.code
        owned.code = null
        if (code == null) { fail(t(R.string.console_time_up)); return }
        Thread({ attach(owned, code) }, "Multitap-console-attach").start()
    }

    private fun attach(owned: ConsoleWebViews.Request, code: String) {
        var connection: HttpsURLConnection? = null
        try {
            connection = URL(owned.target.attachUrl).openConnection() as HttpsURLConnection
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 10000; connection.readTimeout = 10000
            connection.requestMethod = "POST"; connection.doOutput = true; connection.useCaches = false
            connection.setRequestProperty("Origin", owned.target.origin)
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            connection.setRequestProperty("Accept", "text/html")
            val body = "code=$code".toByteArray(Charsets.US_ASCII)
            connection.setFixedLengthStreamingMode(body.size)
            connection.outputStream.use { it.write(body) }
            val location = connection.getHeaderField("Location")
            val cookies = connection.headerFields.entries.filter { it.key.equals("Set-Cookie", true) }.flatMap { it.value ?: emptyList() }
            if (connection.responseCode != 303 || location != owned.target.consolePath || cookies.size != 1) throw IllegalStateException()
            val cookie = owned.target.cookie(cookies.single()) ?: throw IllegalStateException()
            runOnUiThread {
                if (closed || isFinishing || request !== owned) return@runOnUiThread
                CookieManager.getInstance().setCookie(owned.target.origin, cookie) { accepted ->
                    if (closed || isFinishing || request !== owned) return@setCookie
                    if (accepted != true) fail(t(R.string.console_auth_not_saved))
                    else web?.loadUrl(owned.target.consoleUrl)
                }
            }
        } catch (_: Exception) {
            runOnUiThread { if (!closed && !isFinishing) fail(t(R.string.console_connect_failed)) }
        } finally { connection?.disconnect() }
    }

    private fun fail(message: String) {
        if (closed || isFinishing) return
        web?.stopLoading()
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; gravity = android.view.Gravity.CENTER
            setBackgroundColor(charcoal); setPadding(dp(28), dp(28), dp(28), dp(28))
            addView(TextView(context).apply { text = message; textSize = 18f; setTextColor(Color.WHITE); gravity = android.view.Gravity.CENTER })
            addView(Button(context).apply { text = t(R.string.close); setTextColor(amber); setOnClickListener { closeFromApp() } }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(56)).apply { topMargin = dp(24) })
        }
        root?.removeAllViews(); root?.addView(panel, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }
    @Deprecated("Android compatibility callback")
    override fun onBackPressed() {
        if (closingDialog?.isShowing == true) return
        closingDialog = AlertDialog.Builder(this)
            .setTitle(t(R.string.console_close_title))
            .setMessage(t(R.string.console_close_message))
            .setNegativeButton(t(R.string.cancel), null)
            .setPositiveButton(t(R.string.close)) { _, _ -> closeFromApp() }.show()
    }
    fun closeFromApp() { if (!closed) { releaseConsole(); finish() } }
    override fun onPause() { web?.clearFocus(); web?.onPause(); super.onPause() }
    override fun onResume() { super.onResume(); if (!ClientAuthActivity.check()) hideFromApp() else web?.onResume() }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) web?.requestFocus() else web?.clearFocus() }
    override fun onSaveInstanceState(state: Bundle) { super.onSaveInstanceState(state); state.clear() }
    private fun releaseConsole() {
        closed = true; closingDialog?.dismiss(); closingDialog = null
        web?.let { view -> view.stopLoading(); (view.parent as? ViewGroup)?.removeView(view); view.webChromeClient = null; view.destroy() }; web = null
        request?.let { owned ->
            // Clear only our exact host-only console cookie, never other origins.
            try { CookieManager.getInstance().setCookie(owned.target.origin, "${owned.target.cookieName}=; Path=/console; HttpOnly; Secure; SameSite=Strict; Max-Age=0") } catch (_: Exception) { }
            ConsoleWebViews.finished(this, owned)
        }
        request = null; root = null; progress = null
    }
    override fun onDestroy() {
        releaseConsole()
        super.onDestroy()
    }
}
