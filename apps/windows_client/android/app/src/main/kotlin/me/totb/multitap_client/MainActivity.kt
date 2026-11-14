package me.totb.multitap_client

import android.content.res.Configuration
import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewTreeObserver
import android.view.WindowInsets
import android.hardware.input.InputManager
import android.os.Handler
import android.view.WindowManager
import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/** Process-local app ownership only. No key/text/coordinate data crosses it. */
internal object ClientAuthActivity {
    var owner: String? = null
        private set
    private var retired = false
    private var uiReady = false
    private var last = 0L
    private var deadlineAt: Long? = null
    private val deadlineHandler = Handler(android.os.Looper.getMainLooper())
    private var deadlineTask: Runnable? = null
    private var listener: ((Map<String, String>) -> Unit)? = null
    fun bind(value: String, remaining: Any?, callback: (Map<String, String>) -> Unit): Boolean {
        if (!Regex("^[a-f0-9]{32}$").matches(value)) return false
        if (owner != value && ConsoleWebViews.hasPending()) {
            owner?.let { ConsoleWebViews.hide(it) }
            return false
        }
        val accepted: Long? = if (remaining == null) null else {
            integer(remaining)?.takeIf { it in 0L..86400000L } ?: return false
        }
        owner = value; retired = false; uiReady = false; last = 0; listener = callback
        deadlineTask?.let { deadlineHandler.removeCallbacks(it) }
        deadlineTask = null; deadlineAt = null
        if (accepted != null) setDeadline(value, accepted)
        return true
    }
    fun integer(value: Any?): Long? = when (value) { is Long -> value; is Int -> value.toLong(); else -> null }
    private fun setDeadline(expected: String, remaining: Long) {
        deadlineTask?.let { deadlineHandler.removeCallbacks(it) }
        val elapsedDeadline = android.os.SystemClock.elapsedRealtime() + remaining
        deadlineAt = elapsedDeadline
        val task = Runnable { if (owner == expected && deadlineAt == elapsedDeadline) check() }
        deadlineTask = task; deadlineHandler.postDelayed(task, remaining)
    }
    fun updateDeadline(expected: String, value: Any?): Boolean {
        if (expected != owner || retired || !check()) return false
        val remaining: Long? = if (value == null) null else {
            integer(value)?.takeIf { it in 0L..86400000L } ?: return false
        }
        if (remaining == null) {
            deadlineTask?.let { deadlineHandler.removeCallbacks(it) }; deadlineTask = null; deadlineAt = null
        } else setDeadline(expected, remaining)
        return true
    }
    fun check(): Boolean {
        if (retired) return false
        val deadline = deadlineAt
        if (deadline != null && android.os.SystemClock.elapsedRealtime() >= deadline) {
            val current = owner ?: return false
            retire(current)
            listener?.invoke(mapOf("owner" to current, "kind" to "terminal", "reason" to "session_idle_timeout"))
            return false
        }
        return true
    }
    fun human(expected: String?) {
        if (expected == null || expected != owner || !check()) return
        val now = android.os.SystemClock.elapsedRealtime()
        if (last != 0L && now - last < 250) return
        last = now
        listener?.invoke(mapOf("owner" to expected, "kind" to "human"))
    }
    fun retire(expected: String?): Boolean {
        if (expected == null || expected != owner) return false
        if (retired) return true
        retired = true
        uiReady = false
        deadlineTask?.let { deadlineHandler.removeCallbacks(it) }; deadlineTask = null
        ConsoleWebViews.hide(expected)
        return true
    }
    fun isRetired(expected: String?) = expected != null && expected == owner && retired
    fun markUiReady(expected: String?): Boolean {
        if (!isRetired(expected)) return false
        uiReady = true; return true
    }
    fun needsShield() = retired && !uiReady
}

class MainActivity : FlutterActivity() {
    // The launcher activity is exported, and FlutterActivity reads its launch
    // configuration from the Intent: any app could start it transparent over
    // its own window, on a cached engine, at another route or Dart entry point,
    // with engine flags. Every such extra is
    // ignored: always opaque, a new engine, the default route and main().
    // Debug builds keep the engine flags `flutter run` passes.
    override fun getBackgroundMode() = io.flutter.embedding.android.FlutterActivityLaunchConfigs.BackgroundMode.opaque
    override fun getCachedEngineId(): String? = null
    override fun getCachedEngineGroupId(): String? = null
    override fun getInitialRoute(): String? = null
    override fun getDartEntrypointFunctionName(): String = "main"
    override fun getDartEntrypointArgs(): List<String>? = null
    override fun shouldHandleDeeplinking(): Boolean = false
    override fun shouldDestroyEngineWithHost(): Boolean = true
    override fun shouldRestoreAndSaveState(): Boolean = true
    @Suppress("DEPRECATION")  // the engine still reads these flags from the Intent
    override fun getFlutterShellArgs(): io.flutter.embedding.engine.FlutterShellArgs =
        if ((applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) super.getFlutterShellArgs()
        else io.flutter.embedding.engine.FlutterShellArgs(arrayOf<String>())
    private var authenticationShield: android.view.View? = null
    private fun shieldAuthentication() {
        if (authenticationShield == null) {
            authenticationShield = android.view.View(this).apply { setBackgroundColor(android.graphics.Color.BLACK); isClickable = true }
            addContentView(authenticationShield, android.view.ViewGroup.LayoutParams(-1, -1))
        }
        media?.releaseInput()
    }
    private var media: AndroidMedia? = null
    private var gameMouse: AndroidGameMouse? = null
    // gamepad-v1: this phone's game controllers for the session (AndroidGamepad.kt).
    private var gamepad: AndroidGamepad? = null
    private var udp: AndroidUdp? = null
    private var update: AndroidUpdate? = null
    private var printer: AndroidPrint? = null
    private var drive: AndroidDrive? = null
    private var clipboard: AndroidClipboard? = null
    private var network: AndroidNetwork? = null
    private var penInput: PenInputPlugin? = null
    private var sessionForeground = false
    private var sessionLease: String? = null
    private var notificationReply: MethodChannel.Result? = null
    private var disconnectBridgeToken: Any? = null
    private var applicationChannel: MethodChannel? = null
    private var tvTextEditor: AndroidTvTextEditor? = null
    private var inputManager: InputManager? = null
    private var keyboardKnown: Boolean? = null
    private var lastHardwareNavigation: Boolean? = null
    private var televisionMode = false
    private var lastImeVisibility: Boolean? = null
    private val imeLayoutListener = ViewTreeObserver.OnGlobalLayoutListener { reportImeVisibility() }
    private val imeDrawListener = ViewTreeObserver.OnPreDrawListener { reportImeVisibility(); true }
    private fun imeVisible(): Boolean? = if (televisionMode && Build.VERSION.SDK_INT >= 30) {
        window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime())
    } else null
    private fun reportImeVisibility() {
        val visible = imeVisible() ?: return
        if (lastImeVisibility == visible) return
        lastImeVisibility = visible
        if (visible) media?.releaseInput()
        applicationChannel?.invokeMethod("imeVisibility", visible)
    }
    private val inputListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(id: Int) { keyboardChanged(); gameMouse?.devicesChanged() }
        override fun onInputDeviceRemoved(id: Int) { keyboardChanged(); gameMouse?.devicesChanged() }
        override fun onInputDeviceChanged(id: Int) { keyboardChanged(); gameMouse?.devicesChanged() }
    }
    private fun keyboardChanged() {
        if (!AndroidTv.isTelevision(this)) return
        val present = AndroidTv.hasHardwareKeyboard()
        if (keyboardKnown != present) {
            keyboardKnown = present
            applicationChannel?.invokeMethod("hardwareKeyboardChanged", present)
        }
    }
    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)
        penInput?.dispose()
        penInput = PenInputPlugin(engine.dartExecutor.binaryMessenger)
        televisionMode = AndroidTv.isTelevision(this)
        tvTextEditor = AndroidTvTextEditor(this)
        applicationChannel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/application")
        applicationChannel!!.setMethodCallHandler { call, result ->
            if (call.method == "bindAuthentication") {
                val args = call.arguments as? Map<*, *>
                val owner = args?.get("owner") as? String
                val activity = java.lang.ref.WeakReference(this)
                if (args == null || args.keys.any { it != "owner" && it != "idleRemainingMs" } || owner == null || !ClientAuthActivity.bind(owner, args["idleRemainingMs"]) { value ->
                    activity.get()?.let { current ->
                        if (value["kind"] == "terminal") current.shieldAuthentication()
                        current.applicationChannel?.invokeMethod("authenticationEvent", value)
                    }
                })
                    result.error("authentication_owner_changed", "Authentication owner changed.", null)
                else result.success(null)
            } else if (call.method == "authenticationDeadline") {
                val args = call.arguments as? Map<*, *>
                val owner = args?.get("owner") as? String
                val remaining = args?.get("idleRemainingMs")
                if (args?.size != 2 || !args.containsKey("idleRemainingMs") || owner == null || !ClientAuthActivity.updateDeadline(owner, remaining))
                    result.error("authentication_owner_changed", "Authentication owner changed.", null)
                else result.success(null)
            } else if (call.method == "hideAuthentication") {
                val args = call.arguments as? Map<*, *>
                val owner = args?.get("owner") as? String
                if (args?.size != 1 || !ClientAuthActivity.retire(owner)) result.error("authentication_owner_changed", "Authentication owner changed.", null)
                else {
                    if (ClientAuthActivity.needsShield()) shieldAuthentication()
                    result.success(null)
                }
            } else if (call.method == "authenticationShieldReady") {
                val args = call.arguments as? Map<*, *>
                if (args?.size != 1 || !ClientAuthActivity.markUiReady(args["owner"] as? String)) result.error("authentication_owner_changed", "Authentication owner changed.", null)
                else { (authenticationShield?.parent as? android.view.ViewGroup)?.removeView(authenticationShield); authenticationShield = null; result.success(null) }
            } else if (call.method == "hideConsoleWebViews") {
                ClientAuthActivity.owner?.let { ConsoleWebViews.hide(it) }; result.success(null)
            } else if (call.method == "close") {
                tvTextEditor?.cancel()
                result.success(null)
                window.decorView.post { finishAndRemoveTask() }
            } else if (call.method == "tvEditText") {
                tvTextEditor?.edit(call.arguments, result) ?: result.success(null)
            } else if (call.method == "isTelevision") {
                result.success(AndroidTv.isTelevision(this))
            } else if (call.method == "hasHardwareKeyboard") {
                result.success(AndroidTv.hasHardwareKeyboard())
            } else if (call.method == "imeVisible") {
                result.success(imeVisible())
            } else if (call.method == "openConsoleWebView") {
                val failure = ConsoleWebViews.open(this, call.arguments) { value -> applicationChannel?.invokeMethod("consoleWebViewClosed", value) }
                if (failure == null) result.success(true)
                else result.error(failure, "Console unavailable.", null)
            } else if (call.method == "closeConsoleWebViews") {
                val failure = ConsoleWebViews.close(call.arguments)
                if (failure == null) result.success(null)
                else result.error(failure, "Invalid console scope.", null)
            } else if (call.method == "openConsoleBrowser") {
                val args = call.arguments as? Map<*, *>
                val target = ConsoleBrowserTarget.build(args?.get("origin") as? String ?: "", args?.get("desktopId") as? String ?: "")
                if (target == null) result.error("invalid_console_browser", "Invalid console destination.", null)
                else try {
                    startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(target)).addCategory(Intent.CATEGORY_BROWSABLE))
                    result.success(true)
                } catch (_: android.content.ActivityNotFoundException) {
                    result.error("console_browser_unavailable", "Browser unavailable.", null)
                } catch (_: SecurityException) {
                    result.error("console_browser_unavailable", "Browser unavailable.", null)
                }
            } else result.notImplemented()
        }
        inputManager = getSystemService(INPUT_SERVICE) as InputManager
        inputManager?.registerInputDeviceListener(inputListener, Handler(mainLooper))
        // Observe actual IME visibility without replacing Flutter's inset
        // listener or requiring the keyboard to reduce the window height.
        window.decorView.viewTreeObserver.addOnGlobalLayoutListener(imeLayoutListener)
        window.decorView.viewTreeObserver.addOnPreDrawListener(imeDrawListener)
        // udp-media-v1: the UDP leg and the decoder meet at one path gate.
        udp?.dispose()
        val udpMedia = AndroidUdp(engine)
        udp = udpMedia
        media = AndroidMedia(this, engine, udpMedia.gate)
        udpMedia.trackGate = { binding -> media?.trackGate(binding) }
        // 게임 마우스: pointer capture feeds the virtual USB mouse or moveRelative.
        gameMouse?.dispose()
        gameMouse = AndroidGameMouse(this, engine, media!!)
        gamepad?.dispose()
        gamepad = AndroidGamepad(this, engine)
        AndroidStore(this, engine)
        AndroidDisplay(this, engine)
        clipboard?.dispose()
        clipboard = AndroidClipboard(this, engine)
        network = AndroidNetwork(this, engine)
        update?.dispose()
        update = AndroidUpdate(this, engine)
        // print-v1: documents printed in the VM go to Android's print dialog.
        printer = AndroidPrint(this, engine)
        // drive-v1: a folder picked with Android's folder picker is shared with the desktop.
        drive?.dispose()
        drive = AndroidDrive(this, engine)
        val sessionChannel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/session-keepalive")
        disconnectBridgeToken = AndroidRemoteSessionService.attachDisconnectBridge {
            sessionChannel.invokeMethod("disconnect", mapOf("lease" to it))
        }
        sessionChannel.setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    val lease = call.argument<String>("lease")
                    if (lease == null) result.success("failed")
                    else {
                        val previous = sessionLease
                        sessionLease = lease
                        AndroidRemoteSessionService.start(this, sessionForeground, lease) { state ->
                            // A rejected start may leave an older owned service
                            // intact. Do not lose that Activity's cleanup lease.
                            if (sessionLease == lease && state != "started" &&
                                state != "startedWithoutNotificationPermission" && state != "superseded") sessionLease = previous
                            result.success(state)
                        }
                    }
                }
                "stop" -> {
                    val lease = call.argument<String>("lease") ?: ""
                    if (sessionLease == lease) sessionLease = null
                    result.success(AndroidRemoteSessionService.stop(applicationContext, lease))
                }
                // 공지 (AndroidNotices.kt): a notice that arrived while the app was not in front.
                "notice" -> result.success(AndroidNotices.post(applicationContext, call.argument<String>("title"), call.argument<String>("body"), call.argument<Int>("id")))
                "notificationPermission" -> {
                    val preferences = getSharedPreferences("session-notifications", MODE_PRIVATE)
                    if (Build.VERSION.SDK_INT < 33 || checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) result.success(AndroidRemoteSessionService.notificationsVisible(this))
                    else if (!sessionForeground || notificationReply != null) result.success(false)
                    else if (preferences.getBoolean("permissionPrompted", false)) result.success(false)
                    else {
                        preferences.edit().putBoolean("permissionPrompted", true).apply()
                        notificationReply = result
                        try { requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 7321) }
                        catch (_: Exception) { notificationReply = null; result.success(false) }
                    }
                }
                "notificationSettings" -> {
                    try {
                        if (!sessionForeground) throw IllegalStateException()
                        startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, packageName))
                        result.success(true)
                    } catch (_: Exception) { result.success(false) }
                }
                else -> result.notImplemented()
            }
        }
    }
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        ClientAuthActivity.human(ClientAuthActivity.owner)
        if (!ClientAuthActivity.check()) return super.dispatchTouchEvent(event)
        gameMouse?.observe(event)
        return penInput?.dispatchTouch(event) { super.dispatchTouchEvent(it) }
            ?: super.dispatchTouchEvent(event)
    }
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_HOVER_MOVE || event.action == MotionEvent.ACTION_SCROLL || event.action == MotionEvent.ACTION_MOVE)
            ClientAuthActivity.human(ClientAuthActivity.owner)
        if (!ClientAuthActivity.check()) return super.dispatchGenericMotionEvent(event)
        if (gamepad?.motion(event) == true) return true
        gameMouse?.observe(event)
        return penInput?.dispatchGeneric(
            event,
            touch = { super.dispatchTouchEvent(it) },
            generic = { super.dispatchGenericMotionEvent(it) }
        ) ?: super.dispatchGenericMotionEvent(event)
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.flags and KeyEvent.FLAG_FALLBACK == 0) ClientAuthActivity.human(ClientAuthActivity.owner)
        if (!ClientAuthActivity.check()) return super.dispatchKeyEvent(event)
        // A game controller's keys go to the desktop's virtual controller, never the app's navigation.
        if (gamepad?.key(event) == true) return true
        reportImeVisibility()
        if (imeVisible() == true) return super.dispatchKeyEvent(event)
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 && AndroidTv.isTelevision(this)) {
            val hardware = AndroidTv.hardwareKeyboard(event.device)
            if ((hardware || AndroidTv.flutterNavigation(this, event)) && lastHardwareNavigation != hardware) {
                lastHardwareNavigation = hardware
                // Input class only; no typed keys, text or device identity.
                applicationChannel?.invokeMethod("hardwareNavigation", hardware)
            }
        }
        // A TV remote steers the Flutter pointer/UI. A real hardware keyboard
        // keeps the existing captured Windows-key path.
        if (AndroidTv.flutterNavigation(this, event)) return super.dispatchKeyEvent(event)
        if (gameMouse?.key(event) == true) return true
        if (media?.key(event) == true) return true
        return super.dispatchKeyEvent(event)
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) penInput?.clear()
        reportImeVisibility()
        if (!hasFocus) gameMouse?.background()
        if (!hasFocus) media?.releaseInput()
        // A controller held now would come back as held (its later moves go elsewhere).
        if (!hasFocus) gamepad?.released()
        // Android lets only the focused app read the clipboard: a copy made in
        // another app is noticed when the user comes back.
        clipboard?.focusChanged(hasFocus)
        // A fold, unfold or rotation can move the window to another display or
        // surface: the stream's refresh request is made again if it no longer holds.
        if (hasFocus) media?.refreshChanged()
        // The user back in the app asks back an audio focus another player took (AndroidAudioFocus).
        media?.windowFocus(hasFocus)
    }
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        televisionMode = AndroidTv.isTelevision(this)
        reportImeVisibility()
        media?.refreshChanged()
        keyboardChanged()
    }
    override fun onResume() {
        super.onResume()
        if (!ClientAuthActivity.check() && ClientAuthActivity.needsShield()) shieldAuthentication()
        sessionForeground = true
        // Flutter counts the app as in front only with a focused window, from what this activity
        // told it. Where a pause and resume came without a focus change of their own (a result
        // delivered to the resumed activity), what Flutter believes is set right from the window.
        if (hasWindowFocus()) flutterEngine?.lifecycleChannel?.aWindowIsFocused()
        drive?.resumed()
        reportImeVisibility()
        keyboardChanged()
        media?.refreshChanged()
    }
    // Visible or not (onPause alone is still visible: a dialog, split screen).
    override fun onStart() {
        super.onStart()
        media?.visible(true)
    }
    override fun onStop() {
        media?.visible(false)
        super.onStop()
    }
    override fun onPointerCaptureChanged(hasCapture: Boolean) {
        super.onPointerCaptureChanged(hasCapture)
        gameMouse?.captureChanged(hasCapture)
    }
    override fun onPause() {
        sessionForeground = false
        penInput?.clear()
        gameMouse?.background()
        media?.releaseInput()
        drive?.paused()
        super.onPause()
    }
    override fun onDestroy() {
        penInput?.dispose()
        penInput = null
        tvTextEditor?.dispose()
        tvTextEditor = null
        if (window.decorView.viewTreeObserver.isAlive) {
            window.decorView.viewTreeObserver.removeOnGlobalLayoutListener(imeLayoutListener)
            window.decorView.viewTreeObserver.removeOnPreDrawListener(imeDrawListener)
        }
        inputManager?.unregisterInputDeviceListener(inputListener)
        inputManager = null
        applicationChannel?.setMethodCallHandler(null)
        applicationChannel = null
        printer?.dispose()
        printer = null
        drive?.dispose()
        drive = null
        clipboard?.dispose()
        clipboard = null
        AndroidRemoteSessionService.detachDisconnectBridge(disconnectBridgeToken)
        disconnectBridgeToken = null
        sessionForeground = false
        sessionLease?.let { AndroidRemoteSessionService.stop(applicationContext, it) }
        sessionLease = null
        notificationReply?.success(false)
        notificationReply = null
        udp?.dispose()
        udp = null
        gameMouse?.dispose()
        gameMouse = null
        gamepad?.dispose()
        gamepad = null
        media?.dispose()
        media = null
        update?.dispose()
        update = null
        network?.dispose()
        network = null
        super.onDestroy()
    }
    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        // The folder picker of a shared folder (AndroidDrive); everything else is Flutter's.
        if (drive?.activityResult(requestCode, resultCode, data) == true) return
        super.onActivityResult(requestCode, resultCode, data)
    }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 7321) {
            notificationReply?.success(AndroidRemoteSessionService.notificationsVisible(this))
            notificationReply = null
        }
    }
}
