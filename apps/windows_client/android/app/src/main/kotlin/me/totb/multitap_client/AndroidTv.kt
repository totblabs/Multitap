package me.totb.multitap_client

import android.app.Activity
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.view.InputDevice
import android.view.KeyEvent

/** No model names or device identifiers are collected. */
object AndroidTvPolicy {
    fun hardwareKeyboard(alphabetic: Boolean, virtual: Boolean): Boolean = alphabetic && !virtual
    fun television(modeType: Int, leanback: Boolean): Boolean = modeType == 4 || leanback
    fun flutterNavigation(television: Boolean, alphabetic: Boolean, key: Int): Boolean =
        television && !alphabetic && (key in 19..23 || key == 66 || key == 82)
}

object AndroidTv {
    fun hardwareKeyboard(device: InputDevice?): Boolean = device != null &&
        AndroidTvPolicy.hardwareKeyboard(device.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC, device.isVirtual)

    fun hasHardwareKeyboard(): Boolean = InputDevice.getDeviceIds().any { hardwareKeyboard(InputDevice.getDevice(it)) }

    fun isTelevision(activity: Activity): Boolean = AndroidTvPolicy.television(
        activity.resources.configuration.uiMode and Configuration.UI_MODE_TYPE_MASK,
        activity.packageManager.hasSystemFeature(PackageManager.FEATURE_LEANBACK),
    )

    fun flutterNavigation(activity: Activity, event: KeyEvent): Boolean =
        AndroidTvPolicy.flutterNavigation(
            isTelevision(activity),
            hardwareKeyboard(event.device),
            event.keyCode,
        )
}
