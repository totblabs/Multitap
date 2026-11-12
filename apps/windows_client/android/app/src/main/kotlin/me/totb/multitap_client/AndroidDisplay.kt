package me.totb.multitap_client

import android.app.Activity
import android.os.Build
import android.util.DisplayMetrics
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/**
 * The current display's physical pixel density and full size, for the session's 자동 화면 배율
 * (android_screen.dart). Android's density is a bucket (2, 2.625, 3 …); xdpi/ydpi are the
 * panel's measured pixels per inch, which a 2560×1600 tablet and a 1920×1200 one in the same
 * bucket do not share. Read again after folding, unfolding or rotating (the activity's
 * resources follow the display it is on). Nothing here identifies the device.
 */
class AndroidDisplay(private val activity: Activity, engine: FlutterEngine) {
    init {
        MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/display").setMethodCallHandler { call, result ->
            when (call.method) {
                "metrics" -> try { result.success(metrics()) } catch (_: Exception) { result.success(null) }
                else -> result.notImplemented()
            }
        }
    }

    private fun metrics(): Map<String, Any> {
        val metrics = activity.resources.displayMetrics
        var width = metrics.widthPixels
        var height = metrics.heightPixels
        if (Build.VERSION.SDK_INT >= 30) {
            // The whole display, not this window (split screen keeps the panel's size).
            val bounds = activity.windowManager.maximumWindowMetrics.bounds
            width = bounds.width(); height = bounds.height()
        } else {
            val real = DisplayMetrics()
            @Suppress("DEPRECATION") activity.windowManager.defaultDisplay.getRealMetrics(real)
            width = real.widthPixels; height = real.heightPixels
        }
        return mapOf("xdpi" to metrics.xdpi.toDouble(), "ydpi" to metrics.ydpi.toDouble(),
            "width" to width.toDouble(), "height" to height.toDouble())
    }
}
