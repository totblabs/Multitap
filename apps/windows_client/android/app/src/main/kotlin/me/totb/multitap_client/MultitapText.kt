package me.totb.multitap_client

import android.content.Context
import android.content.res.Configuration
import java.util.Locale

/**
 * Native text (notifications, Console, folder places, TV input) in the app's
 * own language: the "언어 / Language" choice the Dart side stores as
 * `pref:appearance.language` (AndroidStore), else the device's language.
 * The words live in res/values (English) and res/values-ko (Korean).
 */
internal object MultitapText {
    fun language(context: Context): String? =
        context.getSharedPreferences("multitap", Context.MODE_PRIVATE)
            .getString("pref:appearance.language", null)
            ?.takeIf { it == "ko" || it == "en" }

    /** [context] with the chosen language's resources (the device's for auto). */
    fun localized(context: Context): Context {
        val chosen = language(context) ?: return context
        val config = Configuration(context.resources.configuration)
        config.setLocale(Locale.forLanguageTag(chosen))
        return context.createConfigurationContext(config)
    }

    fun get(context: Context, id: Int, vararg args: Any): String =
        localized(context).getString(id, *args)
}
